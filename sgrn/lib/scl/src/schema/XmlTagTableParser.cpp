#include <sgrn/scl/schema/XmlTagTableParser.hpp>
extern "C" {
#include <xml.h>
}
#include <fmt/color.h>
#include <fmt/core.h>
#include <iostream>

namespace sgrn::scl
{

sgrn::Result<std::vector<TagDefinition>, SclError> XmlTagTableParser::parse(const std::string& t_xml_content) {
    std::vector<TagDefinition> tags;

    XMLNode* p_root = xml_parse_string(t_xml_content.c_str());
    if (!p_root) {
        return sgrn::Result<std::vector<TagDefinition>, SclError>::Error(SclError::ParseError);
    }

    // Traverse the document: Tagtable -> Tag
    for (size_t i = 0; i < p_root->children->len; i++) {
        XMLNode* p_child = static_cast<XMLNode*>(p_root->children->data[i]);
        if (p_child->tag && std::string(p_child->tag) == "Tag") {
            TagDefinition tag;

            const char* p_addr = xml_node_attr(p_child, "addr");
            const char* p_type = xml_node_attr(p_child, "type");

            if (p_addr)
                tag.addr = p_addr;
            if (p_type)
                tag.type = p_type;

            if (p_child->text)
                tag.name = p_child->text;

            if (!tag.name.empty() && !tag.addr.empty()) {
                tags.push_back(tag);
            }
        }
    }

    xml_node_free(p_root);
    return tags;
}

} // namespace sgrn::scl
