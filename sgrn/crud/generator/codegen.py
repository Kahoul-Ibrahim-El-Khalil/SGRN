from pathlib import Path
from typing import Dict, Any, List

try:
    from jinja2 import Environment, FileSystemLoader
    HAS_JINJA2 = True
except ImportError:
    HAS_JINJA2 = False

def snake_to_pascal(snake_str: str) -> str:
    components = snake_str.split('_')
    return ''.join(x.title() for x in components)

class CppCodeGenerator:
    def __init__(self, templates_dir: Path):
        self.templates_dir = templates_dir
        if HAS_JINJA2:
            self.env = Environment(
                loader=FileSystemLoader(str(templates_dir)),
                trim_blocks=True,
                lstrip_blocks=True
            )
        else:
            self.env = None

    def generate_handler(self, manifest: Dict[str, Any]) -> str:
        if HAS_JINJA2 and self.env:
            template = self.env.get_template("handler.gen.hpp.j2")
            table_name = manifest["table"]["name"]
            class_name = f"{snake_to_pascal(table_name)}View"

            read_scope = manifest["policy"].get("read_scope", "org").title()
            write_scope = manifest["policy"].get("write_scope", "own").title()
            delete_scope = manifest["policy"].get("delete_scope", "admin").title()

            return template.render(
                manifest=manifest,
                class_name=class_name,
                read_scope_pascal=read_scope,
                write_scope_pascal=write_scope,
                delete_scope_pascal=delete_scope
            )

        # Fallback pure-Python rendering if jinja2 is absent
        table_name = manifest["table"]["name"]
        schema_name = manifest["table"]["schema"]
        class_name = f"{snake_to_pascal(table_name)}View"

        read_scope = manifest["policy"].get("read_scope", "org").title()
        write_scope = manifest["policy"].get("write_scope", "own").title()
        delete_scope = manifest["policy"].get("delete_scope", "admin").title()
        roles = ", ".join(f'"{r}"' for r in manifest["policy"].get("allowed_roles", ["admin"]))

        fields_code = []
        for fname, fval in manifest.get("fields", {}).items():
            fmt = fval.get("validation", {}).get("format", "") if fval.get("validation") else ""
            maxlen = fval.get("validation", {}).get("maxlen", 0) if fval.get("validation") else 0
            unique = "true" if fval.get("validation", {}).get("unique", False) else "false"
            op_flags = fval.get("op_flags_str", "sgrn::crud::Op::None")

            field_str = f"""            sgrn::crud::CrudFieldSpec{{
                .name = "{fname}",
                .type = sgrn::crud::FieldType::{fval.get("type_pascal", "Text")},
                .readable = {"true" if fval.get("readable") else "false"},
                .writable = {"true" if fval.get("writable") else "false"},
                .filterable = {"true" if fval.get("filterable") else "false"},
                .operators = {op_flags},
                .validation = sgrn::crud::FieldValidation{{
                    .format = "{fmt}",
                    .maxlen = {maxlen},
                    .unique = {unique},
                    .enum_values = {{}}
                }},
                .nullable = {"true" if fval.get("nullable", True) else "false"},
                .default_val = "{fval.get('default', '')}",
                .soft_delete_field = {"true" if fval.get("soft_delete_field") else "false"},
                .version_field = {"true" if fval.get("version_field") else "false"}
            }}"""
            fields_code.append(field_str)

        fields_joined = ",\n".join(fields_code)

        routes = manifest.get("routes", {})
        list_route = routes.get("list", {})
        get_route = routes.get("get", {})
        create_route = routes.get("create", {})
        update_route = routes.get("update", {})
        delete_route = routes.get("delete", {})

        return f"""// generated/{table_name}.gen.hpp (auto-generated from manifest)
#pragma once

#include <sgrn/crud/CrudViewHandler.hpp>
#include <array>
#include <vector>

namespace sgrn::crud::generated {{

class {class_name} : public sgrn::crud::CrudViewHandler<{class_name}> {{
public:
    static inline const sgrn::crud::CrudPolicy kPolicy{{
        .read_scope = sgrn::crud::ReadScope::{read_scope},
        .write_scope = sgrn::crud::WriteScope::{write_scope},
        .delete_scope = sgrn::crud::DeleteScope::{delete_scope},
        .allowed_roles = {{ {roles} }},
        .soft_delete = {"true" if manifest["policy"].get("soft_delete") else "false"},
        .audit = {"true" if manifest["policy"].get("audit") else "false"},
        .versioning = {"true" if manifest["policy"].get("versioning") else "false"},
        .batch_enabled = {"true" if manifest["policy"].get("batch_enabled") else "false"},
        .max_limit = {manifest["policy"].get("max_limit", 500)},
    }};

    static inline const sgrn::crud::CrudViewSpec kSpec{{
        .table_schema = "{schema_name}",
        .table_name = "{table_name}",
        .primary_key = "{manifest['table']['primary_key']}",
        .primary_key_type = "{manifest['table']['primary_key_type']}",
        .tenant_column = "{manifest['tenant']['column']}",
        .policy = kPolicy,
        .fields = {{
{fields_joined}
        }},
        .default_order = "{manifest['table']['primary_key']} DESC",
        .admin_overrides = {{}}
    }};

    {class_name}() : CrudViewHandler(this, kRoutes, kItemRoutes) {{}}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO({class_name}::handleList, "{list_route.get('path', '/api/v1/' + table_name)}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO({class_name}::handleCreate, "{create_route.get('path', '/api/v1/' + table_name)}", drogon::Post, "UserAuthFilter");
    ADD_METHOD_TO({class_name}::handleGet, "{get_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Get, "UserAuthFilter");
    ADD_METHOD_TO({class_name}::handleUpdate, "{update_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Patch, "UserAuthFilter");
    ADD_METHOD_TO({class_name}::handleDelete, "{delete_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Delete, "UserAuthFilter");
    METHOD_LIST_END

private:
    static inline const std::vector<sgrn::crud::RouteConfig> kRoutes = {{
        {{"{list_route.get('path', '/api/v1/' + table_name)}", drogon::Get, {{"UserAuthFilter"}}}},
        {{"{create_route.get('path', '/api/v1/' + table_name)}", drogon::Post, {{"UserAuthFilter"}}}}
    }};

    static inline const std::vector<sgrn::crud::ItemRouteConfig> kItemRoutes = {{
        {{"{get_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Get, {{"UserAuthFilter"}}}},
        {{"{update_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Patch, {{"UserAuthFilter"}}}},
        {{"{delete_route.get('path', '/api/v1/' + table_name + '/{id}')}", drogon::Delete, {{"UserAuthFilter"}}}}
    }};
}};

}} // namespace sgrn::crud::generated
"""

    def generate_spec(self, manifest: Dict[str, Any]) -> str:
        table_name = manifest["table"]["name"]
        class_name = f"{snake_to_pascal(table_name)}"
        return f"""// generated/{table_name}.spec.gen.hpp
#pragma once
#include <sgrn/crud/CrudSpec.hpp>

namespace sgrn::crud::generated {{

struct {class_name}Spec {{
    static const CrudViewSpec& get() {{
        static const CrudViewSpec spec{{
            .table_schema = "{manifest['table']['schema']}",
            .table_name = "{table_name}",
            .primary_key = "{manifest['table']['primary_key']}",
            .primary_key_type = "{manifest['table']['primary_key_type']}",
            .tenant_column = "{manifest['tenant']['column']}",
            .default_order = "{manifest['table']['primary_key']} DESC"
        }};
        return spec;
    }}
}};

}} // namespace sgrn::crud::generated
"""

    def generate_registered_views(self, handler_info_list: List[Dict[str, str]]) -> str:
        includes = "\n".join(f'#include "{h["file_basename"]}"' for h in handler_info_list)
        registers = "\n".join(f'    drogon::app().registerController(std::make_shared<{h["class_name"]}>());' for h in handler_info_list)

        return f"""// generated/RegisteredViews.cpp (auto-generated from manifests)
#include <drogon/HttpAppFramework.h>

{includes}

namespace sgrn::crud::generated {{

void registerAllCrudViews() {{
{registers}
}}

}} // namespace sgrn::crud::generated
"""
