// debug-pretty-json.hpp — ANSI-colored pretty-printer for Json::Value (inline header)
#pragma once

#include <json/value.h>
#include <string>

namespace sgrn::datastore::handlers::storage
{

// Recursively pretty-print a Json::Value with ANSI color coding.
// Colors: keys=cyan, strings=green, numbers=magenta, booleans=yellow,
// null=red, punctuation=default. Returns the formatted string.
inline std::string debugPrettyJson(const Json::Value& t_val, int t_indent = 0) {
    std::string out;
    const std::string indent(t_indent * 2, ' ');
    const std::string child_indent((t_indent + 1) * 2, ' ');
    const char* K = "\033[1;36m"; // bold cyan - key
    const char* S = "\033[1;32m"; // bold green - string
    const char* N = "\033[1;35m"; // bold magenta - number
    const char* B = "\033[1;33m"; // bold yellow - bool
    const char* Z = "\033[1;31m"; // bold red - null
    const char* R = "\033[0m";    // reset

    switch (t_val.type()) {
        case Json::objectValue: {
            out += "{\n";
            const auto& members = t_val.getMemberNames();
            for (size_t i = 0; i < members.size(); ++i) {
                const auto& key = members[i];
                out += child_indent;
                out += K;
                out += key;
                out += R;
                out += ": ";
                out += debugPrettyJson(t_val[key], t_indent + 1);
                if (i + 1 < members.size())
                    out += ",";
                out += "\n";
            }
            out += indent;
            out += "}";
            break;
        }
        case Json::arrayValue: {
            out += "[\n";
            for (size_t i = 0; i < t_val.size(); ++i) {
                out += child_indent;
                out += debugPrettyJson(t_val[static_cast<int>(i)], t_indent + 1);
                if (i + 1 < t_val.size())
                    out += ",";
                out += "\n";
            }
            out += indent;
            out += "]";
            break;
        }
        case Json::stringValue:
            out += S;
            out += "\"";
            out += t_val.asString();
            out += "\"";
            out += R;
            break;
        case Json::intValue:
        case Json::uintValue:
            out += N;
            out += std::to_string(t_val.asInt64());
            out += R;
            break;
        case Json::realValue:
            out += N;
            out += std::to_string(t_val.asDouble());
            out += R;
            break;
        case Json::booleanValue:
            out += B;
            out += (t_val.asBool() ? "true" : "false");
            out += R;
            break;
        case Json::nullValue:
            out += Z;
            out += "null";
            out += R;
            break;
        default:
            out += "<?>";
            break;
    }
    return out;
}

} // namespace sgrn::datastore::handlers::storage
