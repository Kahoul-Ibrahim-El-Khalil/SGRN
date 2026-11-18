// Unit tests for backup primitives (TarWriter, Sha512Stream, directory hygiene, permissions).
//
// Ensures that backup archives generated for PostgreSQL and Garage conform
// to POSIX ustar specifications, have exact padding and zero blocks, stream
// SHA-512 digests accurately, and clean stale tmp files.

#include <fmt/core.h>
#include <sgrn/datastore/bootstrap/backup.hpp>
#include <sgrn/utils/hashing.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>
namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

namespace fs = std::filesystem;
using sgrn::datastore::bootstrap::backup::formatBytes;
using sgrn::datastore::bootstrap::backup::formatDuration;
using sgrn::datastore::bootstrap::backup::prepareBackupDir;
using sgrn::datastore::bootstrap::backup::ProgressMeter;
using sgrn::datastore::bootstrap::backup::publishArtifact;
using sgrn::datastore::bootstrap::backup::Sha512Stream;
using sgrn::datastore::bootstrap::backup::TarWriter;

void testFormattingAndProgress() {
    // formatDuration
    CHECK(formatDuration(std::chrono::seconds(5)) == "00m 05s");
    CHECK(formatDuration(std::chrono::seconds(75)) == "01m 15s");
    CHECK(formatDuration(std::chrono::seconds(3665)) == "1h 01m 05s");

    // formatBytes
    CHECK(formatBytes(500) == "500 B");
    CHECK(formatBytes(2048) == "2.0 KB");
    CHECK(formatBytes(15 * 1024 * 1024) == "15.0 MB");
    CHECK(formatBytes(3ULL * 1024 * 1024 * 1024) == "3.00 GB");

    // ProgressMeter: test non-TTY updates and completion
    ProgressMeter meter(10, 1000);
    meter.update(5, 500, true);
    meter.finish(10, 1000);
}

void testSha512Stream() {
    // 1. Empty string digest
    {
        Sha512Stream s;
        CHECK(s.hex() == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                         "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    }

    // 2. Known test vector: "The quick brown fox jumps over the lazy dog"
    {
        Sha512Stream s;
        const std::string text = "The quick brown fox jumps over the lazy dog";
        s.update(text.data(), text.size());
        CHECK(s.hex() == "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb64"
                         "2e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6");
    }

    // 3. Piecewise streaming
    {
        Sha512Stream s;
        s.update("The quick ", 10);
        s.update("brown fox ", 10);
        s.update("jumps over the lazy dog", 23);
        CHECK(s.hex() == "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb64"
                         "2e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6");
    }
}

void testTarWriterAndHygiene() {
    const fs::path test_base = fs::temp_directory_path() / fmt::format("sgrn_backup_test_{}", ::getpid());
    fs::create_directories(test_base);

    // Prepare backup dir
    const fs::path backup_dir = prepareBackupDir(test_base.string());
    CHECK(fs::exists(backup_dir));

    // Stale tmp cleanup check: create an orphan .tmp and a finished file
    const fs::path orphan_tmp = backup_dir / ".sgrn-stale.1234.tmp";
    const fs::path keeper = backup_dir / "sgrn-bck-keeper.pg";
    {
        std::ofstream(orphan_tmp) << "orphan";
        std::ofstream(keeper) << "keeper";
    }
    CHECK(fs::exists(orphan_tmp));
    CHECK(fs::exists(keeper));

    // Calling prepareBackupDir again should delete orphan_tmp but preserve keeper
    prepareBackupDir(test_base.string());
    CHECK(!fs::exists(orphan_tmp));
    CHECK(fs::exists(keeper));

    // Create scratch source files to add to a tar archive
    const fs::path src1 = backup_dir / "src1.bin";
    const fs::path src2 = backup_dir / "src2.bin";
    {
        std::ofstream out(src1, std::ios::binary);
        out << "Hello from source 1! This is not 512 bytes.";
    }
    {
        std::ofstream out(src2, std::ios::binary);
        std::string pattern(1024, 'X');
        out << pattern;
    }

    const fs::path tar_tmp = backup_dir / ".test_archive.tmp";
    const fs::path tar_final = backup_dir / "test_archive.tar";

    Sha512Stream tar_digest;
    {
        TarWriter writer(tar_tmp);
        CHECK(writer.ok());

        // File 1: short path, size < 512
        CHECK(writer.addFile("sgrn-uploads/file1.txt", src1, fs::file_size(src1), tar_digest));

        // File 2: partitioned path (prefix > 0, name <= 100), size == 1024
        CHECK(writer.addFile("sgrn-uploads/subfolder/nested/part_002.dat", src2, fs::file_size(src2), tar_digest));

        // File 3: long name (> 100 chars, testing ustar / LongLink)
        std::string long_member = "sgrn-uploads/longprefix/";
        while (long_member.size() < 120) {
            long_member += "abcdef1234567890/";
        }
        long_member += "chunk.bin";
        CHECK(writer.addFile(long_member, src1, fs::file_size(src1), tar_digest));

        CHECK(writer.finalize(tar_digest));
    }

    const std::string computed_hash = tar_digest.hex();

    // Verify tar file properties
    CHECK(fs::exists(tar_tmp));
    const auto tar_size = fs::file_size(tar_tmp);
    // Every POSIX tar must be a multiple of 512 bytes
    CHECK(tar_size > 0);
    CHECK(tar_size % 512 == 0);

    // Verify streamed digest against independent file hash
    auto file_digest = sgrn::utils::computeSha512File(tar_tmp, sgrn::utils::HashEncoding::base64url);
    CHECK(file_digest.hasValue());
    CHECK(file_digest.value() == computed_hash);

    // Promote via publishArtifact
    CHECK(publishArtifact(tar_tmp, tar_final));
    CHECK(!fs::exists(tar_tmp));
    CHECK(fs::exists(tar_final));

    // Check file permissions (owner only 0600)
    std::error_code ec;
    auto perms = fs::status(tar_final, ec).permissions();
    CHECK((perms & fs::perms::owner_read) != fs::perms::none);
    CHECK((perms & fs::perms::owner_write) != fs::perms::none);
    CHECK((perms & fs::perms::group_all) == fs::perms::none);
    CHECK((perms & fs::perms::others_all) == fs::perms::none);

    // Verify tar archive integrity with standard GNU tar
    const std::string tar_check_cmd = fmt::format("tar -tf {} > /dev/null 2>&1", tar_final.string());
    const int tar_rc = std::system(tar_check_cmd.c_str());
    CHECK(tar_rc == 0);

    // Cleanup test artifacts
    fs::remove_all(test_base, ec);
}

} // namespace

int main() {
    testFormattingAndProgress();
    testSha512Stream();
    testTarWriterAndHygiene();

    if (g_failures == 0) {
        std::printf("backup_test: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("backup_test: %d FAILURES\n", g_failures);
    return 1;
}
