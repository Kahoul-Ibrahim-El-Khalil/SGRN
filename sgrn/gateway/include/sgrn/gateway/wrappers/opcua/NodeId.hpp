#pragma once

#include <open62541/types.h>
#include <open62541/types_generated_handling.h>
#include <string_view>

namespace sgrn::gateway::wrappers::opcua
{

/// RAII move-only wrapper over UA_NodeId.
/// Raw UA_* types never cross this header's public interface.
class NodeId {
public:
    NodeId() noexcept {
        UA_NodeId_init(&id_);
    }

    ~NodeId() noexcept {
        UA_NodeId_clear(&id_);
    }

    // Move-only
    NodeId(NodeId&& t_other) noexcept
        : id_(t_other.id_) {
        UA_NodeId_init(&t_other.id_);
    }
    NodeId& operator=(NodeId&& t_other) noexcept {
        if (this != &t_other) {
            UA_NodeId_clear(&id_);
            id_ = t_other.id_;
            UA_NodeId_init(&t_other.id_);
        }
        return *this;
    }
    NodeId(const NodeId&) = delete;
    NodeId& operator=(const NodeId&) = delete;

    // ── Factories ────────────────────────────────────────────────────────────

    static NodeId numeric(uint16_t t_ns, uint32_t t_id) noexcept {
        NodeId n;
        n.id_ = UA_NODEID_NUMERIC(t_ns, t_id);
        return n;
    }

    static NodeId string(uint16_t t_ns, std::string_view t_id) {
        NodeId n;
        // UA_NODEID_STRING_ALLOC copies the string — we own the memory
        n.id_ = UA_NODEID_STRING_ALLOC(t_ns, t_id.data());
        return n;
    }

    // ── Observers ────────────────────────────────────────────────────────────

    bool isNull() const noexcept {
        return UA_NodeId_isNull(&id_);
    }

    bool operator==(const NodeId& t_other) const noexcept {
        return UA_NodeId_equal(&id_, &t_other.id_);
    }

    // ── INTERNAL: escape hatch for the adapter layer only ───────────────────
    const UA_NodeId& get() const noexcept {
        return id_;
    }
    UA_NodeId& get() noexcept {
        return id_;
    }

private:
    // Private constructor used by factories that need to wrap an existing UA_NodeId
    // produced by an open62541 call (takes ownership via copy).
    explicit NodeId(const UA_NodeId& t_raw) {
        UA_NodeId_copy(&t_raw, &id_);
    }

    friend NodeId nodeIdFromRaw(const UA_NodeId&); // defined in NodeId.cpp

    UA_NodeId id_{};
};

/// Wrap a raw UA_NodeId by copying it into a managed NodeId.
/// For use inside the wrapper implementation only.
inline NodeId nodeIdFromRaw(const UA_NodeId& t_raw) {
    return NodeId(t_raw);
}

} // namespace sgrn::gateway::wrappers::opcua
