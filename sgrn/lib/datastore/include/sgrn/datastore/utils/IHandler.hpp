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

    // Item routes (`GET/PATCH/DELETE .../{id}`) need a second handler
    // signature that also takes the path parameter. Drogon maps `{...}`
    // placeholders to handler function arguments (NOT to
    // HttpRequest::getParameter() — that only carries query parameters),
    // so the item-route lambdas below declare the trailing std::string
    // argument and Drogon fills it from the matched path segment.
    // Hand-written, one-time change — independent of codegen.
    struct item_route_config {
        using CoroutineItemHandler = drogon::Task<drogon::HttpResponsePtr> (T::*)(drogon::HttpRequestPtr, std::string);

        std::string_view path;
        CoroutineItemHandler handler;

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

    // Register item routes (`.../{id}`): the trailing `{...}` placeholder
    // is mapped by Drogon onto the lambda's std::string argument and
    // forwarded as the handler's second parameter.
    template <size_t N>
    IHandler(T* tp_self, const std::array<item_route_config, N>& t_routes) {
        for (const auto& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](
                    drogon::HttpRequestPtr tsp_req, std::string t_id) -> drogon::Task<drogon::HttpResponsePtr> {
                    auto task_result = (tp_self->*h)(tsp_req, std::move(t_id));
                    co_return co_await task_result;
                },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
    }

    // Register collection + item routes together (the common CRUD shape).
    template <size_t N, size_t M>
    IHandler(T* tp_self, const std::array<route_config, N>& t_routes, const std::array<item_route_config, M>& t_item_routes) {
        for (const auto& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](drogon::HttpRequestPtr tsp_req) -> drogon::Task<drogon::HttpResponsePtr> {
                    auto task_result = (tp_self->*h)(tsp_req);
                    co_return co_await task_result;
                },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
        for (const auto& route_config : t_item_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](
                    drogon::HttpRequestPtr tsp_req, std::string t_id) -> drogon::Task<drogon::HttpResponsePtr> {
                    auto task_result = (tp_self->*h)(tsp_req, std::move(t_id));
                    co_return co_await task_result;
                },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
    }
};

} // namespace sgrn
