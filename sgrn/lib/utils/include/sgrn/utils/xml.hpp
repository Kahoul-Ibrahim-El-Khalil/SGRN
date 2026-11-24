#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// ── Lightweight XML text/tag helpers ─────────────────────────────────────────

/// Escape XML special characters (&, <, >, ", ').
std::string xmlEscape(std::string_view t_str);

/// Unescape standard XML entities (&amp;, &lt;, &gt;, &quot;, &apos;).
std::string xmlUnescape(std::string_view t_str);

/// Extract and unescape the text inside the first <tag>...</tag> element.
std::string extractXmlTag(std::string_view t_xml, std::string_view t_tag);

/// Extract raw inner contents of all <tag>...</tag> blocks in t_xml.
std::vector<std::string> extractXmlBlocks(std::string_view t_xml, std::string_view t_tag);

/// Zero-copy iteration over the inner contents of all <tag>...</tag> blocks in t_xml.
template <typename Fn>
inline void forEachXmlBlock(std::string_view t_xml, std::string_view t_tag, Fn&& t_fn) {
    std::string open_tag;
    open_tag.reserve(t_tag.size() + 2);
    open_tag.push_back('<');
    open_tag.append(t_tag);
    open_tag.push_back('>');

    std::string close_tag;
    close_tag.reserve(t_tag.size() + 3);
    close_tag.append("</");
    close_tag.append(t_tag);
    close_tag.push_back('>');

    size_t pos = 0;
    while (pos < t_xml.size()) {
        const size_t start = t_xml.find(open_tag, pos);
        if (start == std::string_view::npos)
            break;
        const size_t content_start = start + open_tag.size();
        const size_t end = t_xml.find(close_tag, content_start);
        if (end == std::string_view::npos)
            break;
        t_fn(t_xml.substr(content_start, end - content_start));
        pos = end + close_tag.size();
    }
}

} // namespace sgrn::utils::xml
