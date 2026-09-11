#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declaration of the internal C struct
struct XMLNode;

namespace sgrn::utils::xml
{

/**
 * @brief RAII wrapper for an XMLNode from the internal lightweight XML parser.
 *
 * Provides a safe, object-oriented interface to traverse XML trees loaded
 * from PLC export files.
 */
class Node {
public:
    Node(XMLNode* tp_n)
        : node_(tp_n) {
    }

    std::string tag() const;
    std::optional<std::string> text() const;

    std::vector<Node> children() const;
    std::optional<Node> findFirstChild(const std::string& t_name) const;

    std::optional<std::string> attribute(const std::string& t_name) const;
    std::vector<std::pair<std::string, std::string>> attributes() const;

    XMLNode* get() const {
        return node_;
    }
    explicit operator bool() const {
        return node_ != nullptr;
    }

private:
    XMLNode* node_;
};

/**
 * @brief RAII wrapper for an XML Document.
 *
 * Owns the lifecycle of the parsed XML tree. When this object is destroyed,
 * all associated memory for the document is freed.
 */
class Document {
public:
    Document() = default;
    ~Document();

    // Non-copyable, movable
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& t_other) noexcept;
    Document& operator=(Document&& t_other) noexcept;

    static std::optional<Document> parseString(const std::string& t_xml_content);
    static std::optional<Document> parseFile(const std::string& t_path);

    Node tp_root() const {
        return Node(root_);
    }
    explicit operator bool() const {
        return root_ != nullptr;
    }

private:
    explicit Document(XMLNode* tp_root);
    XMLNode* root_{nullptr};
};

} // namespace sgrn::utils::xml
