#pragma once
// REVAMP-1: PlcClient.hpp — renamed from SemanticAwareS7Client.hpp

#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::gateway::adapters::s7
{
using ::sgrn::scl::DbSchema;
using ::sgrn::scl::FieldTarget;
using ::sgrn::scl::PlcSchemaStore;

/**
 * @brief Symbol-aware S7 client built on top of the raw Snap7 transport wrapper.
 *
 * Renamed from SemanticAwareS7Client. This class holds a NON-OWNING pointer to
 * a PlcSchemaStore (Section 3.1). Schema lookups are delegated to the store;
 * no local schema copy is maintained. rebuildCache() is eliminated (DEL-3).
 */
class PlcClient : public ::sgrn::gateway::wrappers::s7::S7Client { // REVAMP-2: renamed from SemanticAwareS7Client
public:
    PlcClient() = default;

    PlcClient(const PlcClient&) = delete;
    PlcClient& operator=(const PlcClient&) = delete;
    PlcClient(PlcClient&&) = delete;
    PlcClient& operator=(PlcClient&&) = delete;

    // ── Schema attachment (non-owning) ────────────────────────────────────────

    /**
     * @brief Attach a schema store. PlcClient does NOT take ownership.
     *
     * The PlcSchemaStore must outlive this PlcClient. (Section 3.1)
     * Replaces loadRegistry(S7SemanticRegistry) — no copy, no rebuild. (DEL-3)
     */
    void attachSchema(const PlcSchemaStore& t_store) { // REVAMP-3: non-owning attach
        schema_ = &t_store;
    }

    void detachSchema() {
        schema_ = nullptr;
    } // REVAMP-4

    void clearRegistry() {
        detachSchema();
    } // REVAMP-5: compat alias

    bool hasSchema() const {
        return schema_ != nullptr;
    } // REVAMP-6: renamed from hasRegistry()

    // ── Schema delegation ─────────────────────────────────────────────────────

    const PlcSchemaStore* schemaStore() const {
        return schema_;
    } // REVAMP-7

    /**
     * @brief Get the schema for a DB by number.
     * @return nullptr if schema not attached or DB not found.
     */
    const DbSchema* getDbSchema(uint16_t t_db_number) const { // REVAMP-8
        if (!schema_)
            return nullptr;
        auto r = schema_->getDb(t_db_number);
        return r.hasError() ? nullptr : r.value();
    }

    std::vector<uint16_t> availableDbs() const { // REVAMP-9
        if (!schema_)
            return {};
        return schema_->availableDbs();
    }

    std::vector<std::string> availableDbNames() const { // REVAMP-10
        if (!schema_)
            return {};
        return schema_->availableDbNames();
    }

    /**
     * @brief Resolve a DB reference ("DB10", "10", "Configuration") → db_number.
     * Delegates to PlcSchemaStore::resolveDbRef (DEL-4).
     */
    std::optional<uint16_t> resolveDbRef(std::string_view t_token) const { // REVAMP-11
        if (!schema_)
            return std::nullopt;
        return schema_->resolveDbRef(t_token);
    }

    /**
     * @brief Parse "Configuration.SetPoint" → {db_number, field_path}.
     * Delegates to PlcSchemaStore::parseFieldTarget (DEL-4).
     */
    std::optional<FieldTarget> parseFieldTarget(std::string_view t_target) const { // REVAMP-12
        if (!schema_)
            return std::nullopt;
        return schema_->parseFieldTarget(t_target);
    }

    // REVAMP-13: rebuildCache() DELETED (DEL-3) — no local cache to rebuild

private:
    const PlcSchemaStore* schema_{nullptr}; // REVAMP-14: non-owning (Section 3.1)
    // REVAMP-15: loaded_dbs_, db_name_to_number_ DELETED — schema_ handles all lookups
};

// ---------------------------------------------------------------------------
// Backward-compat alias
// ---------------------------------------------------------------------------
} // namespace sgrn::gateway::adapters::s7
