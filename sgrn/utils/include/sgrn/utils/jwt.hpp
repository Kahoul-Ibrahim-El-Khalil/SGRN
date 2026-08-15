#pragma once

#include <sgrn/Result.hpp>
#include <json/json.h>
#include <string>

namespace sgrn::utils::jwt
{

/**
 * Generate a JWT for PostgREST
 * @param t_claims JSON object containing claims (e.g., {"role": "web_user"})
 * @param t_secret Shared secret for signing
 * @return Signed JWT string
 */
::sgrn::Result<std::string> generateToken(const Json::Value& t_claims, const std::string& t_secret);

/**
 * Verify and decode a JWT
 * @param t_token Signed JWT string
 * @param t_secret Shared secret for verification
 * @return JSON object containing claims if valid, error otherwise
 */
::sgrn::Result<Json::Value> verifyToken(const std::string& t_token, const std::string& t_secret);

} // namespace sgrn::utils::jwt
