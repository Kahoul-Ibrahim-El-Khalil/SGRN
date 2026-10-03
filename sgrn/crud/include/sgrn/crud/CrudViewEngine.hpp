#pragma once

#include <sgrn/crud/CrudSpec.hpp>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

#include <string>

namespace sgrn::crud
{

class CrudViewEngine {
public:
    static drogon::Task<drogon::HttpResponsePtr> executeList(const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant);

    static drogon::Task<drogon::HttpResponsePtr> executeGet(
        const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id);

    static drogon::Task<drogon::HttpResponsePtr> executeCreate(const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant);

    static drogon::Task<drogon::HttpResponsePtr> executeUpdate(
        const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id);

    static drogon::Task<drogon::HttpResponsePtr> executeDelete(
        const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id);
};

} // namespace sgrn::crud
