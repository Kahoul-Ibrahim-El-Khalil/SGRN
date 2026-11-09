// sgrn/core/IHandler.hpp
#pragma once

#include <sgrn/datastore/utils/route-utils.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// Note: makeConstraints() and joinStrings() now live in route_utils.hpp.
// Including this header is sufficient — route-utils.hpp is pulled in above.

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
    struct RouteConfig {
        using CoroutineHandler = drogon::Task<drogon::HttpResponsePtr> (T::*)(drogon::HttpRequestPtr);

        // Current route declarations use at most two methods and two filters.
        // The arrays own copies of initializer-list values; the spans below
        // never refer to an initializer_list backing array.
        static constexpr std::size_t MaxMethods = 2;
        static constexpr std::size_t MaxFilters = 2;

        std::string_view path;
        CoroutineHandler handler;
        std::span<const drogon::HttpMethod> methods;
        std::span<const std::string_view> filter_strings;

        constexpr RouteConfig(std::string_view t_path, CoroutineHandler t_handler, std::initializer_list<drogon::HttpMethod> t_methods,
            std::initializer_list<std::string_view> t_filters)
            : path(t_path)
            , handler(t_handler)
            , methods(method_storage)
            , filter_strings(filter_storage) {
            copyMetadata(t_methods, t_filters);
        }

        constexpr RouteConfig(const RouteConfig& t_other)
            : path(t_other.path)
            , handler(t_other.handler)
            , methods(method_storage)
            , filter_strings(filter_storage)
            , method_storage(t_other.method_storage)
            , filter_storage(t_other.filter_storage) {
        }

        constexpr RouteConfig(RouteConfig&& t_other) noexcept
            : path(t_other.path)
            , handler(t_other.handler)
            , methods(method_storage)
            , filter_strings(filter_storage)
            , method_storage(t_other.method_storage)
            , filter_storage(t_other.filter_storage) {
        }

        constexpr RouteConfig& operator=(const RouteConfig& t_other) {
            if (this != &t_other) {
                path = t_other.path;
                handler = t_other.handler;
                method_storage = t_other.method_storage;
                filter_storage = t_other.filter_storage;
            }
            return *this;
        }

        constexpr RouteConfig& operator=(RouteConfig&& t_other) noexcept {
            return operator=(static_cast<const RouteConfig&>(t_other));
        }

    private:
        std::array<drogon::HttpMethod, MaxMethods> method_storage{};
        std::array<std::string_view, MaxFilters> filter_storage{};

        constexpr void copyMetadata(
            std::initializer_list<drogon::HttpMethod> t_methods, std::initializer_list<std::string_view> t_filters) {
            // Route metadata is authored in this source tree. Keep failures
            // explicit if a route grows beyond the fixed, allocation-free
            // representation instead of truncating registration constraints.
            if (t_methods.size() > MaxMethods || t_filters.size() > MaxFilters) {
                throw std::length_error("IHandler route metadata exceeds fixed capacity");
            }
            std::copy(t_methods.begin(), t_methods.end(), method_storage.begin());
            std::copy(t_filters.begin(), t_filters.end(), filter_storage.begin());
            methods = std::span<const drogon::HttpMethod>(method_storage.data(), t_methods.size());
            filter_strings = std::span<const std::string_view>(filter_storage.data(), t_filters.size());
        }
    };

    // Item routes (`GET/PATCH/DELETE .../{id}`) need a second handler
    // signature that also takes the path parameter. Drogon maps `{...}`
    // placeholders to handler function arguments (NOT to
    // HttpRequest::getParameter() — that only carries query parameters),
    // so the item-route lambdas below declare the trailing std::string
    // argument and Drogon fills it from the matched path segment.
    // Hand-written, one-time change — independent of codegen.
    struct ItemRouteConfig {
        using CoroutineItemHandler = drogon::Task<drogon::HttpResponsePtr> (T::*)(drogon::HttpRequestPtr, std::string);

        static constexpr std::size_t MaxMethods = RouteConfig::MaxMethods;
        static constexpr std::size_t MaxFilters = RouteConfig::MaxFilters;

        std::string_view path;
        CoroutineItemHandler handler;
        std::span<const drogon::HttpMethod> methods;
        std::span<const std::string_view> filter_strings;

        constexpr ItemRouteConfig(std::string_view t_path, CoroutineItemHandler t_handler,
            std::initializer_list<drogon::HttpMethod> t_methods, std::initializer_list<std::string_view> t_filters)
            : path(t_path)
            , handler(t_handler)
            , methods(method_storage)
            , filter_strings(filter_storage) {
            copyMetadata(t_methods, t_filters);
        }

        constexpr ItemRouteConfig(const ItemRouteConfig& t_other)
            : path(t_other.path)
            , handler(t_other.handler)
            , methods(method_storage)
            , filter_strings(filter_storage)
            , method_storage(t_other.method_storage)
            , filter_storage(t_other.filter_storage) {
        }

        constexpr ItemRouteConfig(ItemRouteConfig&& t_other) noexcept
            : path(t_other.path)
            , handler(t_other.handler)
            , methods(method_storage)
            , filter_strings(filter_storage)
            , method_storage(t_other.method_storage)
            , filter_storage(t_other.filter_storage) {
        }

        constexpr ItemRouteConfig& operator=(const ItemRouteConfig& t_other) {
            if (this != &t_other) {
                path = t_other.path;
                handler = t_other.handler;
                method_storage = t_other.method_storage;
                filter_storage = t_other.filter_storage;
            }
            return *this;
        }

        constexpr ItemRouteConfig& operator=(ItemRouteConfig&& t_other) noexcept {
            return operator=(static_cast<const ItemRouteConfig&>(t_other));
        }

    private:
        std::array<drogon::HttpMethod, MaxMethods> method_storage{};
        std::array<std::string_view, MaxFilters> filter_storage{};

        constexpr void copyMetadata(
            std::initializer_list<drogon::HttpMethod> t_methods, std::initializer_list<std::string_view> t_filters) {
            if (t_methods.size() > MaxMethods || t_filters.size() > MaxFilters) {
                throw std::length_error("IHandler item route metadata exceeds fixed capacity");
            }
            std::copy(t_methods.begin(), t_methods.end(), method_storage.begin());
            std::copy(t_filters.begin(), t_filters.end(), filter_storage.begin());
            methods = std::span<const drogon::HttpMethod>(method_storage.data(), t_methods.size());
            filter_strings = std::span<const std::string_view>(filter_storage.data(), t_filters.size());
        }
    };

    // Register all routes described by t_routes with Drogon's app framework.
    template <size_t N>
    IHandler(T* tp_self, const std::array<RouteConfig, N>& t_routes) {
        for (const auto& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](
                    drogon::HttpRequestPtr tsp_req) -> drogon::Task<drogon::HttpResponsePtr> { return (tp_self->*h)(std::move(tsp_req)); },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
    }

    // Register item routes (`.../{id}`): the trailing `{...}` placeholder
    // is mapped by Drogon onto the lambda's std::string argument and
    // forwarded as the handler's second parameter.
    template <size_t N>
    IHandler(T* tp_self, const std::array<ItemRouteConfig, N>& t_routes) {
        for (const ItemRouteConfig& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](drogon::HttpRequestPtr tsp_req, std::string t_id)
                    -> drogon::Task<drogon::HttpResponsePtr> { return (tp_self->*h)(std::move(tsp_req), std::move(t_id)); },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
    }

    // Register collection + item routes together (the common CRUD shape).
    template <size_t N, size_t M>
    IHandler(T* tp_self, const std::array<RouteConfig, N>& t_routes, const std::array<ItemRouteConfig, M>& t_item_routes) {
        for (const RouteConfig& route_config : t_routes) {
            drogon::app().registerHandler(
                std::string(route_config.path),
                [tp_self, h = route_config.handler](
                    drogon::HttpRequestPtr tsp_req) -> drogon::Task<drogon::HttpResponsePtr> { return (tp_self->*h)(std::move(tsp_req)); },
                makeConstraints(route_config.methods, route_config.filter_strings));
        }
        for (const auto& item_route_config : t_item_routes) {
            drogon::app().registerHandler(
                std::string(item_route_config.path),
                [tp_self, h = item_route_config.handler](drogon::HttpRequestPtr tsp_req, std::string t_id)
                    -> drogon::Task<drogon::HttpResponsePtr> { return (tp_self->*h)(std::move(tsp_req), std::move(t_id)); },
                makeConstraints(item_route_config.methods, item_route_config.filter_strings));
        }
    }
};

} // namespace sgrn
