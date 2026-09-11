
#include <regex>
#pragma once
#include <drogon/drogon.h>
#include <functional>

namespace sgrn
{
using HttpResponseCallback = std::function<void(drogon::HttpResponsePtr)>;

} // namespace sgrn
