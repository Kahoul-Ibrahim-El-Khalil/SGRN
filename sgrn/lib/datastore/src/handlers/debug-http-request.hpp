// debug-http-request.hpp — macro to dump an HTTP request as color-coded JSON
#pragma once

#include <drogon/HttpRequest.h>
#include <json/reader.h>
#include <json/value.h>
#include <sstream>
#include <string>

#include "debug-pretty-json.hpp"

#ifdef DEBUG_STORAGE_HANDLER
#define DEBUG_HTTP_REQUEST(tsp_req, t_handler)                                                                                             \
    do {                                                                                                                                   \
        const std::string DEBUG_METHOD = std::string((tsp_req)->methodString());                                                           \
        const std::string DEBUG_PATH = (tsp_req)->path();                                                                                  \
        const std::string DEBUG_QUERY = (tsp_req)->query();                                                                                \
        const std::string_view DEBUG_BODY_SV = (tsp_req)->getBody();                                                                       \
        const std::string DEBUG_BODY(DEBUG_BODY_SV);                                                                                       \
        std::ostringstream DEBUG_SS;                                                                                                       \
        DEBUG_SS << "\033[1;34m=== " << (t_handler) << " ===\033[0m\n";                                                                    \
        DEBUG_SS << "  \033[1;36m" << DEBUG_METHOD << "\033[0m \033[1;33m" << DEBUG_PATH << "\033[0m";                                     \
        if (!DEBUG_QUERY.empty())                                                                                                          \
            DEBUG_SS << "?\033[1;33m" << DEBUG_QUERY << "\033[0m";                                                                         \
        DEBUG_SS << "\n";                                                                                                                  \
        DEBUG_SS << "  \033[1;32mHeaders:\033[0m\n";                                                                                       \
        const auto& DEBUG_HEADERS = (tsp_req)->headers();                                                                                  \
        for (auto DEBUG_IT = DEBUG_HEADERS.begin(); DEBUG_IT != DEBUG_HEADERS.end(); ++DEBUG_IT) {                                         \
            DEBUG_SS << "    \033[1;32m" << DEBUG_IT->first << "\033[0m: " << DEBUG_IT->second << "\n";                                    \
        }                                                                                                                                  \
        if (!DEBUG_BODY.empty()) {                                                                                                         \
            DEBUG_SS << "  \033[1;35mBody:\033[0m\n";                                                                                      \
            try {                                                                                                                          \
                Json::CharReaderBuilder DEBUG_BUILDER;                                                                                     \
                std::string DEBUG_ERRS;                                                                                                    \
                Json::Value DEBUG_ROOT;                                                                                                    \
                std::istringstream DEBUG_IS(DEBUG_BODY);                                                                                   \
                if (Json::parseFromStream(DEBUG_BUILDER, DEBUG_IS, &DEBUG_ROOT, &DEBUG_ERRS)) {                                            \
                    DEBUG_SS << sgrn::datastore::handlers::storage::debugPrettyJson(DEBUG_ROOT) << "\n";                                   \
                } else {                                                                                                                   \
                    DEBUG_SS << DEBUG_BODY << "\n";                                                                                        \
                }                                                                                                                          \
            } catch (...) {                                                                                                                \
                DEBUG_SS << DEBUG_BODY << "\n";                                                                                            \
            }                                                                                                                              \
        }                                                                                                                                  \
        DEBUG_LOG("{}", DEBUG_SS.str());                                                                                                   \
    } while (0)
#else
#define DEBUG_HTTP_REQUEST(tsp_req, t_handler) ((void)0)
#endif
