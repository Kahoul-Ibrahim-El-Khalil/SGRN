#pragma once
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/scl/utils.hpp>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DbField;
using ::sgrn::scl::LocatedField;

int fieldSpanSize(const DbField& t_field);
int symbolFieldSpanBytes(const DbField& t_field);
const DbField* findFieldByName(const ::sgrn::scl::DbSchema& t_reg, const std::string& t_name);
std::optional<LocatedField> findFieldByPath(const std::vector<DbField>& t_fields, const std::string& t_path, int t_base_offset = 0);
DbField plcNodeToDbField(const sgrn::gateway::twin::PlcNode& t_node);
} // namespace sgrn::gateway::twin
