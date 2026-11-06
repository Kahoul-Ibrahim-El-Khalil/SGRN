#pragma once
/**
 * @file  sgrn/utils/tar.hpp
 * @brief Lightweight POSIX ustar tar archive writer.
 *
 * Implements POSIX ustar tar header generation with GNU 'L' longname
 * extension for paths exceeding ustar limits. Integrates with Sha256Stream
 * for simultaneous digest calculation during archive writing.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <sgrn/utils/hashing.hpp>

namespace sgrn::utils::tar
{

constexpr size_t kTarBlockSize = 512;

/**
 * @brief POSIX ustar header + GNU 'L' longname fallback for paths > 255 chars.
 */
class TarWriter {
public:
    explicit TarWriter(const std::filesystem::path& t_tmp)
        : out_(t_tmp, std::ios::binary | std::ios::trunc) {
    }

    bool ok() const {
        return static_cast<bool>(out_);
    }

    /// Append one regular file (source stays on disk; we stream chunks).
    bool addFile(const std::string& t_member_path, const std::filesystem::path& t_source_file, std::uintmax_t t_size,
        sgrn::utils::Sha256Stream& t_tar_digest) {
        if (t_member_path.size() > kUstarMaxPath) {
            if (!appendLongNameEntry(t_member_path, t_tar_digest)) {
                return false;
            }
            return appendBody(t_member_path.substr(0, 100), t_source_file, t_size, t_tar_digest);
        }
        const auto split = splitUstarPath(t_member_path);
        if (split.has_value()) {
            return appendBody(t_member_path, t_source_file, t_size, t_tar_digest, split->first, split->second);
        }
        // ustar cannot represent it (e.g. > 255 total or bad split point)
        if (!appendLongNameEntry(t_member_path, t_tar_digest)) {
            return false;
        }
        return appendBody(t_member_path.substr(0, 100), t_source_file, t_size, t_tar_digest);
    }

    /// Two zero blocks close the archive.
    bool finalize(sgrn::utils::Sha256Stream& t_tar_digest) {
        static const std::array<char, kTarBlockSize * 2> kZeros{};
        if (!out_.write(kZeros.data(), kZeros.size())) {
            return false;
        }
        t_tar_digest.update(kZeros.data(), kZeros.size());
        out_.flush();
        return static_cast<bool>(out_);
    }

private:
    static constexpr size_t kUstarMaxPath = 255; // prefix(155) + '/' + name(100)

    static std::optional<std::pair<std::string, std::string>> splitUstarPath(const std::string& t_path) {
        if (t_path.size() <= 100) {
            return std::make_pair(std::string(), std::string()); // no prefix, plain name
        }
        if (t_path.size() > kUstarMaxPath) {
            return std::nullopt;
        }
        // Split at last '/' such that prefix <= 155 and remainder <= 100.
        const size_t cut = t_path.size() < 101 ? std::string::npos : t_path.size() - 101;
        size_t split_pos = t_path.find_last_of('/', std::min(cut, t_path.size() - 1));
        while (split_pos != std::string::npos) {
            const std::string prefix = t_path.substr(0, split_pos);
            const std::string name = t_path.substr(split_pos + 1);
            if (prefix.size() <= 155 && name.size() <= 100 && !name.empty()) {
                return std::make_pair(prefix, name);
            }
            split_pos = split_pos == 0 ? std::string::npos : t_path.find_last_of('/', split_pos - 1);
        }
        return std::nullopt;
    }

    static void putOctal(char* t_dst, size_t t_field, std::uint64_t t_value) {
        if (t_field == 0) {
            return;
        }
        std::snprintf(t_dst, t_field, "%0*llo", static_cast<int>(t_field - 1), static_cast<long long unsigned>(t_value));
        t_dst[t_field - 1] = 0;
    }

    static void putString(char* t_dst, size_t t_field, const std::string& t_value) {
        const size_t n = std::min(t_field - 1, t_value.size());
        std::memcpy(t_dst, t_value.data(), n);
        std::memset(t_dst + n, 0, t_field - n);
    }

