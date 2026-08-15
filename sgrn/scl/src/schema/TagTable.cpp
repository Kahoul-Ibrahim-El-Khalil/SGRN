#include <fmt/format.h>
#include <sgrn/scl/schema/TagTable.hpp>
#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/xml.hpp>

namespace sgrn::scl
{

std::optional<uint64_t> parseTagValue(const std::string& t_raw) {
    std::string s = utils::strings::trim(t_raw);
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    if (s.empty()) {
        return std::nullopt;
    }
    if (s == "true" || s == "TRUE")
        return uint64_t{1};
    if (s == "false" || s == "FALSE")
        return uint64_t{0};
    if (s.size() > 2 && s[0] == '0') {
        if (s[1] == 'b' || s[1] == 'B') {
            uint64_t val = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] != '0' && s[i] != '1')
                    return std::nullopt;
                val = (val << 1) | static_cast<uint64_t>(s[i] - '0');
            }
            return val;
        }
        if (s[1] == 'o' || s[1] == 'O') {
            uint64_t val = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] < '0' || s[i] > '7')
                    return std::nullopt;
                val = (val << 3) | static_cast<uint64_t>(s[i] - '0');
            }
            return val;
        }
        if (s[1] == 'x' || s[1] == 'X')
            return utils::strings::parseUInt64(s, 16);
    }
    if (auto u = utils::strings::parseUInt64(s, 10); u.has_value())
        return u;
    if (auto i = utils::strings::parseInt64(s); i.has_value())
        return static_cast<uint64_t>(i.value());
    return std::nullopt;
}

bool isTagTableXml(const std::string& t_content) {
    return t_content.find("<Tagtable") != std::string::npos || t_content.find("<tagtable") != std::string::npos;
}

static std::vector<PlcTag> extractTags(const sgrn::utils::xml::Document& t_doc, std::vector<std::string>& t_warnings) {
    std::vector<PlcTag> tags;
    if (!t_doc)
        return tags;

    sgrn::utils::xml::Node root = t_doc.tp_root();
    auto tagtable_node = root.findFirstChild("Tagtable");
    if (!tagtable_node) {
        tagtable_node = root.findFirstChild("tagtable");
    }

    if (!tagtable_node) {
        t_warnings.push_back("Could not find <Tagtable> root element in XML.");
        return tags;
    }

    std::string table_name = tagtable_node->attribute("name").value_or("Default tag table");

    for (auto child : tagtable_node->children()) {
        if (child.tag() != "Tag")
            continue;

        std::string tag_name = utils::strings::trim(child.text().value_or(""));
        if (tag_name.empty())
            continue;

        std::string type_str = child.attribute("type").value_or("Bool");
        std::string addr_str = child.attribute("addr").value_or("");
        std::string remark = child.attribute("remark").value_or("");

        if (!addr_str.empty() && addr_str[0] == '%') {
            addr_str = addr_str.substr(1);
        }

        if (addr_str.empty()) {
            t_warnings.push_back(fmt::format("Tag '{}': missing addr, skipped.", tag_name));
            continue;
        }

        std::optional<PlcAddress> addr = parsePlcAddress(addr_str);
        if (!addr.has_value()) {
            t_warnings.push_back(fmt::format("Tag '{}': cannot resolve addr '%{}', skipped.", tag_name, addr_str));
            continue;
        }
        addr->label = tag_name;

        DataType s7t = DataType::Bool;
        if (auto t = parseS7Type(type_str); t.has_value())
            s7t = t.value();

        if (addr->bit_index < 0) {
            const int span = rawTypeSpanBytes(s7t, 1);
            if (span > 0)
                addr->byte_count = span;
        }

        PlcTag tag;
        tag.name = tag_name;
        tag.table_name = table_name;
        tag.type_str = type_str;
        tag.remark = remark;
        tag.addr = *addr;
        tag.type = s7t;
        tags.push_back(std::move(tag));
    }
    return tags;
}

std::vector<PlcTag> parseTagTableXmlString(const std::string& t_content, std::vector<std::string>& t_warnings) {
    if (auto t_doc = sgrn::utils::xml::Document::parseString(t_content)) {
        return extractTags(*t_doc, t_warnings);
    }
    t_warnings.push_back("Failed to parse XML string.");
    return {};
}

std::vector<PlcTag> parseTagTableXmlFile(const std::string& t_path, std::vector<std::string>& t_warnings) {
    if (auto t_doc = sgrn::utils::xml::Document::parseFile(t_path)) {
        return extractTags(*t_doc, t_warnings);
    }
    t_warnings.push_back(fmt::format("Failed to parse XML file '{}'.", t_path));
    return {};
}

} // namespace sgrn::scl
