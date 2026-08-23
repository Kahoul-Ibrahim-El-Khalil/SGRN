#!/usr/bin/env python3

import asyncio
import dataclasses
import enum
import inspect
import sys
from typing import Optional


# =============================================================================
# asyncua / Python 3.14 compatibility patch
#
# asyncua 1.1.8 can encounter a Python `property` object while building its
# binary serializers on Python 3.14. This happens before the OPC UA connection
# is established. The upstream project tracks this as issue #1880.
#
# The patch below:
#   1. makes issubclass() safe for non-class inputs
#   2. filters property-typed dataclass fields from asyncua's serializer input
#
# This keeps the test usable on Python 3.14 without modifying site-packages.
# =============================================================================

try:
    import asyncua.ua.ua_binary as ua_binary

    # -------------------------------------------------------------------------
    # 1. Safe issubclass
    # -------------------------------------------------------------------------

    _original_issubclass = issubclass

    def _safe_issubclass(cls, classinfo):
        if not isinstance(cls, type):
            return False
        return _original_issubclass(cls, classinfo)

    ua_binary.issubclass = _safe_issubclass

    # -------------------------------------------------------------------------
    # 2. Patch create_dataclass_serializer()
    #
    # asyncua 1.1.8 roughly does:
    #
    #   data_fields = dataclasses.fields(dataclazz)
    #   field_serializer(f.type, ...)
    #
    # Python 3.14 can expose a property descriptor in that field list/type.
    # Filter those before asyncua tries to build a UA binary serializer.
    # -------------------------------------------------------------------------

    _original_create_dataclass_serializer = (
        ua_binary.create_dataclass_serializer
    )

    def _patched_create_dataclass_serializer(dataclazz):
        data_fields = []

        for field in dataclasses.fields(dataclazz):
            field_type = field.type

            # Python 3.14 compatibility problem seen in asyncua 1.1.8.
            if isinstance(field_type, property):
                continue

            data_fields.append(field)

        # Preserve asyncua's serializer implementation as closely as possible.
        encoding_functions = [
            (
                field.name,
                ua_binary.field_serializer(field.type, dataclazz),
            )
            for field in data_fields
        ]

        # The exact serializer class used by asyncua 1.1.8.
        # It is intentionally obtained from the module rather than recreated.
        serializer_class = getattr(
            ua_binary,
            "DataclassSerializer",
            None,
        )

        if serializer_class is not None:
            return serializer_class(
                dataclazz,
                encoding_functions,
            )

        # Some asyncua releases use a dynamically generated callable rather
        # than a public DataclassSerializer class. In that case, reproduce the
        # serializer behavior expected by struct_to_binary().
        def serializer(obj):
            result = bytearray()

            for field_name, field_serializer in encoding_functions:
                value = getattr(obj, field_name)
                result.extend(field_serializer(value))

            return bytes(result)

        return serializer

    ua_binary.create_dataclass_serializer = (
        _patched_create_dataclass_serializer
    )

    print(
        f"✅ asyncua Python 3.14 compatibility patch installed "
        f"(asyncua={getattr(__import__('asyncua'), '__version__', 'unknown')})"
    )

except Exception as exc:
    print(
        f"⚠️ Could not install asyncua Python 3.14 compatibility patch: {exc}",
        file=sys.stderr,
    )


from asyncua import Client, ua


# =============================================================================
# Configuration
# =============================================================================

URL = "opc.tcp://127.0.0.1:4840"

# SGRN normally uses namespace 1 for the application information model.
APPLICATION_NAMESPACE = 1


# =============================================================================
# Formatting helpers
# =============================================================================

def nodeid_to_str(nodeid: ua.NodeId) -> str:
    try:
        return nodeid.to_string()
    except Exception:
        return str(nodeid)


async def read_browse_name(node) -> str:
    try:
        browse_name = await node.read_browse_name()
        return browse_name.Name
    except Exception:
        return "<?>"


