#include <sgrn/gateway/adapters/http/path.hpp>
#include <algorithm>
#include <cctype>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::gateway::adapters
{

bool isAllDigits(const std::string& t_s) {
    return !t_s.empty() && std::all_of(t_s.begin(), t_s.end(), [](unsigned char t_c) { return std::isdigit(t_c); });
}

Resolution resolveSemanticPath(const std::vector<std::string>& t_segs, const ::sgrn::scl::PlcSchemaStore& t_registry) {
    std::optional<size_t> detected_index;
    std::vector<std::string> base_segs = t_segs;
    if (t_segs.size() >= 2 && isAllDigits(t_segs.back())) {
        detected_index = std::stoull(t_segs.back());
        base_segs.pop_back();
    }

    std::string t_prefix;
    for (size_t i = 0; i < base_segs.size(); ++i) {
        if (!t_prefix.empty())
            t_prefix += "/";
        t_prefix += base_segs[i];

        if (auto r = t_registry.getDbByName(t_prefix); !r.hasError()) {
            std::string fpath;
            for (size_t j = i + 1; j < base_segs.size(); ++j) {
                if (!fpath.empty())
                    fpath += "/";
                fpath += base_segs[j];
            }
            return {r.value(), fpath, detected_index};
        }
    }

    if (detected_index.has_value()) {
        std::string prefix2;
        for (size_t i = 0; i < t_segs.size(); ++i) {
            if (!prefix2.empty())
                prefix2 += "/";
            prefix2 += t_segs[i];

            if (auto r = t_registry.getDbByName(prefix2); !r.hasError()) {
                std::string fpath;
                for (size_t j = i + 1; j < t_segs.size(); ++j) {
                    if (!fpath.empty())
                        fpath += "/";
                    fpath += t_segs[j];
                }
                return {r.value(), fpath, std::nullopt};
            }
        }
    }
    return {};
}

void collectLeaves(const rapidjson::Value& t_node, const std::string& t_prefix, std::vector<std::pair<std::string, std::string>>& t_out) {
    if (!t_node.IsObject()) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        t_node.Accept(w);
        t_out.push_back({t_prefix, sb.GetString()});
        return;
    }
    for (auto it = t_node.MemberBegin(); it != t_node.MemberEnd(); ++it) {
        std::string child = t_prefix.empty() ? it->name.GetString() : (t_prefix + "/" + it->name.GetString());
        collectLeaves(it->value, child, t_out);
    }
}

} // namespace sgrn::gateway::adapters
