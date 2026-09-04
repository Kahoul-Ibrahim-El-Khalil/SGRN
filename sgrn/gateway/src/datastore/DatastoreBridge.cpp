#include <fmt/color.h>
#include <fmt/core.h>

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/datastore/DatastoreBridge.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/time.hpp>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::gateway::backend
{

using namespace sgrn::gateway::core;

using sgrn::gateway::database::GatewayDatabaseSptr;

DatastoreBridge::~DatastoreBridge() {
    this->stop();
}

sgrn::Result<void> DatastoreBridge::configure(const DatastoreConnectionConfig& t_cfg, const std::string& t_state_dir,
    const std::string& t_registry_source, GatewayDatabaseSptr tsp_db) {
    cfg_ = t_cfg;
    state_dir_ = t_state_dir;
    unsynced_dir_ = t_state_dir + "/unsynced";
    synced_dir_ = t_state_dir + "/synced";
    registry_source_ = t_registry_source;
    node_db_ = tsp_db;

    try {
        std::filesystem::create_directories(unsynced_dir_);
        std::filesystem::create_directories(synced_dir_);
    } catch (const std::exception& e) {
        return fmt::format("Failed to create bridge directories: {}", e.what());
    }

    if (!t_cfg.isConfigured()) {
        fmt::print(fg(fmt::color::yellow), "[backend] No credentials — offline mode.\n");
        if (!t_cfg.offline_persistence) {
            return {};
        }
        fmt::print(fg(fmt::color::cyan), "[backend] Offline persistence enabled.\n");
    }

    // Start uploader immediately; it will handle background connection
    if (t_cfg.isConfigured()) {
        startUploader();
    }

    // ── ARCHITECTURAL NOTE ───────────────────────────────────────────────────
    // DatastoreBridge does NOT subscribe to TelemetryBroker.
    // PersistenceService is the sole writer of .json.zst files to unsynced/.
    // DatastoreBridge only polls the database and uploads existing files.
    // ─────────────────────────────────────────────────────────────────────────

    return {};
}

void DatastoreBridge::startUploader() {
    if (uploader_running_)
        return;
    uploader_running_ = true;
    scheduleUploaderTick();
}

void DatastoreBridge::stop() {
    uploader_running_ = false;
    stopClient();
}

void DatastoreBridge::stopClient() {
    if (client_) {
        session_ready_ = false;
        client_.reset();
    }
}

bool DatastoreBridge::enabled() const {
    return session_ready_ && cfg_.telemetry_enabled;
}

bool DatastoreBridge::reconnectDue() const {
    return cfg_.isConfigured() && !session_ready_ && std::chrono::steady_clock::now() >= next_reconnect_;
}

void DatastoreBridge::scheduleUploaderTick() {
    if (!uploader_running_)
        return;

    // Post to the global light io_context
    asio::post(GlobalContext::instance().io_context(), [this]() {
        if (session_ready_) {
            processPendingBatches(); // still light work (checking DB, etc.)
        } else if (reconnectDue()) {
            tryConnect(); // may do network handshake
        }

        if (uploader_running_) {
            // Chain the next tick using a timer on the same light context
            asio::post(GlobalContext::instance().io_context(), [this]() {
                uint16_t delay_s = session_ready_ ? 15 : 30;
                auto timer = std::make_shared<asio::steady_timer>(GlobalContext::instance().io_context(), std::chrono::seconds(delay_s));
                timer->async_wait([this, timer](const asio::error_code& t_ec) {
                    if (!t_ec && uploader_running_) {
                        scheduleUploaderTick();
                    }
                });
            });
        }
    });
}

void DatastoreBridge::tryConnect() {
    if (connecting_ || session_ready_ || !uploader_running_ || !cfg_.isConfigured())
        return;

    connecting_ = true;
    asio::post(GlobalContext::instance().io_context(), [this]() {
        try {
            // Initialize the SGRN SDK client if needed.
            if (!client_) {
                sgrn::sdk::SgrnClientConfig sdk;
                sdk.backend_url_ = cfg_.url;
                sdk.auth_mode_ = sgrn::sdk::AuthMode::AutomatedService;
                sdk.public_token_ = cfg_.public_token;
                sdk.private_token_ = cfg_.private_token;
                sdk.compress_zstd_ = true;
                sdk.auto_decompress_zstd_ = false;
                sdk.telemetry_timeout_s_ = 5.0;
                client_ = std::make_unique<sgrn::sdk::SgrnClient>(std::move(sdk));
            }

            // Perform Backend Handshake
            if (client_->hasSessionToken() || client_->signIn()) {
                session_ready_ = true;
                cfg_reconnect_ms_ = reconnect_ms_base_; // reset backoff to 5s
                fmt::print(fg(fmt::color::green), "[backend] Session established with {}\n", cfg_.url);
            } else {
                scheduleReconnect();
            }
        } catch (const std::exception& e) {
            fmt::print(fg(fmt::color::red), "[backend] Exception in tryConnect: {}\n", e.what());
            scheduleReconnect();
        }
        connecting_ = false;
    });
}

void DatastoreBridge::setReconnectBase(int t_ms) {
    reconnect_ms_base_ = t_ms;
    cfg_reconnect_ms_ = t_ms;
}

void DatastoreBridge::updateConfig(const DatastoreConnectionConfig& t_cfg) {
    cfg_ = t_cfg;
}

// ── NOTE: DatastoreBridge no longer writes files ─────────────────────────────
// All file writing is handled by PersistenceService.
// DatastoreBridge only uploads files that PersistenceService has already written.
// ──────────────────────────────────────────────────────────────────────────

void DatastoreBridge::scheduleReconnect() {
    next_reconnect_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_reconnect_ms_);
    cfg_reconnect_ms_ = std::min(cfg_reconnect_ms_ * 2, 60000);
}

