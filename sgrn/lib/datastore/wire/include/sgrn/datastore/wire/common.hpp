#pragma once
// common.hpp — wire types shared by every endpoint.

#include <cstdint>
#include <string>

namespace sgrn::datastore::common
{

struct Message {
    std::string message;
};

/// {id, name} list entry (organisations, domains, statuses).
struct IdName {
    int32_t id{0};
    std::string name;
};

} // namespace sgrn::datastore::common
