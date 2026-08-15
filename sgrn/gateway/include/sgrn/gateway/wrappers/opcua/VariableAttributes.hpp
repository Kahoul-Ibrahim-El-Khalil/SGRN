#pragma once

#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>

#include <open62541/server.h>
#include <open62541/types.h>
#include <open62541/types_generated.h>

#include <cstdint>
#include <string>

namespace sgrn::gateway::wrappers::opcua
{

/// Fluent builder over UA_VariableAttributes.
/// build() returns a UA_VariableAttributes by value — stack-allocated,
/// no UA_clear needed. Strings are kept alive in this builder for the
/// duration of the call; pass the result of build() immediately to the
/// open62541 AddNode call and do not store it separately.
class VariableAttributes {
public:
    VariableAttributes() noexcept {
        attrs_ = UA_VariableAttributes_default;
    }

    // No destructor needed — UA_VariableAttributes holds non-owning string
    // references when constructed this way; the std::string members below
    // own the storage.

    // ── Fluent setters ───────────────────────────────────────────────────────

    VariableAttributes& displayName(std::string_view t_name, std::string_view t_locale = "en") {
        display_name_ = t_name;
        locale_ = t_locale;
        attrs_.displayName = UA_LOCALIZEDTEXT(const_cast<char*>(locale_.c_str()), const_cast<char*>(display_name_.c_str()));
        return *this;
    }

    VariableAttributes& description(std::string_view t_desc, std::string_view t_locale = "en") {
        description_ = t_desc;
        desc_locale_ = t_locale;
        attrs_.description = UA_LOCALIZEDTEXT(const_cast<char*>(desc_locale_.c_str()), const_cast<char*>(description_.c_str()));
        return *this;
    }

    /// Set data type by UA_TYPES_* index (e.g. UA_TYPES_DOUBLE).
    VariableAttributes& dataType(int t_ua_types_index) noexcept {
        attrs_.dataType = UA_TYPES[t_ua_types_index].typeId;
        return *this;
    }

    /// Set data type by NodeId (e.g. for custom UDTs).
    VariableAttributes& dataType(const NodeId& t_type_node_id) noexcept {
        // UA_NodeId is value-copied into the stack struct — safe because
        // UA_VariableAttributes_default initialises dataType to UA_NODEID_NULL
        // and open62541's AddNode call reads it before this builder is destroyed.
        attrs_.dataType = t_type_node_id.get();
        return *this;
    }

    /// Bitfield: UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE etc.
    VariableAttributes& accessLevel(uint8_t t_mask) noexcept {
        attrs_.accessLevel = t_mask;
        return *this;
    }

    /// Sets a 1-D array with the given element count.
    VariableAttributes& arrayDimensions(uint32_t t_count) noexcept {
        array_dim_ = t_count;
        attrs_.valueRank = 1;
        attrs_.arrayDimensions = &array_dim_;
        attrs_.arrayDimensionsSize = 1;
        return *this;
    }

    // ── Build ────────────────────────────────────────────────────────────────

    /// Returns the underlying UA_VariableAttributes by value.
    /// The returned struct holds pointers into this builder's string members —
    /// use it immediately in an open62541 AddNode call; do not outlive this builder.
    UA_VariableAttributes build() const noexcept {
        return attrs_;
    }

private:
    UA_VariableAttributes attrs_{};

    // String storage — open62541 localised text holds non-owning char* pointers
    std::string display_name_;
    std::string locale_;
    std::string description_;
    std::string desc_locale_;

    // Array dimension storage
    UA_UInt32 array_dim_{0};
};

} // namespace sgrn::gateway::wrappers::opcua
