#pragma once
#include "SgrnError.hpp"

#include <drogon/HttpTypes.h>
#include <stdexcept>
#include <string>

namespace sgrn
{

class SgrnException : public std::runtime_error {
public:
    // Constructor taking HttpError
    explicit SgrnException(const HttpError& t_err)
        : std::runtime_error(t_err.message)
        , status_(t_err.status)
        , error_code_(t_err.code_name) {
    }

    // Templated constructor for domain enums
    template <typename EnumT>
    explicit SgrnException(EnumT e)
        : SgrnException(makeHttpError(e)) {
    }

    // Constructor for generic string messages
    explicit SgrnException(const std::string& t_message, drogon::HttpStatusCode t_status = drogon::k500InternalServerError)
        : std::runtime_error(t_message)
        , status_(t_status)
        , error_code_("SGRN_ERROR_UNKNOWN") {
    }

    drogon::HttpStatusCode getStatus() const noexcept {
        return status_;
    }

    std::string getErrorCode() const noexcept {
        return error_code_;
    }

private:
    drogon::HttpStatusCode status_;
    std::string error_code_; // Store the stringified Error code
};

} // namespace sgrn
