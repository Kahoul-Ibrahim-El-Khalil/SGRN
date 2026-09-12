#pragma once
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>
#include <vector>

namespace sgrn::datastore::core
{

// Runtime-sized bind wrapper around Drogon's variadic/compile-time sized
// execSqlCoro. Drogon exposes an overload taking `const std::vector<T>&`;
// this helper pins T=std::string (all CRUD binds are text; Postgres casts
// implicitly) so call sites never need to know about SqlBinder internals.
//
// The installed Drogon version provides:
//   template <typename T>
//   internal::SqlAwaiter execSqlCoro(const std::string&, const std::vector<T>&)
// so this is a thin forwarding coroutine, not a new binding primitive.
template <typename ClientPtr>
inline drogon::Task<drogon::orm::Result> execSqlCoroVec(ClientPtr client, const std::string& sql, const std::vector<std::string>& binds) {
    co_return co_await client->execSqlCoro(sql, binds);
}

} // namespace sgrn::datastore::core
