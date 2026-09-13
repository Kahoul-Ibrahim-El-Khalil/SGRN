#pragma once
// =============================================================================
// AsApiEmitter.hpp — Generates declaration-only AngelScript .as files
// =============================================================================

#include <sgrn/Result.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <string>

namespace sgrn::scl
{

struct AsEmitterOptions {
    std::string output_dir{"./generated"};
    bool include_shell_api{false};
    // Emit as.predefined: ambient declarations for IDE language servers
    // (native API + schema UDT/DB classes + DB globals). Tooling-only, never
    // loaded at runtime. Mutually exclusive with include_shell_api output
    // in the same directory (both declare the native API surface).
    bool include_predefined{false};
};

class AsApiEmitter {
public:
    static Result<void, std::string> emit(const PlcSchemaStore& store, const AsEmitterOptions& opts);
};

} // namespace sgrn::scl
