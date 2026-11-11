#pragma once

// SGRN's typed JSON serialization boundary.
//
// Application code should include this header instead of depending on the
// selected serialization backend. Backend-specific reflection declarations
// may still live in implementation files, but application-facing operations
// use the sgrn::json read/write functions defined here.

#include <sgrn/utils/glaze.hpp>
