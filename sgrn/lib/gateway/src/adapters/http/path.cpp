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

std::vector<std::string_view> splitSlashes(std::string_view path) {
    std::vector<std::string_view> segments;

    if (path.empty()) {
        segments.emplace_back();
        return segments;
    }

    size_t begin = 0;

    while (begin <= path.size()) {
        const size_t end = path.find('/', begin);

        if (end == std::string_view::npos) {
            segments.emplace_back(path.substr(begin));
            break;
        }

        segments.emplace_back(path.substr(begin, end - begin));
        begin = end + 1;

        // Preserve a trailing empty segment, matching the old implementation.
        if (begin == path.size()) {
            segments.emplace_back();
            break;
        }
    }

    return segments;
}

Resolution resolveSemanticPath(const std::vector<std::string>& t_segs, const ::sgrn::scl::PlcSchemaStore& t_registry) {
    std::optional<size_t> detected_index;
    size_t index_pos = std::string::npos;

    // Find the first all-digits segment (array index)
    for (size_t i = 0; i < t_segs.size(); ++i) {
        if (isAllDigits(t_segs[i])) {
            detected_index = std::stoull(t_segs[i]);
            index_pos = i;
            break;
        }
    }

    std::vector<std::string> base_segs = t_segs;
    if (detected_index.has_value()) {
        base_segs.erase(base_segs.begin() + index_pos);
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
            // If we detected an array index, split field_path into array path + rest
            std::string array_rest_path;
            if (detected_index.has_value()) {
                // Find the last segment that could be an array field
                size_t last_slash = fpath.find_last_of('/');
                if (last_slash != std::string::npos) {
                    array_rest_path = fpath.substr(last_slash + 1);
                    fpath = fpath.substr(0, last_slash);
                }
            }
            return {r.value(), fpath, array_rest_path, detected_index};
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
                return {r.value(), fpath, std::string{}, std::nullopt};
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