async def read_display_name(node) -> str:
    try:
        display_name = await node.read_display_name()
        return display_name.Text or ""
    except Exception:
        return ""


async def read_node_class(node):
    try:
        return await node.read_node_class()
    except Exception:
        return None


async def read_value(node):
    try:
        return await node.read_value()
    except Exception:
        return None


async def read_data_type(node):
    try:
        return await node.read_data_type()
    except Exception:
        return None


async def read_value_rank(node):
    try:
        return await node.read_value_rank()
    except Exception:
        return None


async def read_array_dimensions(node):
    try:
        return await node.read_array_dimensions()
    except Exception:
        return None


# =============================================================================
# Generic value printing
# =============================================================================

def print_value(value, indent=""):
    if isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            print(f"{indent}[{index}]")
            print_value(item, indent + "  ")
        return

    if isinstance(value, enum.Enum):
        print(f"{indent}{value.name} ({value.value})")
        return

    if dataclasses.is_dataclass(value):
        print(f"{indent}{type(value).__name__}")
        for field in dataclasses.fields(value):
            try:
                field_value = getattr(value, field.name)
            except Exception:
                field_value = "<unreadable>"

            print(f"{indent}  {field.name} = ", end="")
            if isinstance(field_value, (list, tuple)) or dataclasses.is_dataclass(field_value):
                print()
                print_value(field_value, indent + "    ")
            else:
                print(repr(field_value))
        return

    print(f"{indent}{value!r}")


# =============================================================================
# DataTypeDefinition inspection
# =============================================================================

def inspect_extension_object(value, indent=""):
    """
    Print UA StructureDefinition / EnumDefinition and related ExtensionObjects
    returned by DataTypeDefinition.
    """

    if value is None:
        print(f"{indent}<none>")
        return

    if isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            print(f"{indent}[{index}]")
            inspect_extension_object(item, indent + "  ")
        return

    if isinstance(value, ua.EnumDefinition):
        print(f"{indent}EnumDefinition")

        fields = getattr(value, "Fields", [])

        for field in fields:
            name = getattr(field, "Name", "<unnamed>")
            numeric_value = getattr(field, "Value", "?")

            print(
                f"{indent}  {name} = {numeric_value}"
            )

        return

    if isinstance(value, ua.StructureDefinition):
        print(f"{indent}StructureDefinition")

        base_type = getattr(value, "BaseDataType", None)
        structure_type = getattr(value, "StructureType", None)

        if base_type is not None:
            print(
                f"{indent}  BaseDataType: "
                f"{nodeid_to_str(base_type)}"
            )

        if structure_type is not None:
            print(
                f"{indent}  StructureType: "
                f"{structure_type}"
            )

        fields = getattr(value, "Fields", [])

        for field in fields:
            field_name = getattr(field, "Name", "<unnamed>")
            field_type = getattr(field, "DataType", None)
            value_rank = getattr(field, "ValueRank", None)

            print(
                f"{indent}  field: {field_name}"
            )

            if field_type is not None:
                print(
                    f"{indent}    DataType: "
                    f"{nodeid_to_str(field_type)}"
                )

            if value_rank is not None:
                print(
                    f"{indent}    ValueRank: "
                    f"{value_rank}"
                )

        return

    print(f"{indent}{value!r}")


async def inspect_datatype_definition(node, indent=""):
    try:
        attribute = await node.read_attribute(
            ua.AttributeIds.DataTypeDefinition
        )

        value = attribute.Value

        if value is None:
            print(f"{indent}DataTypeDefinition: <none>")
            return

        print(f"{indent}DataTypeDefinition:")
        inspect_extension_object(
            value,
            indent + "  ",
        )

    except Exception as exc:
        print(
            f"{indent}DataTypeDefinition: <unavailable: {exc}>"
        )


# =============================================================================
# Enum property inspection
# =============================================================================

