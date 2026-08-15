#pragma once
#include <sgrn/scl/types.hpp>
#include <map>
#include <string>
#include <vector>

namespace sgrn::scl
{

struct TagDefinition {
    std::string name;
    std::string type;
    std::string addr;
};

class XmlTagTableParser {
public:
    static sgrn::Result<std::vector<TagDefinition>, ::sgrn::scl::Error> parse(const std::string& t_xml_content);
};

} // namespace sgrn::scl
