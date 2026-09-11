/*
 * Copyright (C) 2026 Kahoul Ibrahim El-Khalil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

/**
 * @file opcua_types.hpp
 * @brief Independent mapping of S7 types to OPC UA types.
 *
 * This header acts as the translation layer between the S7 semantic type system
 * and standard OPC UA Type IDs, ensuring portability by avoiding dependencies
 * on full OPC UA library binaries.
 */

#include "types.hpp"

namespace s7codec::opcua
{

/**
 * @brief Standard OPC UA Type IDs mapping.
 */
enum class OpcUaType {
    Boolean,
    SByte,
    Byte,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    DateTime,
    Guid,
    ByteString,
    XmlElement,
    NodeId,
    ExpandedNodeId,
    StatusCode,
    QualifiedName,
    LocalizedText,
    ExtensionObject,
    DataValue,
    Variant,
    DiagnosticInfo
};

/**
 * @brief Maps an Type to a corresponding OPC UA Type.
 */
inline OpcUaType mapToOpcUa(Type t) {
    switch (t) {
        case Type::Bool:
            return OpcUaType::Boolean;
        case Type::Byte:
            return OpcUaType::Byte;
        case Type::Int:
            return OpcUaType::Int16;
        case Type::DInt:
            return OpcUaType::Int32;
        case Type::Word:
            return OpcUaType::UInt16;
        case Type::DWord:
            return OpcUaType::UInt32;
        case Type::Real:
            return OpcUaType::Float;
        case Type::String:
            return OpcUaType::String;
        case Type::WString:
            return OpcUaType::String;
        default:
            return OpcUaType::Variant;
    }
}

} // namespace s7codec::opcua
