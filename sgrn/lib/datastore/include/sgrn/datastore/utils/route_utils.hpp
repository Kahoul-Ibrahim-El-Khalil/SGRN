// sgrn/core/route_utils.hpp
//
// Single definition of route-registration helpers shared by IHandler.hpp and
// registerRoute.hpp.  Both files previously defined their own makeConstraints()
// overload inside namespace sgrn, which caused an ODR violation whenever a
// translation unit included both headers.  Moving the definitions here and
// including this file from both headers eliminates the problem.
#pragma once

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <sstream>
#include <string>
#include <vector>

namespace sgrn
{

// Build a Drogon constraint list from HTTP methods and optional filter names.
// Filters are embedded as HttpConstraint entries (same approach used by the
// Drogon controller macros).  The default-empty filters parameter means
// callers that only have methods don't need a second argument.
inline std::vector<drogon::internal::HttpConstraint> makeConstraints(
    const std::vector<drogon::HttpMethod>& t_methods, const std::vector<std::string>& t_filters = {}) {
    std::vector<drogon::internal::HttpConstraint> constraints;
    constraints.reserve(t_methods.size() + t_filters.size());
    for (const auto& method : t_methods) {
        constraints.emplace_back(method);
    }
    for (const auto& filter : t_filters) {
        constraints.emplace_back(filter);
    }
    return constraints;
}

// Convenience overload that accepts an initializer_list of methods so call
// sites that don't need filters can write makeConstraints({Get, Post}).
inline std::vector<drogon::internal::HttpConstraint> makeConstraints(
    std::initializer_list<drogon::HttpMethod> t_methods, const std::vector<std::string>& t_filters = {}) {
    return makeConstraints(std::vector<drogon::HttpMethod>(t_methods), t_filters);
}

inline std::string joinStrings(const std::vector<std::string>& t_strings, const std::string& t_delimiter) {
    std::ostringstream oss;
    for (size_t i = 0; i < t_strings.size(); ++i) {
        if (i > 0) {
            oss << t_delimiter;
        }
        oss << t_strings[i];
    }
    return oss.str();
}

} // namespace sgrn