// ── NOTE: All file writing removed ──────────────────────────────────────────
// PersistenceService handles all file writing and compression.
// DatastoreBridge only uploads files that already exist in unsynced/.
// ──────────────────────────────────────────────────────────────────────────

void DatastoreBridge::processPendingBatches() {
    std::vector<sgrn::gateway::database::BackendBatchRecord> pending;
    if (node_db_) {
        auto res = node_db_->getPendingBatches(10);
        if (res.hasError()) {
            fmt::print(fg(fmt::color::red), "[backend] Failed to get pending batches: {}\n", res.error());
            return;
        }
        pending = std::move(res.value());
    }

    for (const auto& batch : pending) {
        int t_id = batch.id;
        std::string t_file_path = batch.file_path;
        if (!uploader_running_)
            break;

        if (!std::filesystem::exists(t_file_path)) {
            markSynced(t_id, t_file_path);
            continue;
        }

        bool success = uploadRaw(t_file_path);

        if (success) {
            moveToSynced(t_file_path);
            markSynced(t_id, t_file_path);
        }
    }

    cleanOldSyncedFiles();
}

void DatastoreBridge::cleanOldSyncedFiles() {
    if (node_db_) {
        (void)node_db_->purgeSyncedBatches();
    }

    if (synced_dir_.empty() || !std::filesystem::exists(synced_dir_))
        return;

    try {
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(synced_dir_)) {
            if (entry.is_regular_file()) {
                auto age = std::chrono::duration_cast<std::chrono::minutes>(now - entry.last_write_time());
                // Remove synced files older than 5 minutes to free disk space
                if (age.count() >= 5) {
                    std::error_code ec;
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
    } catch (...) {
        // Ignore filesystem iteration errors during pruning
    }
}

bool DatastoreBridge::uploadRaw(const std::string& t_file_path) {
    try {
        const std::string remote_dir = cfg_.getEffectiveVfsRemoteDir();
        const std::filesystem::path p(t_file_path);
        const std::string relative = std::filesystem::relative(p, unsynced_dir_).string();
        const std::string remote = remote_dir + "/" + relative;
        fmt::print(fg(fmt::color::cyan), "[backend] Raw upload → {}\n", remote);

        bool ok = client_->storage().upload(remote, t_file_path);
        if (!ok) {
            // Upload returned false; could be server error or duplicate key constraint.
            // Mark as ok to move on if upload failed cleanly with server response (e.g. duplicate file record already stored).
            fmt::print(fg(fmt::color::yellow),
                "[backend] Upload returned false for {} — marking as processed to avoid hammering backend.\n", remote);
            ok = true;
        } else {
            fmt::print(fg(fmt::color::green), "[backend] Raw upload successful for {}\n", remote);
        }
        return ok;
    } catch (const std::exception& e) {
        std::string err = e.what();
        if (err.find("duplicate key value") != std::string::npos || err.find("already exists") != std::string::npos) {
            fmt::print(fg(fmt::color::yellow), "[backend] File {} already exists in datastore DB — marking as synced.\n", t_file_path);
            return true;
        }
        fmt::print(fg(fmt::color::red), "[backend] Raw upload threw an exception: {}\n", err);
        return false;
    }
}

void DatastoreBridge::moveToSynced(const std::string& t_file_path) {
    try {
        const std::filesystem::path p(t_file_path);
        const std::string relative = std::filesystem::relative(p, unsynced_dir_).string();
        const std::string target = synced_dir_ + "/" + relative;

        std::filesystem::create_directories(std::filesystem::path(target).parent_path());
        std::filesystem::rename(t_file_path, target);
    } catch (const std::exception& e) {
        fmt::print(fg(fmt::color::red), "[backend] Failed to move {} to synced: {}\n", t_file_path, e.what());
    }
}

void DatastoreBridge::markSynced(int t_id, const std::string& t_file_path) {
    if (node_db_) {
        (void)node_db_->markBatchSynced(t_id);
    }
}

void DatastoreBridge::uploadLogArchive(const std::string& t_file_path) {
    if (!client_) {
        return;
    }

    asio::post(GlobalContext::instance().io_context(), [this, t_file_path]() {
        if (!client_ || !session_ready_.load()) {
            return;
        }
        try {
            const std::filesystem::path p(t_file_path);
            const std::string remote = cfg_.getEffectiveVfsRemoteDir() + "/logs/" + p.filename().string();
            fmt::print(fg(fmt::color::cyan), "[backend] Log archival upload → {}\n", remote);

            bool ok = client_->storage().upload(remote, t_file_path);
            if (ok) {
                fmt::print(fg(fmt::color::green), "[backend] Log archival successful: {}\n", remote);
                std::filesystem::remove(t_file_path);
            } else {
                fmt::print(fg(fmt::color::red), "[backend] Log archival failed for {}\n", remote);
            }
        } catch (const std::exception& e) {
            fmt::print(fg(fmt::color::red), "[backend] Log archival exception: {}\n", e.what());
        }
    });
}

} // namespace sgrn::gateway::backend
