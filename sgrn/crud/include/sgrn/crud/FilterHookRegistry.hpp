#pragma once

#include <drogon/HttpRequest.h>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::crud
{

using FilterHookFunc =
    std::function<void(const drogon::HttpRequestPtr& req, std::string& sql_where_clause, std::vector<std::string>& query_params)>;

class FilterHookRegistry {
public:
    static FilterHookRegistry& instance();

    void registerHook(const std::string& name, FilterHookFunc hook);
    FilterHookFunc getHook(const std::string& name) const;
    bool hasHook(const std::string& name) const;

private:
    std::unordered_map<std::string, FilterHookFunc> hooks_;
    mutable std::mutex mutex_;
};

} // namespace sgrn::crud
