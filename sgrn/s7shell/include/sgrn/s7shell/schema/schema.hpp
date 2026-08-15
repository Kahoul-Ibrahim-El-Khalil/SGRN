
#pragma once

#include <sgrn/AngelScriptEngine.hpp>
#include <sgrn/Result.hpp>
#include <sgrn/scl/DbSchema.hpp>
namespace sgrn::s7shell::shell
{

void injectDbRefs(
    asIScriptEngine* tp_engine, asIScriptModule* tp_mod, const sgrn::scl::PlcSchemaStore& t_store, const std::string& t_client_var = "plc");

std::string buildDbPreamble(const sgrn::scl::PlcSchemaStore& t_store, const std::string& t_client_var = "plc");
} // namespace sgrn::s7shell::shell
