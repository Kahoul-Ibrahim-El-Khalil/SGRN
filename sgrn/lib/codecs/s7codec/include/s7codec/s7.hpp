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

/**
 * @file  s7codec/s7.hpp
 * @brief Master include for the S7 header-only library.
 *
 * Include this single header to get access to all S7 types,
 * big-endian utilities, and the scalar codec.
 *
 * This library is fully freestanding (C++17, no external dependencies)
 * and is designed to be shared between:
 *   - SGRN gateway toolset (Linux x86_64, C++20/23)
 *   - ESP32 embedded projects (C++17, Arduino / ESP-IDF)
 *
 * Namespace: s7codec::
 *
 * --- USAGE: Simulating a S7 PLC Memory Layout ---
 * To make your C++ application behave like a S7 PLC, define a packed struct
 * using the provided S7 type aliases and raw structural types:
 *
 * @code
 * struct __attribute__((packed)) MixerDB {
 *     Int   speed;           // Big-Endian int16_t
 *     Real  target_temp;     // Big-Endian float
 *     RawDTL last_update;    // 12-byte DTL structure
 *     String<16> status;  // S7-style String[16]
 * };
 *
 * void updateBuffer(uint8_t* raw_buffer) {
 *     auto* db = reinterpret_cast<MixerDB*>(raw_buffer);
 *
 *     // High-level assignments (automatically handle byte-swapping)
 *     db->speed = 1500;
 *     db->target_temp = 45.5f;
 *     db->last_update = RawDTL::now();
 *     db->status = "RUNNING";
 * }
 * @endcode
 */

#pragma once

#include "codec.hpp"
#include "datetime.hpp"
#include "endian.hpp"
#include "string.hpp"
#include "types.hpp"
