#pragma once

#include <sgrn/scl/types.hpp>

// DbSchema, DbRawBuffer, DbData, DbField, UdtDefinition are all defined in types.hpp.
// This header exists to give the new canonical name a dedicated include path.

// Convenience: bring ParseResult into scope as well (it uses DbSchema).
// All types live in namespace sgrn::scl.