async def inspect_enum_properties(node, indent=""):
    try:
        children = await node.get_children()
    except Exception:
        return

    for child in children:
        name = await read_browse_name(child)

        if name not in {"EnumStrings", "EnumValues"}:
            continue

        print(f"{indent}{name}:")

        value = await read_value(child)

        if value is None:
            print(f"{indent}  <unreadable>")
        else:
            print_value(
                value,
                indent + "  ",
            )


# =============================================================================
# Variable inspection
# =============================================================================

async def inspect_variable(node, indent=""):
    name = await read_browse_name(node)

    print(
        f"{indent}🔹 Variable: {name}"
    )

    print(
        f"{indent}   NodeId: "
        f"{nodeid_to_str(node.nodeid)}"
    )

    datatype = await read_data_type(node)

    if datatype is not None:
        print(
            f"{indent}   DataType: "
            f"{nodeid_to_str(datatype)}"
        )

    value_rank = await read_value_rank(node)

    if value_rank is not None:
        print(
            f"{indent}   ValueRank: "
            f"{value_rank}"
        )

    dimensions = await read_array_dimensions(node)

    if dimensions:
        print(
            f"{indent}   Dimensions: "
            f"{dimensions}"
        )

    value = await read_value(node)

    if value is not None:
        print(f"{indent}   Value:")
        print_value(
            value,
            indent + "      ",
        )


# =============================================================================
# Recursive information model walker
# =============================================================================

async def walk_information_model(
    node,
    depth=0,
    visited=None,
):
    if visited is None:
        visited = set()

    nodeid = nodeid_to_str(node.nodeid)

    # Avoid loops caused by OPC UA references.
    if nodeid in visited:
        return

    visited.add(nodeid)

    indent = "  " * depth

    node_class = await read_node_class(node)
    name = await read_browse_name(node)

    if node_class == ua.NodeClass.Variable:
        await inspect_variable(
            node,
            indent,
        )
    else:
        class_name = (
            node_class.name
            if node_class is not None
            else "Unknown"
        )

        print(
            f"{indent}📁 {name} [{class_name}]"
        )

        print(
            f"{indent}   NodeId: "
            f"{nodeid}"
        )

        display = await read_display_name(node)

        if display and display != name:
            print(
                f"{indent}   DisplayName: "
                f"{display}"
            )

    try:
        children = await node.get_children()
    except Exception as exc:
        print(
            f"{indent}   <browse failed: {exc}>"
        )
        return

    # Deterministic output.
    named_children = []

    for child in children:
        try:
            child_name = await read_browse_name(child)
        except Exception:
            child_name = "<?>"

        named_children.append(
            (child_name.lower(), child_name, child)
        )

    named_children.sort()

    for _, _, child in named_children:
        await walk_information_model(
            child,
            depth=depth + 1,
            visited=visited,
        )


# =============================================================================
# Full DataType tree
# =============================================================================

async def inspect_data_types(client):
    print()
    print("=" * 100)
    print(" OPC UA DATA TYPE MODEL")
    print("=" * 100)

    datatypes_node = client.get_node(
        ua.ObjectIds.DataTypes
    )

    await walk_datatype_tree(
        datatypes_node,
        depth=0,
        visited=set(),
    )


async def walk_datatype_tree(
    node,
    depth=0,
    visited=None,
):
    if visited is None:
        visited = set()

    nodeid = nodeid_to_str(node.nodeid)

    if nodeid in visited:
        return

    visited.add(nodeid)

    indent = "  " * depth

    name = await read_browse_name(node)

    print(
        f"{indent}📐 {name}"
    )

    print(
        f"{indent}   NodeId: "
        f"{nodeid}"
    )

    # Every non-standard custom datatype gets detailed inspection.
    try:
        if node.nodeid.NamespaceIndex != 0:
            await inspect_datatype_definition(
                node,
                indent + "   ",
            )

            await inspect_enum_properties(
                node,
                indent + "   ",
            )
    except Exception:
        pass

    try:
        children = await node.get_children()
    except Exception:
        return

    named_children = []

    for child in children:
        child_name = await read_browse_name(child)

        named_children.append(
            (
                child_name.lower(),
                child_name,
                child,
            )
        )

    named_children.sort()

    for _, _, child in named_children:
        await walk_datatype_tree(
            child,
            depth=depth + 1,
            visited=visited,
        )


