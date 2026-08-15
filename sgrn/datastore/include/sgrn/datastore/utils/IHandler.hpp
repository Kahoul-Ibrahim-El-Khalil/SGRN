// sgrn/core/IHandler.hpp
#pragma once

#include <sgrn/datastore/utils/route_utils.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Note: makeConstraints() and joinStrings() now live in route_utils.hpp.
// Including this header is sufficient — route_utils.hpp is pulled in above.

namespace sgrn
{

template <class T>
class IHandler {
public:
    // --- Rule of 5 ---
    IHandler(const IHandler&) = delete;
    IHandler& operator=(const IHandler&) = delete;
    IHandler(IHandler&&) = delete;
    IHandler& operator=(IHandler&&) = delete;

protected:
    ~IHandler() = default;

public:
    struct route_config {
        using CoroutineHandler = drogon::Task<drogon::HttpResponsePtr> (T::*)(drogon::HttpRequestPtr);

        std::string_view path;
        CoroutineHandler handler;

        // FIX (issue 1): was std::initializer_list<drogon::HttpMethod> and
        // std::initializer_list<std::string_view>.  initializer_list is a
        // non-owning view of a backing array that may be a temporary.
        // Storing initializer_list in a struct and reading it later is UB
        // unless the struct itself is constexpr with static storage.  Switching
        // to std::vector gives owned storage that is always safe to read.
        std::vector<drogon::HttpMethod> methods;
        std::vector<std::string> filter_strings;
    };

    // Register all routes described by t_routes with Drogon's app framework.
    template <size_t N>
    IHandler(T* tp_self, const std::array<route_config, N>& t_routes) {
        for (const auto& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](drogon::HttpRequestPtr tsp_req) -> drogon::Task<drogon::HttpResponsePtr> {
                    // GCC ICE workaround (gimple_add_tmp_var): store the task
                    // in a named variable before co_awaiting.
                    auto task_result = (tp_self->*h)(tsp_req);
                    co_return co_await task_result;
                },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
    }
};

} // namespace sgrn
