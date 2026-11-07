#pragma once

#include "sgrn/utils/crypto/dek.hpp"
#include <sgrn/Result.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sgrn::utils::crypto
{

enum class KeyStatus { PENDING, ACTIVE, SHREDDED };

[[nodiscard]] std::string keyStatusToString(KeyStatus t_status);
[[nodiscard]] ::sgrn::Result<KeyStatus> keyStatusFromString(std::string_view t_str);

struct KeyRecord {
    std::string key_id;
    std::optional<std::string> file_id;
    std::string object_key;
    KeyStatus status{KeyStatus::PENDING};
    std::string wrap_scheme{"none"}; // e.g. "none", "AES_KW", "KMS"
    std::optional<std::string> kek_ref;
    std::string algorithm{"AES-256-GCM"};
    uint32_t chunk_size{1024 * 1024};
    Dek dek;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> activated_at;
    std::optional<std::chrono::system_clock::time_point> shredded_at;

    KeyRecord() = default;
    ~KeyRecord() = default;

    // Move-only because Dek is move-only
    KeyRecord(const KeyRecord&) = delete;
    KeyRecord& operator=(const KeyRecord&) = delete;

    KeyRecord(KeyRecord&&) noexcept = default;
    KeyRecord& operator=(KeyRecord&&) noexcept = default;

    /// Create a clone of the key record
    [[nodiscard]] ::sgrn::Result<KeyRecord> clone() const;
};

/**
 * @brief Abstract interface for object key lifecycle management.
 */
class KeyProvider {
public:
    virtual ~KeyProvider() = default;

    /// Create a new key record in PENDING state bound to an optional object_key
    virtual ::sgrn::Result<KeyRecord> createPending(const std::string& t_object_key = "", const std::string& t_wrap_scheme = "none") = 0;

    /// Activate a key and bind it to a file_id
    virtual ::sgrn::Result<void> activate(const std::string& t_key_id, const std::string& t_file_id) = 0;

    /// Retrieve an existing key record (fails if shredded or not found)
    virtual ::sgrn::Result<KeyRecord> get(const std::string& t_key_id) = 0;

    /// Permanently shred a key: zeroes out DEK material and marks status SHREDDED
    virtual ::sgrn::Result<void> shred(const std::string& t_key_id) = 0;

    /// List all keys for reconciliation (returns cloned key records)
    virtual ::sgrn::Result<std::vector<KeyRecord>> listForReconcile() = 0;
};

/**
 * @brief Thread-safe in-memory key provider for unit tests and local execution.
 */
class InMemoryKeyProvider final : public KeyProvider {
public:
    InMemoryKeyProvider() = default;
    ~InMemoryKeyProvider() override;

    ::sgrn::Result<KeyRecord> createPending(const std::string& t_object_key = "", const std::string& t_wrap_scheme = "none") override;
    ::sgrn::Result<void> activate(const std::string& t_key_id, const std::string& t_file_id) override;
    ::sgrn::Result<KeyRecord> get(const std::string& t_key_id) override;
    ::sgrn::Result<void> shred(const std::string& t_key_id) override;
    ::sgrn::Result<std::vector<KeyRecord>> listForReconcile() override;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, KeyRecord> m_keys;
};

} // namespace sgrn::utils::crypto