# =============================================================================
# NamespaceArray
# =============================================================================

async def print_namespaces(client):
    print()
    print("=" * 100)
    print(" NAMESPACE ARRAY")
    print("=" * 100)

    try:
        node = client.get_node(
            ua.NodeId(
                ua.ObjectIds.Server_NamespaceArray
            )
        )

        namespaces = await node.read_value()

        for index, uri in enumerate(namespaces):
            print(
                f"  [{index}] {uri}"
            )

    except Exception as exc:
        print(
            f"Could not read NamespaceArray: {exc}"
        )


# =============================================================================
# Application enum audit
# =============================================================================

async def audit_application_datatypes(client):
    """
    Enumerate every application-namespace DataType and identify its UA
    DataTypeKind / Definition.

    This is particularly useful for your enum work:
      - enum aliases should appear as Enumeration
      - they should NOT appear as UInt16 / USInt / Byte variables
    """

    print()
    print("=" * 100)
    print(" APPLICATION DATA TYPE AUDIT")
    print("=" * 100)

    datatypes = client.get_node(
        ua.ObjectIds.DataTypes
    )

    results = []

    async def recurse(node):
        try:
            nodeid = node.nodeid

            if nodeid.NamespaceIndex == APPLICATION_NAMESPACE:
                name = await read_browse_name(node)

                try:
                    definition_attr = await node.read_attribute(
                        ua.AttributeIds.DataTypeDefinition
                    )
                    definition = definition_attr.Value
                except Exception:
                    definition = None

                kind = None

                if isinstance(
                    definition,
                    ua.EnumDefinition,
                ):
                    kind = "Enumeration"

                elif isinstance(
                    definition,
                    ua.StructureDefinition,
                ):
                    kind = "Structure"

                results.append(
                    (
                        name,
                        nodeid_to_str(nodeid),
                        kind,
                    )
                )

            children = await node.get_children()

            for child in children:
                await recurse(child)

        except Exception:
            return

    await recurse(datatypes)

    results.sort(
        key=lambda item: item[0].lower()
    )

    for name, nodeid, kind in results:
        kind_text = kind or "Unknown"

        print(
            f"  {name:<40} "
            f"{kind_text:<15} "
            f"{nodeid}"
        )


# =============================================================================
# Main
# =============================================================================

async def main():
    print("=" * 100)
    print(" SGRN OPC UA INFORMATION MODEL INSPECTOR")
    print("=" * 100)

    print(
        f"Python: {sys.version.split()[0]}"
    )

    print(
        f"Connecting to {URL}"
    )

    client = Client(
        url=URL,
    )

    try:
        await client.connect()

        print(
            "✅ Connected successfully"
        )

        await print_namespaces(
            client
        )

        # ---------------------------------------------------------------------
        # Entire Objects tree
        # ---------------------------------------------------------------------

        print()
        print("=" * 100)
        print(" OBJECTS / DIGITAL TWIN INFORMATION MODEL")
        print("=" * 100)

        objects = client.get_objects_node()

        await walk_information_model(
            objects,
            depth=0,
            visited=set(),
        )

        # ---------------------------------------------------------------------
        # Entire OPC UA DataTypes hierarchy
        # ---------------------------------------------------------------------

        await inspect_data_types(
            client
        )

        # ---------------------------------------------------------------------
        # Application type audit
        # ---------------------------------------------------------------------

        await audit_application_datatypes(
            client
        )

        print()
        print("=" * 100)
        print(" DISCOVERY COMPLETE")
        print("=" * 100)

    except Exception as exc:
        print()
        print("=" * 100)
        print(" ❌ DISCOVERY FAILED")
        print("=" * 100)
        print(
            f"{type(exc).__name__}: {exc}"
        )

        import traceback
        traceback.print_exc()

    finally:
        try:
            await client.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
