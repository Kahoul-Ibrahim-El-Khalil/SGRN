#include <sgrn/datastore/services/helpers/magic.hpp>
#include <algorithm>
#include <cctype>

namespace sgrn::datastore::services::helpers
{

static constexpr uint8_t PNG[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
static constexpr uint8_t JPEG[] = {0xFF, 0xD8, 0xFF};
static constexpr uint8_t GIF87[] = {0x47, 0x49, 0x46, 0x38, 0x37, 0x61};
static constexpr uint8_t GIF89[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61};
static constexpr uint8_t PDF[] = {0x25, 0x50, 0x44, 0x46};
static constexpr uint8_t ZIP[] = {0x50, 0x4B, 0x03, 0x04};
static constexpr uint8_t ZIP_EMPTY[] = {0x50, 0x4B, 0x05, 0x06};
static constexpr uint8_t ZIP_SPANNED[] = {0x50, 0x4B, 0x07, 0x08};
static constexpr uint8_t GZIP[] = {0x1F, 0x8B};
static constexpr uint8_t ZSTD[] = {0x28, 0xB5, 0x2F, 0xFD};
static constexpr uint8_t BZIP2[] = {0x42, 0x5A, 0x68};
static constexpr uint8_t XZ[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
static constexpr uint8_t LZ4[] = {0x04, 0x22, 0x4D, 0x18};
static constexpr uint8_t RAR[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
static constexpr uint8_t RAR5[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
static constexpr uint8_t SEVENZ[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
static constexpr uint8_t ELF[] = {0x7F, 0x45, 0x4C, 0x46};
static constexpr uint8_t MACHO_32[] = {0xFE, 0xED, 0xFA, 0xCE};
static constexpr uint8_t MACHO_64[] = {0xFE, 0xED, 0xFA, 0xCF};
static constexpr uint8_t MACHO_FAT[] = {0xCA, 0xFE, 0xBA, 0xBE};
static constexpr uint8_t PE[] = {0x4D, 0x5A};
static constexpr uint8_t CLASS[] = {0xCA, 0xFE, 0xBA, 0xBE};
static constexpr uint8_t DEX[] = {0x64, 0x65, 0x78, 0x0A};
static constexpr uint8_t SQLITE[] = {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00};
static constexpr uint8_t WEBP[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};
static constexpr uint8_t TIFF_II[] = {0x49, 0x49, 0x2A, 0x00};
static constexpr uint8_t TIFF_MM[] = {0x4D, 0x4D, 0x00, 0x2A};
static constexpr uint8_t BMP[] = {0x42, 0x4D};
static constexpr uint8_t ICO[] = {0x00, 0x00, 0x01, 0x00};
static constexpr uint8_t MP3_ID3[] = {0x49, 0x44, 0x33};
static constexpr uint8_t MP3_FRAME[] = {0xFF, 0xFB};
static constexpr uint8_t MP4[] = {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70};
static constexpr uint8_t MOV[] = {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70};
static constexpr uint8_t AVI[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x41, 0x56, 0x49, 0x20};
static constexpr uint8_t WAVE[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45};
static constexpr uint8_t FLAC[] = {0x66, 0x4C, 0x61, 0x43};
static constexpr uint8_t OGG[] = {0x4F, 0x67, 0x67, 0x53};
static constexpr uint8_t MIDI[] = {0x4D, 0x54, 0x68, 0x64};
static constexpr uint8_t TTF[] = {0x00, 0x01, 0x00, 0x00, 0x00};
static constexpr uint8_t OTF[] = {0x4F, 0x54, 0x54, 0x4F};
static constexpr uint8_t WOFF[] = {0x77, 0x4F, 0x46, 0x46};
static constexpr uint8_t WOFF2[] = {0x77, 0x4F, 0x46, 0x32};
static constexpr uint8_t XML[] = {0x3C, 0x3F, 0x78, 0x6D, 0x6C};
static constexpr uint8_t UTF8_BOM[] = {0xEF, 0xBB, 0xBF};
static constexpr uint8_t UTF16_LE_BOM[] = {0xFF, 0xFE};
static constexpr uint8_t UTF16_BE_BOM[] = {0xFE, 0xFF};
static constexpr uint8_t SHELL[] = {0x23, 0x21};
static constexpr uint8_t JAR_CLASS[] = {0xCA, 0xFE, 0xBA, 0xBE};
static constexpr uint8_t RTF[] = {0x7B, 0x5C, 0x72, 0x74, 0x66};

static const MagicSignature signatures[] = {
    {"png", PNG, sizeof(PNG), 0},
    {"jpg", JPEG, sizeof(JPEG), 0},
    {"jpeg", JPEG, sizeof(JPEG), 0},
    {"gif", GIF87, sizeof(GIF87), 0},
    {"gif", GIF89, sizeof(GIF89), 0},
    {"pdf", PDF, sizeof(PDF), 0},
    {"zip", ZIP, sizeof(ZIP), 0},
    {"zip", ZIP_EMPTY, sizeof(ZIP_EMPTY), 0},
    {"zip", ZIP_SPANNED, sizeof(ZIP_SPANNED), 0},
    {"gz", GZIP, sizeof(GZIP), 0},
    {"zst", ZSTD, sizeof(ZSTD), 0},
    {"bz2", BZIP2, sizeof(BZIP2), 0},
    {"xz", XZ, sizeof(XZ), 0},
    {"lz4", LZ4, sizeof(LZ4), 0},
    {"rar", RAR, sizeof(RAR), 0},
    {"rar", RAR5, sizeof(RAR5), 0},
    {"7z", SEVENZ, sizeof(SEVENZ), 0},
    {"elf", ELF, sizeof(ELF), 0},
    {"macho", MACHO_32, sizeof(MACHO_32), 0},
    {"macho", MACHO_64, sizeof(MACHO_64), 0},
    {"macho", MACHO_FAT, sizeof(MACHO_FAT), 0},
    {"exe", PE, sizeof(PE), 0},
    {"dll", PE, sizeof(PE), 0},
    {"class", CLASS, sizeof(CLASS), 0},
    {"dex", DEX, sizeof(DEX), 0},
    {"sqlite", SQLITE, sizeof(SQLITE), 0},
    {"sqlite3", SQLITE, sizeof(SQLITE), 0},
    {"db", SQLITE, sizeof(SQLITE), 0},
    {"webp", WEBP, sizeof(WEBP), 0},
    {"tiff", TIFF_II, sizeof(TIFF_II), 0},
    {"tiff", TIFF_MM, sizeof(TIFF_MM), 0},
    {"tif", TIFF_II, sizeof(TIFF_II), 0},
    {"bmp", BMP, sizeof(BMP), 0},
    {"ico", ICO, sizeof(ICO), 0},
    {"mp3", MP3_ID3, sizeof(MP3_ID3), 0},
    {"mp3", MP3_FRAME, sizeof(MP3_FRAME), 0},
    {"mp4", MP4, sizeof(MP4), 4},
    {"m4v", MP4, sizeof(MP4), 4},
    {"mov", MOV, sizeof(MOV), 4},
    {"avi", AVI, sizeof(AVI), 0},
    {"wav", WAVE, sizeof(WAVE), 0},
    {"flac", FLAC, sizeof(FLAC), 0},
    {"ogg", OGG, sizeof(OGG), 0},
    {"midi", MIDI, sizeof(MIDI), 0},
    {"mid", MIDI, sizeof(MIDI), 0},
    {"ttf", TTF, sizeof(TTF), 0},
    {"otf", OTF, sizeof(OTF), 0},
    {"woff", WOFF, sizeof(WOFF), 0},
    {"woff2", WOFF2, sizeof(WOFF2), 0},
    {"xml", XML, sizeof(XML), 0},
    {"xml", UTF8_BOM, sizeof(UTF8_BOM), 0},
    {"txt", UTF8_BOM, sizeof(UTF8_BOM), 0},
    {"txt", UTF16_LE_BOM, sizeof(UTF16_LE_BOM), 0},
    {"txt", UTF16_BE_BOM, sizeof(UTF16_BE_BOM), 0},
    {"sh", SHELL, sizeof(SHELL), 0},
    {"bash", SHELL, sizeof(SHELL), 0},
    {"zsh", SHELL, sizeof(SHELL), 0},
    {"jar", JAR_CLASS, sizeof(JAR_CLASS), 0},
    {"war", JAR_CLASS, sizeof(JAR_CLASS), 0},
    {"ear", JAR_CLASS, sizeof(JAR_CLASS), 0},
    {"rtf", RTF, sizeof(RTF), 0},
};

const std::vector<MagicSignature>& getMagicSignatures() {
    static const std::vector<MagicSignature> vec(std::begin(signatures), std::end(signatures));
    return vec;
}

static bool matchPattern(const std::string& data, const uint8_t* pattern, size_t pattern_len, size_t offset) {
    if (data.size() < offset + pattern_len)
        return false;
    return std::equal(pattern, pattern + pattern_len, reinterpret_cast<const uint8_t*>(data.data()) + offset);
}

std::optional<std::string> sniffExtension(const std::string& t_data) {
    for (const auto& sig : getMagicSignatures()) {
        if (matchPattern(t_data, sig.pattern, sig.pattern_len, sig.offset)) {
            return std::string(sig.extension);
        }
    }
    return std::nullopt;
}

} // namespace sgrn::datastore::services::helpers