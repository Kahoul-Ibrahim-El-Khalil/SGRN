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
 * @file  s7/datetime.hpp
 * @brief S7 date and time types (DTL, DT, Date, TOD, etc.).
 */

#pragma once

#include "endian.hpp"
#include <ctime>

namespace s7codec
{

// ---------------------------------------------------------------------------
// DTL — Date and Time Long (12 bytes)
// ---------------------------------------------------------------------------

struct alignas(2) DTL {
    BigEndian<uint16_t> year{};
    uint8_t month{1};
    uint8_t day{1};
    uint8_t weekday{1}; // 1=Sunday
    uint8_t hour{0};
    uint8_t minute{0};
    uint8_t second{0};
    BigEndian<uint32_t> nanosecond{};

    DTL() = default;
    explicit DTL(const struct tm& ti, uint32_t ns = 0) {
        fromTm(ti, ns);
    }

    DTL& operator=(const struct tm& ti) {
        fromTm(ti, 0);
        return *this;
    }

    struct tm toTm() const {
        struct tm ti{};
        ti.tm_year = static_cast<uint16_t>(year) - 1900;
        ti.tm_mon = month - 1;
        ti.tm_mday = day;
        ti.tm_wday = weekday - 1;
        ti.tm_hour = hour;
        ti.tm_min = minute;
        ti.tm_sec = second;
        return ti;
    }

    std::string toString() const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%09u", static_cast<uint16_t>(year), static_cast<uint8_t>(month),
            static_cast<uint8_t>(day), static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(second),
            static_cast<uint32_t>(nanosecond));
        return std::string(buf);
    }

private:
    void fromTm(const struct tm& ti, uint32_t ns) {
        year = static_cast<uint16_t>(ti.tm_year + 1900);
        month = static_cast<uint8_t>(ti.tm_mon + 1);
        day = static_cast<uint8_t>(ti.tm_mday);
        weekday = static_cast<uint8_t>(ti.tm_wday + 1);
        hour = static_cast<uint8_t>(ti.tm_hour);
        minute = static_cast<uint8_t>(ti.tm_min);
        second = static_cast<uint8_t>(ti.tm_sec);
        nanosecond = ns;
    }
};

// ---------------------------------------------------------------------------
// Date — days since 1990-01-01 (2 bytes)
// ---------------------------------------------------------------------------

struct alignas(2) Date : BigEndian<uint16_t> {
    using Base = BigEndian<uint16_t>;
    using Base::operator=;

    Date()
        : Base(0) {
    }
    explicit Date(uint16_t days)
        : Base(days) {
    }
    explicit Date(const struct tm& ti)
        : Base(daysSince1990(ti)) {
    }

    Date& operator=(const struct tm& ti) {
        *this = Base(daysSince1990(ti));
        return *this;
    }

    std::string toString() const {
        struct tm ti = toTm();
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        return std::string(buf);
    }

    struct tm toTm() const {
        struct tm epoch{};
        epoch.tm_year = 90;
        epoch.tm_mon = 0;
        epoch.tm_mday = 1 + get();
        std::mktime(&epoch);
        return epoch;
    }

private:
    static uint16_t daysSince1990(const struct tm& ti) {
        struct tm epoch{};
        epoch.tm_year = 90;
        epoch.tm_mon = 0;
        epoch.tm_mday = 1;
        time_t ref = mktime(&epoch);
        struct tm copy = ti;
        time_t t = mktime(&copy);
        return static_cast<uint16_t>((t - ref) / 86400L);
    }
};

// ---------------------------------------------------------------------------
// TOD — Time of Day (4 bytes, ms since midnight)
// ---------------------------------------------------------------------------

struct alignas(2) TOD : BigEndian<uint32_t> {
    using Base = BigEndian<uint32_t>;
    using Base::operator=;

    TOD()
        : Base(0) {
    }
    explicit TOD(uint32_t ms)
        : Base(ms) {
    }
    explicit TOD(const struct tm& ti)
        : Base(fromTm(ti)) {
    }

    TOD& operator=(const struct tm& ti) {
        *this = Base(fromTm(ti));
        return *this;
    }

    std::string toString() const {
        uint32_t ms = get();
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", ms / 3600000u, (ms / 60000u) % 60u, (ms / 1000u) % 60u, ms % 1000u);
        return std::string(buf);
    }

private:
    static uint32_t fromTm(const struct tm& ti) {
        return static_cast<uint32_t>(ti.tm_hour * 3600000u + ti.tm_min * 60000u + ti.tm_sec * 1000u);
    }
};

/**
 * @brief S7-1500 LTime (64-bit nanoseconds).
 */
struct LTime : BigEndian<int64_t> {
    using Base = BigEndian<int64_t>;
    using Base::operator=;
    LTime()
        : Base(0) {
    }
    explicit LTime(int64_t ns)
        : Base(ns) {
    }
    std::string toString() const {
        int64_t ns = get();
        // Simple human-readable format: [+/-]DDd HH:MM:SS.ns
        bool neg = ns < 0;
        uint64_t abs_ns = neg ? static_cast<uint64_t>(-ns) : static_cast<uint64_t>(ns);
        uint64_t sec = abs_ns / 1000000000ULL;
        uint64_t nano = abs_ns % 1000000000ULL;
        uint32_t d = static_cast<uint32_t>(sec / 86400);
        uint32_t h = static_cast<uint32_t>((sec / 3600) % 24);
        uint32_t m = static_cast<uint32_t>((sec / 60) % 60);
        uint32_t s = static_cast<uint32_t>(sec % 60);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%ud %02u:%02u:%02u.%09llu", neg ? "-" : "", d, h, m, s, (unsigned long long)nano);
        return std::string(buf);
    }
};

/**
 * @brief S7-1500 LTimeOfDay (64-bit nanoseconds since midnight).
 */
struct LTOD : BigEndian<uint64_t> {
    using Base = BigEndian<uint64_t>;
    using Base::operator=;
    LTOD()
        : Base(0) {
    }
    explicit LTOD(uint64_t ns)
        : Base(ns) {
    }
    std::string toString() const {
        uint64_t ns = get();
        uint64_t sec = ns / 1000000000ULL;
        uint64_t nano = ns % 1000000000ULL;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%09llu", static_cast<uint32_t>(sec / 3600), static_cast<uint32_t>((sec / 60) % 60),
            static_cast<uint32_t>(sec % 60), (unsigned long long)nano);
        return std::string(buf);
    }
};

/**
 * @brief S7-300/400 DATE_AND_TIME (8-byte BCD).
 */
struct alignas(2) DateTime {
    uint8_t data[8];

    std::string toString() const {
        auto bcd = [](uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); };
        int year = bcd(data[0]);
        year += (year < 90) ? 2000 : 1900;
        int month = bcd(data[1]);
        int day = bcd(data[2]);
        int hour = bcd(data[3]);
        int min = bcd(data[4]);
        int sec = bcd(data[5]);
        int msec = (bcd(data[6]) * 10) + (data[7] >> 4);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", year, month, day, hour, min, sec, msec);
        return std::string(buf);
    }
};

static_assert(sizeof(Date) == 2, "Date size mismatch");
static_assert(sizeof(TOD) == 4, "TOD size mismatch");
static_assert(sizeof(LTime) == 8, "LTime size mismatch");
static_assert(sizeof(LTOD) == 8, "LTOD size mismatch");
static_assert(sizeof(DateTime) == 8, "DateTime size mismatch");
static_assert(sizeof(DTL) == 12, "DTL size mismatch");

} // namespace s7codec
