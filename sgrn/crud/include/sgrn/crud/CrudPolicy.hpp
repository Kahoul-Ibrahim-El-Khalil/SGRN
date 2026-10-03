#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::crud
{

enum class ReadScope { Org, Own, Global, Admin };

enum class WriteScope { Org, Own, Global, Admin };

enum class DeleteScope { Org, Own, Global, Admin };

struct CrudPolicy {
    ReadScope read_scope{ReadScope::Org};
    WriteScope write_scope{WriteScope::Own};
    DeleteScope delete_scope{DeleteScope::Admin};
    std::vector<std::string> allowed_roles{"admin", "user", "service_account"};
    bool soft_delete{true};
    bool audit{true};
    bool versioning{false};
    bool batch_enabled{false};
    uint32_t max_limit{500};
};

} // namespace sgrn::crud
