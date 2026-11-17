#pragma once

#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/utils/glaze.hpp>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace sgrn::datastore
{

struct BackendErrorResponse {
    std::string error;
    std::string scope;
    std::optional<std::string> code;
    drogon::HttpStatusCode http_status{drogon::k500InternalServerError};

    BackendErrorResponse(std::string_view t_message, drogon::HttpStatusCode t_status, std::string_view t_scope = "Unknown")
        : error(t_message)
        , scope(t_scope)
        , http_status(t_status) {
        if (scope == "Unknown" && (t_status == drogon::k401Unauthorized || t_status == drogon::k403Forbidden)) {
            scope = "Authentication";
        }

        if (error.find("duplicate key value violates unique constraint") != std::string::npos ||
            error.find("uq_files") != std::string::npos || error.find("already exists") != std::string::npos) {
            error = "A file or directory with that name already exists in this location.";
        } else if (error.find("violates foreign key constraint") != std::string::npos) {
            error = "Referenced entity does not exist.";
        }
    }

    BackendErrorResponse(const BackendError& t_error, std::optional<drogon::HttpStatusCode> t_status = std::nullopt)
        : error(t_error.message)
        , scope(t_error.scope)
        , code(t_error.sub_code)
        , http_status(t_status.value_or(kindToHttpStatusCode(t_error.kind))) {
    }

    BackendErrorResponse(const ::sgrn::HttpError& t_error)
        : error(t_error.message)
        , scope(scopeFromCode(t_error.code_name))
        , http_status(t_error.status) {
    }

    template <typename EnumT, std::enable_if_t<std::is_enum_v<EnumT>, int> = 0>
    BackendErrorResponse(EnumT t_error)
        : BackendErrorResponse(::sgrn::makeHttpError(t_error)) {
    }

    static std::string scopeFromCode(std::string_view t_code) {
        const auto first_separator = t_code.find('_');
        if (first_separator == std::string_view::npos) {
            return "Unknown";
        }
        const auto second_separator = t_code.find('_', first_separator + 1);
        std::string first(t_code.substr(0, first_separator));
        std::string second(t_code.substr(first_separator + 1,
            second_separator == std::string_view::npos ? std::string_view::npos : second_separator - first_separator - 1));
        if (!first.empty()) {
            first[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(first[0])));
            for (std::size_t i = 1; i < first.size(); ++i) {
                first[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(first[i])));
            }
        }
        if (!second.empty()) {
            second[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(second[0])));
            for (std::size_t i = 1; i < second.size(); ++i) {
                second[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(second[i])));
            }
        }
        return first + second;
    }

    static drogon::HttpStatusCode kindToHttpStatusCode(BackendErrorKind t_kind) noexcept {
        switch (t_kind) {
            case BackendErrorKind::Database:
            case BackendErrorKind::Garage:
            case BackendErrorKind::Filesystem:
            case BackendErrorKind::Compression:
            case BackendErrorKind::Hashing:
            case BackendErrorKind::Runtime:
            case BackendErrorKind::Generic:
                return drogon::k500InternalServerError;
            case BackendErrorKind::AlreadyExists:
                return drogon::k409Conflict;
            case BackendErrorKind::Auth:
                return drogon::k401Unauthorized;
            case BackendErrorKind::Network:
                return drogon::k503ServiceUnavailable;
        }
        return drogon::k500InternalServerError;
    }
};

} // namespace sgrn::datastore

template <>
struct glz::meta<::sgrn::datastore::BackendErrorResponse> {
    using T = ::sgrn::datastore::BackendErrorResponse;
    static constexpr auto value = glz::object("error", &T::error, "scope", &T::scope, "code", &T::code);
};
