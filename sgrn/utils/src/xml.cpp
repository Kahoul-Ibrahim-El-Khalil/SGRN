#include "xml.h"
#include <sgrn/utils/xml.hpp>
#include <utility>

namespace sgrn::utils::xml
{

// -----------------------------------------------------------------------------
// Node Implementation
// -----------------------------------------------------------------------------

std::string Node::tag() const {
    if (node_ && node_->tag)
        return node_->tag;
    return "";
}

std::optional<std::string> Node::text() const {
    if (node_ && node_->text)
        return std::string(node_->text);
    return std::nullopt;
}

std::vector<Node> Node::children() const {
    std::vector<Node> res;
    if (node_ && node_->children) {
        res.reserve(node_->children->len);
        for (size_t i = 0; i < node_->children->len; ++i) {
            res.emplace_back(static_cast<XMLNode*>(node_->children->data[i]));
        }
    }
    return res;
}

std::optional<Node> Node::findFirstChild(const std::string& t_name) const {
    if (!node_)
        return std::nullopt;
    XMLNode* p_found = xml_node_find_tag(node_, t_name.c_str(), true);
    if (p_found)
        return Node(p_found);
    return std::nullopt;
}

std::optional<std::string> Node::attribute(const std::string& t_name) const {
    if (!node_)
        return std::nullopt;
    const char* p_val = xml_node_attr(node_, t_name.c_str());
    if (p_val)
        return std::string(p_val);
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> Node::attributes() const {
    std::vector<std::pair<std::string, std::string>> res;
    if (node_ && node_->attrs) {
        res.reserve(node_->attrs->len);
        for (size_t i = 0; i < node_->attrs->len; ++i) {
            auto attr = static_cast<XMLAttr*>(node_->attrs->data[i]);
            res.emplace_back(attr->key, attr->value);
        }
    }
    return res;
}

// -----------------------------------------------------------------------------
// Document Implementation
// -----------------------------------------------------------------------------

Document::Document(XMLNode* tp_root)
    : root_(tp_root) {
}

Document::~Document() {
    if (root_) {
        xml_node_free(root_);
    }
}

Document::Document(Document&& t_other) noexcept
    : root_(std::exchange(t_other.root_, nullptr)) {
}

Document& Document::operator=(Document&& t_other) noexcept {
    if (this != &t_other) {
        if (root_)
            xml_node_free(root_);
        root_ = std::exchange(t_other.root_, nullptr);
    }
    return *this;
}

std::optional<Document> Document::parseString(const std::string& t_xml_content) {
    XMLNode* p_root = xml_parse_string(t_xml_content.c_str());
    if (p_root)
        return Document(p_root);
    return std::nullopt;
}

std::optional<Document> Document::parseFile(const std::string& t_path) {
    XMLNode* p_root = xml_parse_file(t_path.c_str());
    if (p_root) {
        return Document(p_root);
    }
    return std::nullopt;
}

} // namespace sgrn::utils::xml