    std::array<char, kTarBlockSize> buildHeader(
        const std::string& t_name, std::uint64_t t_size, char t_typeflag, const std::string& t_prefix = {}) {
        std::array<char, kTarBlockSize> h{};
        putString(h.data(), 100, t_name);         // name
        putOctal(h.data() + 100, 8, 0644);        // mode
        putOctal(h.data() + 108, 8, 0);           // uid
        putOctal(h.data() + 116, 8, 0);           // gid
        putOctal(h.data() + 124, 12, t_size);     // size
        putOctal(h.data() + 136, 12, mtime_);     // mtime
        std::memset(h.data() + 148, ' ', 8);      // chksum placeholder (spaces)
        h[156] = t_typeflag;                      // typeflag
        std::memcpy(h.data() + 257, "ustar", 5);  // magic + NUL
        std::memcpy(h.data() + 263, "00", 2);     // version
        putString(h.data() + 265, 32, "");        // uname
        putString(h.data() + 297, 32, "");        // gname
        putString(h.data() + 329, 155, t_prefix); // prefix
        unsigned int sum = 0;
        for (const char c : h) {
            sum += static_cast<unsigned char>(c);
        }
        putOctal(h.data() + 148, 7, sum);
        h[155] = ' '; // 6 octal digits + NUL + space
        return h;
    }

    bool writeBlock(const std::array<char, kTarBlockSize>& t_block, sgrn::utils::Sha256Stream& t_digest) {
        if (!out_.write(t_block.data(), t_block.size())) {
            return false;
        }
        t_digest.update(t_block.data(), t_block.size());
        return true;
    }

    bool appendLongNameEntry(const std::string& t_path, sgrn::utils::Sha256Stream& t_digest) {
        std::string payload = t_path;
        payload.push_back('\0');
        const std::array<char, kTarBlockSize> header = buildHeader("././@LongLink", payload.size(), 'L');
        if (!writeBlock(header, t_digest)) {
            return false;
        }
        size_t written = 0;
        while (written < payload.size()) {
            std::array<char, kTarBlockSize> buf{};
            const size_t n = std::min(payload.size() - written, kTarBlockSize);
            std::memcpy(buf.data(), payload.data() + written, n);
            if (!writeBlock(buf, t_digest)) {
                return false;
            }
            written += n;
        }
        return true;
    }

    bool appendBody(const std::string& t_name, const std::filesystem::path& t_source, std::uintmax_t t_size,
        sgrn::utils::Sha256Stream& t_digest, const std::string& t_prefix = {}, const std::string& t_split_name = {}) {
        std::ifstream in(t_source, std::ios::binary);
        if (!in) {
            return false;
        }
        const std::array<char, kTarBlockSize> header = buildHeader(t_prefix.empty() ? t_name : t_split_name, t_size, '0', t_prefix);
        if (!writeBlock(header, t_digest)) {
            return false;
        }
        std::array<char, kTarBlockSize> buf{}; // zero-filled: partial reads stay zero padding
        std::uintmax_t remaining = t_size;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<std::uintmax_t>(remaining, kTarBlockSize));
            in.read(buf.data(), static_cast<std::streamsize>(want));
            if (in.gcount() != static_cast<std::streamsize>(want)) {
                return false;
            }
            // Emit exactly `want` bytes; the pad branch below closes the
            // block. Writing the full 512 buffer here would double the
            // padding and misalign every subsequent member offset.
            if (!out_.write(buf.data(), static_cast<std::streamsize>(want))) {
                return false;
            }
            t_digest.update(buf.data(), want);
            remaining -= want;
        }
        if (t_size % kTarBlockSize != 0) {
            std::array<char, kTarBlockSize> pad{};
            const size_t tail = kTarBlockSize - (t_size % kTarBlockSize);
            if (!out_.write(pad.data(), tail)) {
                return false;
            }
            t_digest.update(pad.data(), tail);
        }
        return true;
    }

    std::ofstream out_;
    std::uint64_t mtime_ = static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
};

} // namespace sgrn::utils::tar
