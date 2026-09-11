#pragma once
#include <sgrn/scl/types.hpp>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::scl
{

/**
 * @brief Parses a value string for a named tag.
 */
std::optional<uint64_t> parseTagValue(const std::string& t_raw);

/**
 * @brief Checks if a string content looks like a TIA Portal tag table XML.
 */
bool isTagTableXml(const std::string& t_content);

/**
 * @brief Parses a TIA Portal <Tagtable> XML file or string into a list of PlcTag entries.
 */
std::vector<PlcTag> parseTagTableXmlString(const std::string& t_content, std::vector<std::string>& t_warnings);
std::vector<PlcTag> parseTagTableXmlFile(const std::string& t_path, std::vector<std::string>& t_warnings);

} // namespace sgrn::scl
