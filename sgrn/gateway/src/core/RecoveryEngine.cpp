#include <sgrn/debug.hpp>
#include <sgrn/gateway/core/RecoveryEngine.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/core.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>

namespace sgrn::gateway::core
{

namespace
{
namespace fs = std::filesystem;
using sgrn::gateway::twin::DbEntry;
using sgrn::gateway::twin::PlcNode;
using sgrn::gateway::twin::PlcState;
using sgrn::gateway::twin::TreePath;
using sgrn::scl::DbField;
using sgrn::scl::PlcSchemaStore;

// ── Candidate archive discovery ──────────────────────────────────────────────

struct ArchiveCandidate {
    fs::path path;
    std::string sort_key; // "<date>/<end_time>" — newest archive first
};

bool isWALArchive(const fs::path& t_path) {
    const std::string name = t_path.filename().string();
    return name.size() > 9 && (name.ends_with(".jsonl.zst") || name.ends_with(".jsonl.zst.tmp"));
}

// Filename shape: <start_time>-<end_time>.jsonl.zst[.tmp], both time parts use
// ':' (never '-'), so the basename splits on exactly one '-'
// (the <start>-<end> separator).
std::string archiveSortKey(const fs::path& t_path) {
    std::string name = t_path.filename().string();
    if (name.ends_with(".tmp"))
        name.resize(name.size() - 4);
    if (name.ends_with(".jsonl.zst"))
        name.resize(name.size() - 10);
    const size_t dash = name.find('-');
    if (dash == std::string::npos)
        return name; // unparseable — still listed, sorts last
    const std::string end_time = name.substr(dash + 1);
    const fs::path parent = t_path.parent_path();
    return parent.filename().string() + "/" + end_time;
}

std::vector<ArchiveCandidate> collectCandidates(const std::string& t_state_dir) {
    std::vector<ArchiveCandidate> out;
    for (const char* sub : {"unsynced", "synced"}) {
        fs::path root = fs::path(t_state_dir) / sub;
        std::error_code ec;
        if (!fs::exists(root, ec))
            continue;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            const fs::path p = it->path();
            if (!isWALArchive(p))
                continue;
            out.push_back(ArchiveCandidate{p, archiveSortKey(p)});
        }
    }
    std::sort(out.begin(), out.end(), [](const ArchiveCandidate& a, const ArchiveCandidate& b) { return a.sort_key > b.sort_key; });
    return out;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

std::string serializeCompact(const rapidjson::Value& t_value) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    t_value.Accept(w);
    return sb.GetString();
}

// ── State application (mirrors TreePersistenceStore::load, recursively) ─────

DbField makeDbField(const PlcNode& t_node) {
    DbField field;
    field.name = t_node.name_;
    field.type = t_node.type_;
    field.count = static_cast<int>(t_node.count_);
    field.bit_index = static_cast<int>(t_node.bit_index_);
    field.endianness = t_node.endian_;
    field.offset = 0;
    return field;
}

// Encodes one JSON value into one leaf slot under the segment lock. Returns how
// many leaves were restored (1) or skipped (0).
int applyLeaf(PlcState& t_state, const std::string& t_path, const PlcNode* t_node, const rapidjson::Value& t_value, int& t_skipped) {
    if (!t_node || !t_node->cached_slot_ || !t_node->children_.empty()) {
        ++t_skipped;
        return 0;
    }
    auto* p_entry = const_cast<DbEntry*>(t_node->cached_slot_);
    const size_t node_start = t_node->offset_;
    uint8_t* p_ptr = t_state.arenaData() + p_entry->offset + node_start;
    const size_t buf_remaining = p_entry->size - node_start;
    const DbField field = makeDbField(*t_node);
    {
        std::unique_lock<std::shared_mutex> lk(p_entry->mutex_);
        auto res = twin::encodeFieldRapidJson(field, t_value, p_ptr, buf_remaining, 0, t_node->endian_);
        if (res.hasError()) {
            ++t_skipped;
            return 0;
        }
    }
    // Silently bump the node version so TreeCacheEngine treats this path as
    // stale. No dirty flag, no event publish — adapters haven't started yet.
    t_state.incrementNodeVersion(TreePath::fromDotted(t_path));
    return 1;
}

// Walks a PlcNode subtree and applies the matching JSON subtree (struct members
// are matched by child name). Arrays are handled inside encodeFieldRapidJson via
// the field's count.
int applyValueToNode(PlcState& t_state, const std::string& t_path, const PlcNode* t_node, const rapidjson::Value& t_value, int& t_skipped) {
    if (!t_node || !t_node->cached_slot_) {
        ++t_skipped;
        return 0;
    }
    if (t_node->children_.empty()) {
        return applyLeaf(t_state, t_path, t_node, t_value, t_skipped);
    }
    // Struct/DB node: value must be an object keyed by field name.
    if (!t_value.IsObject()) {
        ++t_skipped;
        return 0;
    }
    int restored = 0;
    for (const auto& child : t_node->children_) {
        auto it = t_value.FindMember(child.name_.c_str());
        if (it == t_value.MemberEnd()) {
            ++t_skipped; // schema drift: stored value lacks this field
            continue;
        }
        const std::string child_path = t_path.empty() ? child.name_ : t_path + "." + child.name_;
        const PlcNode* p_child = t_state.find(child_path);
        restored += applyValueToNode(t_state, child_path, p_child, it->value, t_skipped);
    }
    return restored;
}

// Applies one line's payload: an "anchor" line exposes .data (DB-object map),
// a "delta" line exposes .changes (flat dotted-path map). Both recurse into
// whatever node structure exists.
//
// After Phase 1, new anchors are written as flat id-keyed maps and expanded
// by expandRecordKeys() into flat dotted-path maps (same shape as deltas).
// Old anchors remain genuinely nested. This function decides nested-vs-flat
// by inspecting the payload's own keys rather than trusting the line type,
// so both formats replay correctly without an archive version flag.
int applyPayload(PlcState& t_state, const rapidjson::Value& t_root, const std::vector<std::string>& t_path_by_id) {
    int skipped = 0;
    const rapidjson::Value* payload = nullptr;
    if (t_root.HasMember("data") && t_root["data"].IsObject())
        payload = &t_root["data"];
    else if (t_root.HasMember("changes") && t_root["changes"].IsObject())
        payload = &t_root["changes"];

    if (!payload) {
        SGRN_WARN_LOG("Recovery: unsupported line type, skipping");
        return 0;
    }

    int restored = 0;

    if (payload->MemberBegin() == payload->MemberEnd())
        return 0;

    const std::string first_key = payload->MemberBegin()->name.GetString();
    bool is_numeric = std::all_of(first_key.begin(), first_key.end(), ::isdigit);

    if (is_numeric) {
        // Flat form (dictionary encoded): dotted-path map { "123": <value>, … }
        for (auto it = payload->MemberBegin(); it != payload->MemberEnd(); ++it) {
            twin::LeafId id = static_cast<twin::LeafId>(std::stoul(it->name.GetString()));
            if (id < t_path_by_id.size()) {
                const PlcNode* p_node = t_state.find(t_path_by_id[id]);
                restored += applyValueToNode(t_state, t_path_by_id[id], p_node, it->value, skipped);
            } else {
                ++skipped;
            }
        }
        return restored;
    }

    const PlcNode* first_node = t_state.find(first_key);
    const bool is_flat = first_node && first_node->children_.empty();

    if (is_flat) {
        // Flat form: dotted-path map { "DB10.field": <value>, … }
        for (auto it = payload->MemberBegin(); it != payload->MemberEnd(); ++it) {
            const std::string path = it->name.GetString();
            const PlcNode* p_node = t_state.find(path);
            restored += applyValueToNode(t_state, path, p_node, it->value, skipped);
        }
        return restored;
    }
    // Nested form: DB-object map { "DB10": { … }, … }
    for (auto it = payload->MemberBegin(); it != payload->MemberEnd(); ++it) {
        const std::string name = it->name.GetString();
        const PlcNode* p_node = t_state.find(name);
        restored += applyValueToNode(t_state, name, p_node, it->value, skipped);
    }
    return restored;
}

// ── Archive replay ────────────────────────────────────────────────────────────

struct ReplayOutcome {
    int restored{0};
    int skipped{0};
    int64_t last_anchor_line{0}; ///< 0 = no anchor found in this archive
    bool schema_ok{false};       ///< line 1 matched the live schema (or was null)
    bool found_footer{false};
    std::vector<std::string> path_by_id;
};

// Pass 1: stream the archive once, checking the schema line and learning the
// footer's last_anchor_line. Returns the outcome; never applies data.
ReplayOutcome scanArchive(
    const fs::path& t_path, sgrn::utils::compression::ZstdLineReader& t_reader, const PlcSchemaStore& t_schema_store) {
    ReplayOutcome out;
    std::string line;
    size_t line_no = 0;
    const std::string live_schema_json = t_schema_store.toJson();

    while (t_reader.readLine(line)) {
        ++line_no;
        rapidjson::Document doc;
        doc.Parse(line.c_str());
        if (doc.HasParseError() || !doc.IsObject())
            continue; // tolerate a malformed record, keep scanning for the footer

        const char* type = doc.HasMember("type") && doc["type"].IsString() ? doc["type"].GetString() : "";
        if (line_no == 1 && std::string_view(type) == "schema") {
            if (!doc.HasMember("schema") || doc["schema"].IsNull()) {
                out.schema_ok = true; // no stored schema => nothing to validate against
            } else if (doc["schema"].IsObject() && !live_schema_json.empty()) {
                out.schema_ok = (serializeCompact(doc["schema"]) == live_schema_json);
            } else {
                out.schema_ok = false;
            }
        } else if (std::string_view(type) == "dictionary") {
            if (doc.HasMember("leaves") && doc["leaves"].IsArray()) {
                const auto& leaves = doc["leaves"];
                for (rapidjson::SizeType i = 0; i < leaves.Size(); ++i) {
                    const auto& item = leaves[i];
                    if (item.IsObject() && item.HasMember("id") && item["id"].IsUint() && item.HasMember("path") &&
                        item["path"].IsString()) {
                        auto id = item["id"].GetUint();
                        if (id >= out.path_by_id.size())
                            out.path_by_id.resize(id + 1);
                        out.path_by_id[id] = item["path"].GetString();
                    }
                }
            }
        } else if (std::string_view(type) == "anchor") {
            out.last_anchor_line = static_cast<int64_t>(line_no);
        } else if (std::string_view(type) == "footer") {
            out.found_footer = true;
            if (doc.HasMember("last_anchor_line") && doc["last_anchor_line"].IsInt64())
                out.last_anchor_line = doc["last_anchor_line"].GetInt64();
        }
    }
    return out;
}

// Pass 2: replay a scanned archive. Reads forward to last_anchor_line, decodes
// the anchor as the base state, then applies every delta line after it.
ReplayOutcome replayArchive(
    const fs::path& t_path, PlcState& t_state, int64_t t_last_anchor_line, const std::vector<std::string>& t_path_by_id) {
    sgrn::utils::compression::ZstdLineReader reader(t_path);
    if (!reader.ok())
        return ReplayOutcome{}; // caller already warned during pass 1

    ReplayOutcome out;
    std::string line;
    while (reader.readLine(line)) {
        if (reader.lineIndex() <= static_cast<size_t>(t_last_anchor_line)) {
            // Base-state region: only the anchor line applies; the schema and
            // manifest lines carry no twin data.
            rapidjson::Document doc;
            doc.Parse(line.c_str());
            if (doc.IsObject() && doc.HasMember("type") && doc["type"].IsString() &&
                std::string_view(doc["type"].GetString()) == "anchor") {
                out.restored += applyPayload(t_state, doc, t_path_by_id);
            }
            continue;
        }
        rapidjson::Document doc;
        doc.Parse(line.c_str());
        if (doc.HasParseError() || !doc.IsObject())
            continue;

        out.restored += applyPayload(t_state, doc, t_path_by_id);
    }
    out.schema_ok = true;
    out.last_anchor_line = t_last_anchor_line;
    return out;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// recoverStateFromArchives
// ─────────────────────────────────────────────────────────────────────────────

sgrn::Result<RecoveryResult, std::string> recoverStateFromArchives(
    const std::string& t_state_dir, PlcState& t_state, const PlcSchemaStore& t_schema_store) {

    RecoveryResult result;
    const auto candidates = collectCandidates(t_state_dir);

    for (const auto& cand : candidates) {
        sgrn::utils::compression::ZstdLineReader reader(cand.path);
        if (!reader.ok()) {
            SGRN_WARN_LOG("Recovery: skipping unreadable archive {}: {}", cand.path.string(), reader.errorMessage());
            continue;
        }

        ReplayOutcome scan = scanArchive(cand.path, reader, t_schema_store);
        if (!scan.schema_ok) {
            SGRN_WARN_LOG("Recovery: schema mismatch in archive {}, trying older archive", cand.path.string());
            continue;
        }

        if (!scan.found_footer) {
            SGRN_WARN_LOG("Recovery: archive {} has no footer (truncated?) — falling back to full-line scan", cand.path.string());
        }

        // Second pass: seek forward (reader is forward-only) to the anchor line,
        // decode it as the base state, then replay the deltas after it.
        ReplayOutcome replay = replayArchive(cand.path, t_state, scan.last_anchor_line, scan.path_by_id);
        result.leaves_restored += replay.restored;
        result.leaves_skipped += replay.skipped;
        result.archive_used = cand.path.string();

        // Bump every DB-root version so TreeCacheEngine rebuilds the tree
        // instead of serving a pre-recovery cached JSON blob.
        for (const auto& name_ : t_state.getTopLevelNames()) {
            t_state.incrementNodeVersion(TreePath::fromDotted(name_));
        }
        return result;
    }

    // No usable archive: leave the twin untouched and report it as file-not-found
    // so initTwin() can log "starting fresh".
    if (candidates.empty()) {
        return Error("no .jsonl.zst archives found under " + t_state_dir);
    }
    return Error("no archive matched the current schema under " + t_state_dir);
}

} // namespace sgrn::gateway::core
