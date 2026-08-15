// sgrn/core/registerRoute.hpp
//
// Lightweight helpers for registering callback-style and coroutine-style
// Drogon handlers by member-function pointer.
//
// makeConstraints() previously had its own definition here that conflicted
// with the one in IHandler.hpp (ODR violation when both headers were included
// in the same TU).  Both definitions are now removed in favour of the single
// authoritative definition in route_utils.hpp.
#pragma once

#include <drogon/drogon.h>
#include <sgrn/datastore/utils/route_utils.hpp>
#include <sgrn/types/HttpResponseCallback.hpp>
#include <functional>
#include <string>
#include <vector>

namespace sgrn
{

using HandlingRouteMethod = std::function<void(drogon::HttpRequestPtr, HttpResponseCallback&&)>;

// Register a synchronous (callback-style) handler.
// Filters are now passed as a vector<string> and embedded in the constraint
// list via makeConstraints() so both IHandler and registerRoute use the same
// Drogon registration path.
template <typename T, typename Method>
void registerCallbackRoute(T* tp_arg_self, const std::string& t_arg_route, Method t_arg_method,
    std::initializer_list<drogon::HttpMethod> t_arg_methods, std::vector<std::string> t_arg_filters = {}) {
    drogon::app().registerHandler(
        t_arg_route,
        [tp_arg_self, t_arg_method](drogon::HttpRequestPtr tsp_http_req, HttpResponseCallback&& tsp_response_callback) {
            (tp_arg_self->*t_arg_method)(std::move(tsp_http_req), std::move(tsp_response_callback));
        },
        makeConstraints(t_arg_methods, t_arg_filters));
}

// Register an async (coroutine-style) handler.
//
// WORKAROUND: GCC crash (gimple_add_tmp_var).
// The coroutine task must be stored in a named variable before co_awaiting it;
// do not combine into a single expression.
template <typename T, typename Method>
void registerCoroutineRoute(T* tp_arg_self, const std::string& t_arg_route, Method t_arg_method,
    std::initializer_list<drogon::HttpMethod> t_arg_methods, std::vector<std::string> t_arg_filters = {}) {
    drogon::app().registerHandler(
        t_arg_route,
        [tp_arg_self, t_arg_method](drogon::HttpRequestPtr tsp_http_req) -> drogon::Task<drogon::HttpResponsePtr> {
            auto task_result = (tp_arg_self->*t_arg_method)(tsp_http_req);
            co_return co_await task_result;
        },
        makeConstraints(t_arg_methods, t_arg_filters));
}

} // namespace sgrn
