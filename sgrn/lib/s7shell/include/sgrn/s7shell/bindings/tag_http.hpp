#pragma once
// =============================================================================
// tag_http.hpp — hook a PlcRuntime's discrete tags into HttpAdapter /tags/*.
//
// The twin is DB-only, so the HTTP adapter cannot serve tags by itself.
// Whoever owns a runtime (s7shell Gateway / HttpServer bindings) calls
// hookHttpTags() after configure() and the /tags listing, reads and writes
// all flow through the same shared backing as tags().get/put, the S7
// server and the gateway deltas.
// =============================================================================

#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>

#include <string>
#include <vector>

namespace sgrn::s7shell::bindings
{

inline void hookHttpTags(::sgrn::gateway::adapters::HttpAdapter* tp_http, const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& t_rt) {
    if (!tp_http || !t_rt)
        return;
    using ::sgrn::gateway::adapters::HttpAdapter;
    tp_http->setTagAccess(
        [t_rt]() {
            std::vector<HttpAdapter::TagEndpointInfo> out;
            for (const auto& name : t_rt->tagNames()) {
                auto d = t_rt->describeTag(name);
                if (d.hasError())
                    continue;
                const auto& tag = d.value();
                HttpAdapter::TagEndpointInfo info;
                info.name = tag.name;
                info.table = tag.table;
                info.type = tag.udt_name.empty() ? tag.type_str : tag.udt_name;
                info.address = tag.addr.label;
                out.push_back(std::move(info));
            }
            return out;
        },
        [t_rt](const std::string& t_name) -> sgrn::Result<std::string, std::string> {
            auto r = t_rt->readTagJson(t_name);
            if (r.hasError())
                return sgrn::Result<std::string, std::string>::Error(toString(r.error()));
            return r.value();
        },
        [t_rt](const std::string& t_name, const std::string& t_json) -> sgrn::Result<void, std::string> {
            if (auto r = t_rt->writeTagJson(t_name, t_json); r.hasError())
                return r;
            return {};
        });
}

} // namespace sgrn::s7shell::bindings
