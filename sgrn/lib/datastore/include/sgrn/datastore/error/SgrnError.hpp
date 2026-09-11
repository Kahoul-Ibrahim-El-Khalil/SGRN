#pragma once

#include <drogon/HttpTypes.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <string>

namespace sgrn
{

/// Convenience result alias for HTTP handler functions.
template <typename T>
using HttpResult = ::sgrn::Result<T, HttpError>;

} // namespace sgrn
