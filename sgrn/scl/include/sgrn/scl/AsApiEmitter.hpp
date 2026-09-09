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
};

class AsApiEmitter {
public:
    static Result<void, std::string> emit(const PlcSchemaStore& store, const AsEmitterOptions& opts);
};

} // namespace sgrn::scl
