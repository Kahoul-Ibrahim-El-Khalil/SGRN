#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::utils::mime
{

enum class MimeType : uint16_t {
    Unknown = 0,
    // --- Text ---
    TextPlain,
    TextHtml,
    TextCss,
    TextCsv,
    TextXml,
    TextJavascript,
    TextMarkdown,
    TextYaml,
    TextVcard,
    TextCalendar,
    TextRichtext,
    TextCache,
    // --- Application / Docs ---
    ApplicationJson,
    ApplicationXml,
    ApplicationPdf,
    ApplicationRtf,
    ApplicationOctetStream,
    ApplicationFormUrlEncoded,
    ApplicationJavascript,
    ApplicationWasm,
    ApplicationLd,
    ApplicationSql,
    ApplicationGraphql,
    ApplicationManifest,
    ApplicationXhtml,
    ApplicationAtom,
    ApplicationRss,
    ApplicationSoap,
    // MS Office / OpenOffice
    DocWord,
    DocExcel,
    DocPowerpoint,
    DocxWord,
    XlsxExcel,
    PptxPowerpoint,
    DocAccess,
    DocxAccess,
    DocVisio,
    DocxVisio,
    DocProject,
    DocPublisher,
    DocOneNote,
    // OpenDocument Format
    OdtText,
    OdsSpreadsheet,
    OdpPresentation,
    OdgGraphics,
    OdfFormula,
    OdbDatabase,
    OdcChart,
    OdiImage,
    // --- Archives ---
    ArchiveZstd,
    ArchiveZip,
    ArchiveGzip,
    ArchiveTar,
    Archive7z,
    ArchiveRar,
    ArchiveBzip2,
    ArchiveXz,
    ArchiveLzma,
    ArchiveZ,
    ArchiveCab,
    ArchiveDmg,
    ArchiveIso,
    ArchiveJar,
    ArchiveWar,
    ArchiveEar,
    ArchiveDeb,
    ArchiveRpm,
    // --- Images ---
    ImagePng,
    ImageJpeg,
    ImageGif,
    ImageWebp,
    ImageSvg,
    ImageBmp,
    ImageIco,
    ImageTiff,
    ImageAvif,
    ImageHeic,
    ImageHeif,
    ImageJpeg2000,
    ImageJxl,
    ImagePsd,
    ImageXcf,
    ImageXbm,
    ImageXpm,
    ImagePnm,
    ImagePbm,
    ImagePgm,
    ImagePpm,
    ImageRgb,
    ImageCgm,
    ImageDjvu,
    ImageDds,
    ImageKtx,
    ImageApng,
    // --- Audio ---
    AudioMpeg,
    AudioOgg,
    AudioWav,
    AudioWebm,
    AudioAac,
    AudioFlac,
    AudioMidi,
    AudioOpus,
    AudioAmr,
    AudioAiff,
    AudioAu,
    AudioM4a,
    AudioWma,
    AudioRa,
    AudioApe,
    AudioAc3,
    AudioDts,
    AudioMpc,
    AudioSpeex,
    AudioVorbis,
    Audio3gpp,
    Audio3gpp2,
    // --- Video ---
    VideoMp4,
    VideoMpeg,
    VideoWebm,
    VideoOgg,
    VideoQuicktime,
    VideoAvi,
    VideoFlv,
    VideoMkv,
    VideoWmv,
    VideoAsf,
    VideoM4v,
    VideoMts,
    VideoMxf,
    VideoRm,
    VideoVob,
    Video3gpp,
    Video3gpp2,
    VideoTs,
    VideoF4v,
    VideoM2ts,
    VideoDivx,
    VideoXvid,
    // --- Multipart ---
    MultipartFormData,
    MultipartMixed,
    MultipartAlternative,
    MultipartRelated,
    MultipartDigest,
    MultipartParallel,
    MultipartByteranges,
    // --- Font ---
    FontWoff,
    FontWoff2,
    FontTtf,
    FontOtf,
    FontEot,
    FontCollection,
    FontSfnt,
    // --- Code / Programming ---
    CodeC,
    CodeCpp,
    CodeCsharp,
    CodeJava,
    CodePython,
    CodeRuby,
    CodePhp,
    CodeGo,
    CodeRust,
    CodeSwift,
    CodeKotlin,
    CodeScala,
    CodePerl,
    CodeLua,
    CodeShell,
    CodeBash,
    CodePowershell,
    CodeTypescript,
    CodeCoffee,
    CodeDart,
    CodeElixir,
    CodeErlang,
    CodeHaskell,
    CodeClojure,
    CodeAsm,
    CodeFortran,
    CodeCobol,
    CodeVb,
    CodeMatlab,
    CodeR,
    CodeSql,
    // --- Siemens Industrial ---
    CodeScl,
    DataSiemensDb,
    DataSiemensUdt,
    // --- Config / Data ---
    ConfigToml,
    ConfigIni,
    ConfigProperties,
    ConfigEnv,
    ConfigYml,
    ConfigHocon,
    ConfigEditorconfig,
    // --- eBooks ---
    EbookEpub,
    EbookMobi,
    EbookAzw,
    EbookAzw3,
    EbookDjvu,
    EbookFb2,
    EbookLit,
    // --- CAD / 3D ---
    ModelStl,
    ModelObj,
    ModelFbx,
    ModelDae,
    ModelGltf,
    ModelGlb,
    Model3ds,
    ModelPly,
    ModelStep,
    ModelIges,
    ModelDwg,
    ModelDxf,
    ModelX3d,
    ModelU3d,
    ModelVrml,
    // --- Scientific / Medical ---
    ChemicalPdb,
    ChemicalXyz,
    ChemicalMol,
    ChemicalSdf,
    ChemicalCml,
    MedicalDicom,
    MedicalNifti,
    // --- System / Executable ---
    ApplicationExe,
    ApplicationDll,
    ApplicationSo,
    ApplicationDylib,
    ApplicationApp,
    ApplicationApk,
    ApplicationIpa,
    ApplicationMsi,
    ApplicationDmgApp,
    ApplicationPkg,
    ApplicationElf,
    ApplicationMachO,
    ApplicationPe,
    // --- Message ---
    MessageRfc822,
    MessagePartial,
    MessageExternalBody,
    MessageNews,
    MessageHttp,
    MessageImdn,
    // --- Protocol Specific ---
    ApplicationProtobuf,
    ApplicationMsgpack,
    ApplicationAvro,
    ApplicationThrift,
    ApplicationBson,
    ApplicationCbor,
    ApplicationYaml,
    ApplicationToml,
    // --- Streaming ---
    ApplicationDash,
    ApplicationHls,
    ApplicationSmil,
    ApplicationSdp,
    ApplicationRtsp,
    // --- Security / Certificates ---
    ApplicationPkcs7,
    ApplicationPkcs8,
    ApplicationPkcs10,
    ApplicationPkcs12,
    ApplicationPem,
    ApplicationX509,
    ApplicationPgp,
    ApplicationJwt,
    ApplicationJose,
    ApplicationCert,
    // --- Misc Application ---
    ApplicationShockwave,
    ApplicationFlash,
    ApplicationPostscript,
    ApplicationVnd,
    ApplicationAtomcat,
    ApplicationJnlp,
    ApplicationOgg,
    ApplicationXpinstall,
    ApplicationWidget,
    ApplicationVoicexml,
    ApplicationSparql,
    ApplicationSrgs,
    ApplicationOebps,
};

enum class MimeCategory : uint8_t {
    Unknown,
    Text,
    Image,
    Audio,
    Video,
    Archive,
    Document,
    Font,
    Application,
    Multipart,
    Binary,
    Code,
    Config,
    Ebook,
    Model3D,
    Scientific,
    Executable,
    Message,
};

/// Represents a MIME type with all associated metadata
struct MimeRecord {
    MimeType type;
    std::string_view mime_string; // e.g. "text/html"
    std::string_view extension;   // e.g. "html" (primary extension, lowercase)
    MimeCategory category;
    bool compressible; // Whether content should be compressed (gzip/brotli)

    constexpr bool operator==(MimeType t_mime_type) const {
        return type == t_mime_type;
    }

    constexpr bool operator==(const MimeRecord& t_other) const {
        return type == t_other.type;
    }
};

// --- The Master Database ---
// Compiler optimizes lookups in this array efficiently (length-based branching, etc.)
inline constexpr std::array<MimeRecord, 355> kMimeDatabase = {{// ============= TEXT =============
    {MimeType::TextPlain, "text/plain", "txt", MimeCategory::Text, true},
    {MimeType::TextHtml, "text/html", "html", MimeCategory::Text, true}, {MimeType::TextCss, "text/css", "css", MimeCategory::Text, true},
    {MimeType::TextCsv, "text/csv", "csv", MimeCategory::Text, true}, {MimeType::TextXml, "text/xml", "xml", MimeCategory::Text, true},
    {MimeType::TextJavascript, "text/javascript", "js", MimeCategory::Text, true},
    {MimeType::TextMarkdown, "text/markdown", "md", MimeCategory::Text, true},
    {MimeType::TextYaml, "text/yaml", "yaml", MimeCategory::Text, true},
    {MimeType::TextVcard, "text/vcard", "vcf", MimeCategory::Text, true},
    {MimeType::TextCalendar, "text/calendar", "ics", MimeCategory::Text, true},
    {MimeType::TextRichtext, "text/richtext", "rtx", MimeCategory::Text, true},
    {MimeType::TextCache, "text/cache-manifest", "appcache", MimeCategory::Text, true},

    // ============= APPLICATION / DOCS =============
    {MimeType::ApplicationJson, "application/json", "json", MimeCategory::Application, true},
    {MimeType::ApplicationXml, "application/xml", "xml", MimeCategory::Application, true},
    {MimeType::ApplicationJavascript, "application/javascript", "js", MimeCategory::Application, true},
    {MimeType::ApplicationWasm, "application/wasm", "wasm", MimeCategory::Application, true},
    {MimeType::ApplicationRtf, "application/rtf", "rtf", MimeCategory::Document, true},
    {MimeType::ApplicationFormUrlEncoded, "application/x-www-form-urlencoded", "", MimeCategory::Application, true},
    {MimeType::ApplicationPdf, "application/pdf", "pdf", MimeCategory::Document, false},
    {MimeType::ApplicationLd, "application/ld+json", "jsonld", MimeCategory::Application, true},
    {MimeType::ApplicationSql, "application/sql", "sql", MimeCategory::Application, true},
    {MimeType::ApplicationGraphql, "application/graphql", "graphql", MimeCategory::Application, true},
    {MimeType::ApplicationManifest, "application/manifest+json", "webmanifest", MimeCategory::Application, true},
    {MimeType::ApplicationXhtml, "application/xhtml+xml", "xhtml", MimeCategory::Application, true},
    {MimeType::ApplicationAtom, "application/atom+xml", "atom", MimeCategory::Application, true},
    {MimeType::ApplicationRss, "application/rss+xml", "rss", MimeCategory::Application, true},
    {MimeType::ApplicationSoap, "application/soap+xml", "soap", MimeCategory::Application, true},

    // ============= MS OFFICE =============
    {MimeType::DocWord, "application/msword", "doc", MimeCategory::Document, true},
    {MimeType::DocExcel, "application/vnd.ms-excel", "xls", MimeCategory::Document, true},
    {MimeType::DocPowerpoint, "application/vnd.ms-powerpoint", "ppt", MimeCategory::Document, true},
    {MimeType::DocxWord, "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx", MimeCategory::Document, false},
    {MimeType::XlsxExcel, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx", MimeCategory::Document, false},
    {MimeType::PptxPowerpoint, "application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx", MimeCategory::Document,
        false},
    {MimeType::DocAccess, "application/vnd.ms-access", "mdb", MimeCategory::Document, false},
    {MimeType::DocxAccess, "application/vnd.ms-access.addin.macroenabled.12", "accdb", MimeCategory::Document, false},
    {MimeType::DocVisio, "application/vnd.visio", "vsd", MimeCategory::Document, true},
    {MimeType::DocxVisio, "application/vnd.ms-visio.drawing", "vsdx", MimeCategory::Document, false},
    {MimeType::DocProject, "application/vnd.ms-project", "mpp", MimeCategory::Document, false},
    {MimeType::DocPublisher, "application/vnd.ms-publisher", "pub", MimeCategory::Document, false},
    {MimeType::DocOneNote, "application/onenote", "one", MimeCategory::Document, false},

    // ============= OPENDOCUMENT =============
    {MimeType::OdtText, "application/vnd.oasis.opendocument.text", "odt", MimeCategory::Document, false},
    {MimeType::OdsSpreadsheet, "application/vnd.oasis.opendocument.spreadsheet", "ods", MimeCategory::Document, false},
    {MimeType::OdpPresentation, "application/vnd.oasis.opendocument.presentation", "odp", MimeCategory::Document, false},
    {MimeType::OdgGraphics, "application/vnd.oasis.opendocument.graphics", "odg", MimeCategory::Document, false},
    {MimeType::OdfFormula, "application/vnd.oasis.opendocument.formula", "odf", MimeCategory::Document, false},
    {MimeType::OdbDatabase, "application/vnd.oasis.opendocument.database", "odb", MimeCategory::Document, false},
    {MimeType::OdcChart, "application/vnd.oasis.opendocument.chart", "odc", MimeCategory::Document, false},
    {MimeType::OdiImage, "application/vnd.oasis.opendocument.image", "odi", MimeCategory::Document, false},

    // ============= BINARIES =============
    {MimeType::ApplicationOctetStream, "application/octet-stream", "bin", MimeCategory::Application, false},

    // ============= ARCHIVES =============

    {MimeType::ArchiveZstd, "application/zstd", "zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZip, "application/zip", "zip", MimeCategory::Archive, false},
    {MimeType::ArchiveGzip, "application/gzip", "gz", MimeCategory::Archive, false},
    {MimeType::ArchiveTar, "application/x-tar", "tar", MimeCategory::Archive, true},
    {MimeType::Archive7z, "application/x-7z-compressed", "7z", MimeCategory::Archive, false},
    {MimeType::ArchiveRar, "application/vnd.rar", "rar", MimeCategory::Archive, false},
    {MimeType::ArchiveBzip2, "application/x-bzip2", "bz2", MimeCategory::Archive, false},
    {MimeType::ArchiveXz, "application/x-xz", "xz", MimeCategory::Archive, false},
    {MimeType::ArchiveLzma, "application/x-lzma", "lzma", MimeCategory::Archive, false},
    {MimeType::ArchiveZ, "application/x-compress", "Z", MimeCategory::Archive, false},
    {MimeType::ArchiveCab, "application/vnd.ms-cab-compressed", "cab", MimeCategory::Archive, false},
    {MimeType::ArchiveDmg, "application/x-apple-diskimage", "dmg", MimeCategory::Archive, false},
    {MimeType::ArchiveIso, "application/x-iso9660-image", "iso", MimeCategory::Archive, false},
    {MimeType::ArchiveJar, "application/java-archive", "jar", MimeCategory::Archive, false},
    {MimeType::ArchiveWar, "application/java-archive", "war", MimeCategory::Archive, false},
    {MimeType::ArchiveEar, "application/java-archive", "ear", MimeCategory::Archive, false},
    {MimeType::ArchiveDeb, "application/vnd.debian.binary-package", "deb", MimeCategory::Archive, false},
    {MimeType::ArchiveRpm, "application/x-rpm", "rpm", MimeCategory::Archive, false},

    // ============= IMAGES =============
    {MimeType::ImagePng, "image/png", "png", MimeCategory::Image, false},
    {MimeType::ImageJpeg, "image/jpeg", "jpg", MimeCategory::Image, false},
    {MimeType::ImageGif, "image/gif", "gif", MimeCategory::Image, false},
    {MimeType::ImageWebp, "image/webp", "webp", MimeCategory::Image, false},
    {MimeType::ImageAvif, "image/avif", "avif", MimeCategory::Image, false},
    {MimeType::ImageHeic, "image/heic", "heic", MimeCategory::Image, false},
    {MimeType::ImageHeif, "image/heif", "heif", MimeCategory::Image, false},
    {MimeType::ImageJpeg2000, "image/jp2", "jp2", MimeCategory::Image, false},
    {MimeType::ImageJxl, "image/jxl", "jxl", MimeCategory::Image, false},
    {MimeType::ImageTiff, "image/tiff", "tiff", MimeCategory::Image, false},
    {MimeType::ImageIco, "image/x-icon", "ico", MimeCategory::Image, true},
    {MimeType::ImageBmp, "image/bmp", "bmp", MimeCategory::Image, true},
    {MimeType::ImageSvg, "image/svg+xml", "svg", MimeCategory::Image, true},
    {MimeType::ImagePsd, "image/vnd.adobe.photoshop", "psd", MimeCategory::Image, true},
    {MimeType::ImageXcf, "image/x-xcf", "xcf", MimeCategory::Image, true},
    {MimeType::ImageXbm, "image/x-xbitmap", "xbm", MimeCategory::Image, true},
    {MimeType::ImageXpm, "image/x-xpixmap", "xpm", MimeCategory::Image, true},
    {MimeType::ImagePnm, "image/x-portable-anymap", "pnm", MimeCategory::Image, true},
    {MimeType::ImagePbm, "image/x-portable-bitmap", "pbm", MimeCategory::Image, true},
    {MimeType::ImagePgm, "image/x-portable-graymap", "pgm", MimeCategory::Image, true},
    {MimeType::ImagePpm, "image/x-portable-pixmap", "ppm", MimeCategory::Image, true},
    {MimeType::ImageRgb, "image/x-rgb", "rgb", MimeCategory::Image, true},
    {MimeType::ImageCgm, "image/cgm", "cgm", MimeCategory::Image, true},
    {MimeType::ImageDjvu, "image/vnd.djvu", "djvu", MimeCategory::Image, false},
    {MimeType::ImageDds, "image/vnd-ms.dds", "dds", MimeCategory::Image, false},
    {MimeType::ImageKtx, "image/ktx", "ktx", MimeCategory::Image, false},
    {MimeType::ImageApng, "image/apng", "apng", MimeCategory::Image, false},

    // ============= AUDIO =============
    {MimeType::AudioMpeg, "audio/mpeg", "mp3", MimeCategory::Audio, false},
    {MimeType::AudioOgg, "audio/ogg", "ogg", MimeCategory::Audio, false},
    {MimeType::AudioWav, "audio/wav", "wav", MimeCategory::Audio, true},
    {MimeType::AudioWebm, "audio/webm", "webm", MimeCategory::Audio, false},
    {MimeType::AudioAac, "audio/aac", "aac", MimeCategory::Audio, false},
    {MimeType::AudioFlac, "audio/flac", "flac", MimeCategory::Audio, false},
    {MimeType::AudioMidi, "audio/midi", "midi", MimeCategory::Audio, true},
    {MimeType::AudioOpus, "audio/opus", "opus", MimeCategory::Audio, false},
    {MimeType::AudioAmr, "audio/amr", "amr", MimeCategory::Audio, false},
    {MimeType::AudioAiff, "audio/aiff", "aiff", MimeCategory::Audio, true},
    {MimeType::AudioAu, "audio/basic", "au", MimeCategory::Audio, true},
    {MimeType::AudioM4a, "audio/mp4", "m4a", MimeCategory::Audio, false},
    {MimeType::AudioWma, "audio/x-ms-wma", "wma", MimeCategory::Audio, false},
    {MimeType::AudioRa, "audio/x-realaudio", "ra", MimeCategory::Audio, false},
    {MimeType::AudioApe, "audio/x-ape", "ape", MimeCategory::Audio, false},
    {MimeType::AudioAc3, "audio/ac3", "ac3", MimeCategory::Audio, false},
    {MimeType::AudioDts, "audio/vnd.dts", "dts", MimeCategory::Audio, false},
    {MimeType::AudioMpc, "audio/x-musepack", "mpc", MimeCategory::Audio, false},
    {MimeType::AudioSpeex, "audio/speex", "spx", MimeCategory::Audio, false},
    {MimeType::AudioVorbis, "audio/vorbis", "oga", MimeCategory::Audio, false},
    {MimeType::Audio3gpp, "audio/3gpp", "3gp", MimeCategory::Audio, false},
    {MimeType::Audio3gpp2, "audio/3gpp2", "3g2", MimeCategory::Audio, false},

    // ============= VIDEO =============
    {MimeType::VideoMp4, "video/mp4", "mp4", MimeCategory::Video, false},
    {MimeType::VideoMpeg, "video/mpeg", "mpeg", MimeCategory::Video, false},
    {MimeType::VideoWebm, "video/webm", "webm", MimeCategory::Video, false},
    {MimeType::VideoOgg, "video/ogg", "ogv", MimeCategory::Video, false},
    {MimeType::VideoQuicktime, "video/quicktime", "mov", MimeCategory::Video, false},
    {MimeType::VideoAvi, "video/x-msvideo", "avi", MimeCategory::Video, false},
    {MimeType::VideoFlv, "video/x-flv", "flv", MimeCategory::Video, false},
    {MimeType::VideoMkv, "video/x-matroska", "mkv", MimeCategory::Video, false},
    {MimeType::VideoWmv, "video/x-ms-wmv", "wmv", MimeCategory::Video, false},
    {MimeType::VideoAsf, "video/x-ms-asf", "asf", MimeCategory::Video, false},
    {MimeType::VideoM4v, "video/x-m4v", "m4v", MimeCategory::Video, false},
    {MimeType::VideoMts, "video/mp2t", "mts", MimeCategory::Video, false},
    {MimeType::VideoMxf, "application/mxf", "mxf", MimeCategory::Video, false},
    {MimeType::VideoRm, "application/vnd.rn-realmedia", "rm", MimeCategory::Video, false},
    {MimeType::VideoVob, "video/x-ms-vob", "vob", MimeCategory::Video, false},
    {MimeType::Video3gpp, "video/3gpp", "3gp", MimeCategory::Video, false},
    {MimeType::Video3gpp2, "video/3gpp2", "3g2", MimeCategory::Video, false},
    {MimeType::VideoTs, "video/mp2t", "ts", MimeCategory::Video, false},
    {MimeType::VideoF4v, "video/x-f4v", "f4v", MimeCategory::Video, false},
    {MimeType::VideoM2ts, "video/mp2t", "m2ts", MimeCategory::Video, false},
    {MimeType::VideoDivx, "video/x-divx", "divx", MimeCategory::Video, false},
    {MimeType::VideoXvid, "video/x-xvid", "xvid", MimeCategory::Video, false},

    // ============= MULTIPART =============
    {MimeType::MultipartFormData, "multipart/form-data", "", MimeCategory::Multipart, false},
    {MimeType::MultipartMixed, "multipart/mixed", "", MimeCategory::Multipart, false},
    {MimeType::MultipartAlternative, "multipart/alternative", "", MimeCategory::Multipart, false},
    {MimeType::MultipartRelated, "multipart/related", "", MimeCategory::Multipart, false},
    {MimeType::MultipartDigest, "multipart/digest", "", MimeCategory::Multipart, false},
    {MimeType::MultipartParallel, "multipart/parallel", "", MimeCategory::Multipart, false},
    {MimeType::MultipartByteranges, "multipart/byteranges", "", MimeCategory::Multipart, false},

    // ============= FONTS =============
    {MimeType::FontWoff, "font/woff", "woff", MimeCategory::Font, false},
    {MimeType::FontWoff2, "font/woff2", "woff2", MimeCategory::Font, false},
    {MimeType::FontTtf, "font/ttf", "ttf", MimeCategory::Font, true}, {MimeType::FontOtf, "font/otf", "otf", MimeCategory::Font, true},
    {MimeType::FontEot, "application/vnd.ms-fontobject", "eot", MimeCategory::Font, true},
    {MimeType::FontCollection, "font/collection", "ttc", MimeCategory::Font, true},
    {MimeType::FontSfnt, "font/sfnt", "sfnt", MimeCategory::Font, true},

    // ============= CODE / PROGRAMMING =============
    {MimeType::CodeC, "text/x-c", "c", MimeCategory::Code, true}, {MimeType::CodeCpp, "text/x-c++", "cpp", MimeCategory::Code, true},
    {MimeType::CodeCsharp, "text/x-csharp", "cs", MimeCategory::Code, true},
    {MimeType::CodeJava, "text/x-java", "java", MimeCategory::Code, true},
    {MimeType::CodePython, "text/x-python", "py", MimeCategory::Code, true},
    {MimeType::CodeRuby, "text/x-ruby", "rb", MimeCategory::Code, true}, {MimeType::CodePhp, "text/x-php", "php", MimeCategory::Code, true},
    {MimeType::CodeGo, "text/x-go", "go", MimeCategory::Code, true}, {MimeType::CodeRust, "text/x-rust", "rs", MimeCategory::Code, true},
    {MimeType::CodeSwift, "text/x-swift", "swift", MimeCategory::Code, true},
    {MimeType::CodeKotlin, "text/x-kotlin", "kt", MimeCategory::Code, true},
    {MimeType::CodeScala, "text/x-scala", "scala", MimeCategory::Code, true},
    {MimeType::CodePerl, "text/x-perl", "pl", MimeCategory::Code, true}, {MimeType::CodeLua, "text/x-lua", "lua", MimeCategory::Code, true},
    {MimeType::CodeShell, "text/x-shellscript", "sh", MimeCategory::Code, true},
    {MimeType::CodeBash, "application/x-sh", "bash", MimeCategory::Code, true},
    {MimeType::CodePowershell, "application/x-powershell", "ps1", MimeCategory::Code, true},
    {MimeType::CodeTypescript, "text/typescript", "ts", MimeCategory::Code, true},
    {MimeType::CodeCoffee, "text/coffeescript", "coffee", MimeCategory::Code, true},
    {MimeType::CodeDart, "text/x-dart", "dart", MimeCategory::Code, true},
    {MimeType::CodeElixir, "text/x-elixir", "ex", MimeCategory::Code, true},
    {MimeType::CodeErlang, "text/x-erlang", "erl", MimeCategory::Code, true},
    {MimeType::CodeHaskell, "text/x-haskell", "hs", MimeCategory::Code, true},
    {MimeType::CodeClojure, "text/x-clojure", "clj", MimeCategory::Code, true},
    {MimeType::CodeAsm, "text/x-asm", "asm", MimeCategory::Code, true},
    {MimeType::CodeFortran, "text/x-fortran", "f90", MimeCategory::Code, true},
    {MimeType::CodeCobol, "text/x-cobol", "cob", MimeCategory::Code, true}, {MimeType::CodeVb, "text/x-vb", "vb", MimeCategory::Code, true},
    {MimeType::CodeMatlab, "text/x-matlab", "m", MimeCategory::Code, true}, {MimeType::CodeR, "text/x-r", "r", MimeCategory::Code, true},
    {MimeType::CodeSql, "text/x-sql", "sql", MimeCategory::Code, true},

    // ============= CONFIG / DATA =============
    {MimeType::ConfigToml, "application/toml", "toml", MimeCategory::Config, true},
    {MimeType::ConfigIni, "text/plain", "ini", MimeCategory::Config, true},
    {MimeType::ConfigProperties, "text/plain", "properties", MimeCategory::Config, true},
    {MimeType::ConfigEnv, "text/plain", "env", MimeCategory::Config, true},
    {MimeType::ConfigYml, "application/x-yaml", "yml", MimeCategory::Config, true},
    {MimeType::ConfigHocon, "text/plain", "conf", MimeCategory::Config, true},
    {MimeType::ConfigEditorconfig, "text/plain", "editorconfig", MimeCategory::Config, true},

    // ============= EBOOKS =============
    {MimeType::EbookEpub, "application/epub+zip", "epub", MimeCategory::Ebook, false},
    {MimeType::EbookMobi, "application/x-mobipocket-ebook", "mobi", MimeCategory::Ebook, false},
    {MimeType::EbookAzw, "application/vnd.amazon.ebook", "azw", MimeCategory::Ebook, false},
    {MimeType::EbookAzw3, "application/vnd.amazon.ebook", "azw3", MimeCategory::Ebook, false},
    {MimeType::EbookDjvu, "image/vnd.djvu", "djvu", MimeCategory::Ebook, false},
    {MimeType::EbookFb2, "application/x-fictionbook+xml", "fb2", MimeCategory::Ebook, true},
    {MimeType::EbookLit, "application/x-ms-reader", "lit", MimeCategory::Ebook, false},

    // ============= CAD / 3D =============
    {MimeType::ModelStl, "model/stl", "stl", MimeCategory::Model3D, true},
    {MimeType::ModelObj, "model/obj", "obj", MimeCategory::Model3D, true},
    {MimeType::ModelFbx, "application/octet-stream", "fbx", MimeCategory::Model3D, false},
    {MimeType::ModelDae, "model/vnd.collada+xml", "dae", MimeCategory::Model3D, true},
    {MimeType::ModelGltf, "model/gltf+json", "gltf", MimeCategory::Model3D, true},
    {MimeType::ModelGlb, "model/gltf-binary", "glb", MimeCategory::Model3D, false},
    {MimeType::Model3ds, "application/x-3ds", "3ds", MimeCategory::Model3D, false},
    {MimeType::ModelPly, "application/ply", "ply", MimeCategory::Model3D, true},
    {MimeType::ModelStep, "application/step", "step", MimeCategory::Model3D, true},
    {MimeType::ModelIges, "model/iges", "iges", MimeCategory::Model3D, true},
    {MimeType::ModelDwg, "application/acad", "dwg", MimeCategory::Model3D, false},
    {MimeType::ModelDxf, "application/dxf", "dxf", MimeCategory::Model3D, true},
    {MimeType::ModelX3d, "model/x3d+xml", "x3d", MimeCategory::Model3D, true},
    {MimeType::ModelU3d, "model/u3d", "u3d", MimeCategory::Model3D, false},
    {MimeType::ModelVrml, "model/vrml", "wrl", MimeCategory::Model3D, true},

    // ============= SCIENTIFIC / MEDICAL =============
    {MimeType::ChemicalPdb, "chemical/x-pdb", "pdb", MimeCategory::Scientific, true},
    {MimeType::ChemicalXyz, "chemical/x-xyz", "xyz", MimeCategory::Scientific, true},
    {MimeType::ChemicalMol, "chemical/x-mdl-molfile", "mol", MimeCategory::Scientific, true},
    {MimeType::ChemicalSdf, "chemical/x-mdl-sdfile", "sdf", MimeCategory::Scientific, true},
    {MimeType::ChemicalCml, "chemical/x-cml", "cml", MimeCategory::Scientific, true},
    {MimeType::MedicalDicom, "application/dicom", "dcm", MimeCategory::Scientific, false},
    {MimeType::MedicalNifti, "application/x-nifti", "nii", MimeCategory::Scientific, false},

    // ============= SYSTEM / EXECUTABLE =============
    {MimeType::ApplicationExe, "application/vnd.microsoft.portable-executable", "exe", MimeCategory::Executable, false},
    {MimeType::ApplicationDll, "application/x-msdownload", "dll", MimeCategory::Executable, false},
    {MimeType::ApplicationSo, "application/x-sharedlib", "so", MimeCategory::Executable, false},
    {MimeType::ApplicationDylib, "application/x-sharedlib", "dylib", MimeCategory::Executable, false},
    {MimeType::ApplicationApp, "application/x-executable", "app", MimeCategory::Executable, false},
    {MimeType::ApplicationApk, "application/vnd.android.package-archive", "apk", MimeCategory::Executable, false},
    {MimeType::ApplicationIpa, "application/octet-stream", "ipa", MimeCategory::Executable, false},
    {MimeType::ApplicationMsi, "application/x-msi", "msi", MimeCategory::Executable, false},
    {MimeType::ApplicationDmgApp, "application/x-apple-diskimage", "dmg", MimeCategory::Executable, false},
    {MimeType::ApplicationPkg, "application/x-newton-compatible-pkg", "pkg", MimeCategory::Executable, false},
    {MimeType::ApplicationElf, "application/x-executable", "elf", MimeCategory::Executable, false},
    {MimeType::ApplicationMachO, "application/x-mach-binary", "macho", MimeCategory::Executable, false},
    {MimeType::ApplicationPe, "application/x-dosexec", "pe", MimeCategory::Executable, false},

    // ============= MESSAGE =============
    {MimeType::MessageRfc822, "message/rfc822", "eml", MimeCategory::Message, true},
    {MimeType::MessagePartial, "message/partial", "", MimeCategory::Message, true},
    {MimeType::MessageExternalBody, "message/external-body", "", MimeCategory::Message, true},
    {MimeType::MessageNews, "message/news", "", MimeCategory::Message, true},
    {MimeType::MessageHttp, "message/http", "", MimeCategory::Message, true},
    {MimeType::MessageImdn, "message/imdn+xml", "", MimeCategory::Message, true},

    // ============= PROTOCOL SPECIFIC =============
    {MimeType::ApplicationProtobuf, "application/protobuf", "proto", MimeCategory::Application, false},
    {MimeType::ApplicationMsgpack, "application/msgpack", "msgpack", MimeCategory::Application, false},
    {MimeType::ApplicationAvro, "application/avro", "avro", MimeCategory::Application, false},
    {MimeType::ApplicationThrift, "application/x-thrift", "thrift", MimeCategory::Application, false},
    {MimeType::ApplicationBson, "application/bson", "bson", MimeCategory::Application, false},
    {MimeType::ApplicationCbor, "application/cbor", "cbor", MimeCategory::Application, false},
    {MimeType::ApplicationYaml, "application/yaml", "yaml", MimeCategory::Application, true},
    {MimeType::ApplicationToml, "application/toml", "toml", MimeCategory::Application, true},

    // ============= STREAMING =============
    {MimeType::ApplicationDash, "application/dash+xml", "mpd", MimeCategory::Application, true},
    {MimeType::ApplicationHls, "application/vnd.apple.mpegurl", "m3u8", MimeCategory::Application, true},
    {MimeType::ApplicationSmil, "application/smil+xml", "smil", MimeCategory::Application, true},
    {MimeType::ApplicationSdp, "application/sdp", "sdp", MimeCategory::Application, true},
    {MimeType::ApplicationRtsp, "application/rtsp", "rtsp", MimeCategory::Application, true},

    // ============= SECURITY / CERTIFICATES =============
    {MimeType::ApplicationPkcs7, "application/pkcs7-mime", "p7m", MimeCategory::Application, false},
    {MimeType::ApplicationPkcs8, "application/pkcs8", "p8", MimeCategory::Application, false},
    {MimeType::ApplicationPkcs10, "application/pkcs10", "p10", MimeCategory::Application, false},
    {MimeType::ApplicationPkcs12, "application/x-pkcs12", "p12", MimeCategory::Application, false},
    {MimeType::ApplicationPem, "application/x-pem-file", "pem", MimeCategory::Application, true},
    {MimeType::ApplicationX509, "application/x-x509-ca-cert", "crt", MimeCategory::Application, false},
    {MimeType::ApplicationPgp, "application/pgp-encrypted", "pgp", MimeCategory::Application, false},
    {MimeType::ApplicationJwt, "application/jwt", "jwt", MimeCategory::Application, false},
    {MimeType::ApplicationJose, "application/jose", "jose", MimeCategory::Application, false},
    {MimeType::ApplicationCert, "application/x-x509-user-cert", "cer", MimeCategory::Application, false},

    // ============= MISC APPLICATION =============
    {MimeType::ApplicationShockwave, "application/x-director", "dcr", MimeCategory::Application, false},
    {MimeType::ApplicationFlash, "application/x-shockwave-flash", "swf", MimeCategory::Application, false},
    {MimeType::ApplicationPostscript, "application/postscript", "ps", MimeCategory::Application, true},
    {MimeType::ApplicationVnd, "application/vnd", "", MimeCategory::Application, false},
    {MimeType::ApplicationAtomcat, "application/atomcat+xml", "atomcat", MimeCategory::Application, true},
    {MimeType::ApplicationJnlp, "application/x-java-jnlp-file", "jnlp", MimeCategory::Application, true},
    {MimeType::ApplicationOgg, "application/ogg", "ogx", MimeCategory::Application, false},
    {MimeType::ApplicationXpinstall, "application/x-xpinstall", "xpi", MimeCategory::Application, false},
    {MimeType::ApplicationWidget, "application/widget", "wgt", MimeCategory::Application, false},
    {MimeType::ApplicationVoicexml, "application/voicexml+xml", "vxml", MimeCategory::Application, true},
    {MimeType::ApplicationSparql, "application/sparql-query", "rq", MimeCategory::Application, true},
    {MimeType::ApplicationSrgs, "application/srgs", "gram", MimeCategory::Application, true},
    {MimeType::ArchiveZstd, "application/zstd", "json.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "xml.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "toml.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "yaml.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "csv.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "sql.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "txt.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "log.zst", MimeCategory::Archive, false},
    {MimeType::CodeScl, "text/x-scl", "scl", MimeCategory::Code, true},
    {MimeType::DataSiemensDb, "text/x-s7-db", "db", MimeCategory::Text, true},
    {MimeType::DataSiemensUdt, "text/x-s7-udt", "udt", MimeCategory::Text, true},
    {MimeType::ArchiveZstd, "application/zstd", "scl.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "db.zst", MimeCategory::Archive, false},
    {MimeType::ArchiveZstd, "application/zstd", "udt.zst", MimeCategory::Archive, false},
    {MimeType::ApplicationOebps, "application/oebps-package+xml", "opf", MimeCategory::Application, true}}};

// =====================================================================
// INTERNAL HELPERS
// =====================================================================

namespace detail
{

/// Trim whitespace from string_view (constexpr compatible)
constexpr std::string_view trim(std::string_view t_str) noexcept {
    size_t start = 0;
    while (start < t_str.size() && (t_str[start] == ' ' || t_str[start] == '\t')) {
        ++start;
    }
    size_t end = t_str.size();
    while (end > start && (t_str[end - 1] == ' ' || t_str[end - 1] == '\t')) {
        --end;
    }
    return t_str.substr(start, end - start);
}

/// Convert character to lowercase
constexpr char toLower(char t_char) noexcept {
    return (t_char >= 'A' && t_char <= 'Z') ? (t_char + 32) : t_char;
}

/// Case-insensitive string comparison (constexpr compatible)
constexpr bool equalsIgnoreCase(std::string_view t_a, std::string_view t_b) noexcept {
    if (t_a.size() != t_b.size()) {
        return false;
    }
    for (size_t i = 0; i < t_a.size(); ++i) {
        if (toLower(t_a[i]) != toLower(t_b[i])) {
            return false;
        }
    }
    return true;
}

/// Find record by MimeType (internal helper)
constexpr const MimeRecord* findRecord(MimeType t_type) noexcept {
    for (const auto& p_record : kMimeDatabase) {
        if (p_record.type == t_type) {
            return &p_record;
        }
    }
    return nullptr;
}

} // namespace detail

// =====================================================================
// PUBLIC API - PRIMARY CONVERSIONS
// =====================================================================

/// Get MIME string from MimeType enum
/// @returns MIME string (e.g., "text/html"), or std::nullopt if type is Unknown
constexpr std::optional<std::string_view> toMimeStringView(MimeType t_type) noexcept {
    if (t_type == MimeType::Unknown) {
        return std::nullopt;
    }
    if (const auto* p_record = detail::findRecord(t_type)) {
        return p_record->mime_string;
    }
    return std::nullopt;
}

inline std::optional<std::string> toMimeString(MimeType t_type) noexcept {
    std::optional<std::string_view> result = toMimeStringView(t_type);
    if (!result) {
        return std::nullopt;
    } else {
        return std::string(result.value());
    }
}

/// Get MIME string from MimeType enum, with fallback
/// @returns MIME string, or "application/octet-stream" if type is Unknown
constexpr std::string_view toMimeStringOrDefault(MimeType t_type) noexcept {
    if (auto result = toMimeStringView(t_type)) {
        return *result;
    }
    return "application/octet-stream";
}

/// Get file extension from MimeType enum
/// @returns Extension without dot (e.g., "html"), or std::nullopt if not found
constexpr std::optional<std::string_view> toExtension(MimeType t_type) noexcept {
    if (t_type == MimeType::Unknown) {
        return std::nullopt;
    }
    if (const auto* p_record = detail::findRecord(t_type)) {
        if (!p_record->extension.empty()) {
            return p_record->extension;
        }
    }
    return std::nullopt;
}

/// Parse MIME string to MimeType enum (exact match, case-sensitive)
/// @param mime_str MIME string like "text/html"
/// @returns MimeType enum, or std::nullopt if not recognized
constexpr std::optional<MimeType> fromMimeString(std::string_view t_mime_str) noexcept {
    if (t_mime_str.empty()) {
        return std::nullopt;
    }

    for (const auto& p_record : kMimeDatabase) {
        if (p_record.mime_string == t_mime_str && p_record.type != MimeType::Unknown) {
            return p_record.type;
        }
    }
    return std::nullopt;
}

/// Parse file extension to MimeType enum (case-insensitive)
/// @param ext Extension with or without dot (e.g., "jpg" or ".jpg")
/// @returns Primary MimeType for this extension, or std::nullopt if not recognized
/// @note For extensions with multiple MIME types (e.g., "js"), returns the most common one
constexpr std::optional<MimeType> fromExtension(std::string_view t_ext) noexcept {
    if (t_ext.empty()) {
        return std::nullopt;
    }

    // Skip leading dot if present
    if (t_ext[0] == '.') {
        t_ext = t_ext.substr(1);
    }

    if (t_ext.empty()) {
        return std::nullopt;
    }

    // Case-insensitive search (returns first/primary match)
    for (const auto& p_record : kMimeDatabase) {
        if (detail::equalsIgnoreCase(p_record.extension, t_ext) && p_record.type != MimeType::Unknown) {
            return p_record.type;
        }
    }
    return std::nullopt;
}

/// Get all MimeTypes that share the same file extension
/// @param ext Extension with or without dot
/// @returns Vector of all matching MimeTypes (empty if none found)
/// @note Useful for handling ambiguous extensions like "js", "xml", etc.
inline std::vector<MimeType> allFromExtension(std::string_view t_ext) {
    std::vector<MimeType> results;

    if (t_ext.empty()) {
        return results;
    }

    // Skip leading dot if present
    if (t_ext[0] == '.') {
        t_ext = t_ext.substr(1);
    }

    if (t_ext.empty()) {
        return results;
    }

    for (const auto& p_record : kMimeDatabase) {
        if (detail::equalsIgnoreCase(p_record.extension, t_ext) && p_record.type != MimeType::Unknown) {
            results.push_back(p_record.type);
        }
    }

    return results;
}

// =====================================================================
// PUBLIC API - RECORD ACCESS
// =====================================================================

/// Get complete MimeRecord for a given MimeType
/// @returns MimeRecord with all metadata, or std::nullopt if type is Unknown
constexpr std::optional<MimeRecord> getRecord(MimeType t_type) noexcept {
    if (t_type == MimeType::Unknown) {
        return std::nullopt;
    }
    if (const auto* p_record = detail::findRecord(t_type)) {
        return *p_record;
    }
    return std::nullopt;
}

/// Parse HTTP Content-Type header and return complete MimeRecord
/// @param content_type HTTP Content-Type header value
/// @returns MimeRecord if recognized, std::nullopt otherwise
/// @note Handles:
///   - "text/html" -> MimeRecord for text/html
///   - "text/html; charset=utf-8" -> MimeRecord for text/html (strips parameters)
///   - "TEXT/HTML" -> MimeRecord for text/html (case-insensitive)
///   - "  text/html  " -> MimeRecord for text/html (trims whitespace)
inline std::optional<MimeRecord> parseContentType(std::string_view t_content_type) noexcept {
    // Trim leading/trailing whitespace
    t_content_type = detail::trim(t_content_type);

    if (t_content_type.empty()) {
        return std::nullopt;
    }

    // Extract MIME type only (before semicolon for parameters like charset)
    size_t semicolon_pos = t_content_type.find(';');
    std::string_view mime_only = (semicolon_pos != std::string_view::npos) ? t_content_type.substr(0, semicolon_pos) : t_content_type;

    // Trim again in case there's whitespace before semicolon
    mime_only = detail::trim(mime_only);

    // Case-insensitive search
    for (const auto& p_record : kMimeDatabase) {
        if (detail::equalsIgnoreCase(p_record.mime_string, mime_only) && p_record.type != MimeType::Unknown) {
            return p_record;
        }
    }

    return std::nullopt;
}

// =====================================================================
// PUBLIC API - METADATA QUERIES
// =====================================================================

/// Check if content of this MIME type should be compressed (gzip/brotli)
/// @returns true if content is compressible, false otherwise
constexpr bool isCompressible(MimeType t_type) noexcept {
    if (const auto* p_record = detail::findRecord(t_type)) {
        return p_record->compressible;
    }
    return false; // Safe default: don't compress unknown types
}

/// Get category for a MIME type
/// @returns MimeCategory enum value
constexpr MimeCategory getCategory(MimeType t_type) noexcept {
    if (const auto* p_record = detail::findRecord(t_type)) {
        return p_record->category;
    }
    return MimeCategory::Unknown;
}

// =====================================================================
// PUBLIC API - CATEGORY CHECKS
// =====================================================================

constexpr bool isText(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Text;
}

constexpr bool isImage(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Image;
}

constexpr bool isVideo(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Video;
}

constexpr bool isAudio(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Audio;
}

constexpr bool isCode(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Code;
}

constexpr bool isArchive(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Archive;
}

constexpr bool isDocument(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Document;
}

constexpr bool isFont(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Font;
}

constexpr bool isExecutable(MimeType t_type) noexcept {
    return getCategory(t_type) == MimeCategory::Executable;
}

constexpr bool isBinary(MimeType t_type) noexcept {
    auto cat = getCategory(t_type);
    return cat == MimeCategory::Binary || cat == MimeCategory::Archive || cat == MimeCategory::Executable ||
           (cat == MimeCategory::Image && !isCompressible(t_type)) || (cat == MimeCategory::Audio && !isCompressible(t_type)) ||
           (cat == MimeCategory::Video);
}

} // namespace sgrn::utils::mime
