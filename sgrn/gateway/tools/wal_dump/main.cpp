#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json.hpp>

#include <fmt/core.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace sgrn::gateway::twin;

std::string serializeCompact(const rapidjson::Value& t_value) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    t_value.Accept(w);
    return sb.GetString();
}

std::string archiveSortKey(const fs::path& t_path) {
    std::string name = t_path.filename().string();
    if (name.ends_with(".tmp"))
        name.resize(name.size() - 4);
    if (name.ends_with(".jsonl.zst"))
        name.resize(name.size() - 10);
    const size_t dash = name.find('-');
    if (dash == std::string::npos)
        return name;
    return name;
}

bool isWALArchive(const fs::path& t_path) {
    const std::string name = t_path.filename().string();
    return name.size() > 9 && (name.ends_with(".jsonl.zst") || name.ends_with(".jsonl.zst.tmp"));
}

struct ArchiveInfo {
    fs::path path;
    std::string sort_key;
};

void printUsage() {
    fmt::print("Usage:\n");
    fmt::print("  sgrn-wal-dump <archive.jsonl.zst> [--out-dir DIR]\n");
    fmt::print("  sgrn-wal-dump --dir <state_dir>/unsynced [--out-dir DIR]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string input_file;
    std::string input_dir;
    std::string out_dir = ".";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            input_dir = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (input_file.empty() && input_dir.empty()) {
            input_file = arg;
        }
    }

    if (input_file.empty() && input_dir.empty()) {
        fmt::print(stderr, "SchemaError: Must specify either an archive file or --dir.\n");
        return 1;
    }

    std::vector<ArchiveInfo> archives;
    if (!input_dir.empty()) {
        std::error_code ec;
        if (!fs::exists(input_dir, ec)) {
            fmt::print(stderr, "Directory not found: {}\n", input_dir);
            return 1;
        }
        for (fs::recursive_directory_iterator it(input_dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && isWALArchive(it->path())) {
                archives.push_back({it->path(), archiveSortKey(it->path())});
            }
        }
        std::sort(archives.begin(), archives.end(), [](const ArchiveInfo& a, const ArchiveInfo& b) {
            return a.sort_key < b.sort_key; // Chronological order
        });
    } else {
        archives.push_back({fs::path(input_file), archiveSortKey(fs::path(input_file))});
    }

    if (archives.empty()) {
        fmt::print("No archives found.\n");
        return 0;
    }

    std::string current_schema_json;
    std::string current_dict_json;
    std::string current_group_dir;
    int group_counter = 0;
    std::ofstream data_out;

    for (const auto& arch : archives) {
        fmt::print("Processing {}\n", arch.path.string());
        sgrn::utils::compression::ZstdLineReader reader(arch.path);
        if (!reader.ok()) {
            fmt::print(stderr, "SchemaError reading {}: {}\n", arch.path.string(), reader.errorMessage());
            continue;
        }

        std::string line;
        std::string archive_schema_json;
        std::string archive_dict_json;
        std::unordered_map<LeafId, std::string> id_to_path_map;
        rapidjson::Document manifest_doc;
        rapidjson::Document footer_doc;

        bool has_dictionary = false;

        while (reader.readLine(line)) {
            rapidjson::Document doc;
            doc.Parse(line.c_str());
            if (doc.HasParseError() || !doc.IsObject())
                continue;

            const char* type = doc.HasMember("type") && doc["type"].IsString() ? doc["type"].GetString() : "";

            if (std::string_view(type) == "schema") {
                archive_schema_json = serializeCompact(doc["schema"]);
            } else if (std::string_view(type) == "dictionary") {
                has_dictionary = true;
                archive_dict_json = line; // Stash the whole dictionary line JSON
                if (doc.HasMember("leaves") && doc["leaves"].IsArray()) {
                    for (const auto& item : doc["leaves"].GetArray()) {
                        if (item.IsObject() && item.HasMember("id") && item["id"].IsUint() && item.HasMember("path") &&
                            item["path"].IsString()) {
                            id_to_path_map[item["id"].GetUint()] = item["path"].GetString();
                        }
                    }
                }
            } else if (std::string_view(type) == "manifest") {
                manifest_doc.CopyFrom(doc, manifest_doc.GetAllocator());
            } else if (std::string_view(type) == "footer") {
                footer_doc.CopyFrom(doc, footer_doc.GetAllocator());
            } else if (std::string_view(type) == "anchor" || std::string_view(type) == "delta") {
                bool schema_drift = (archive_schema_json != current_schema_json || archive_dict_json != current_dict_json);

                if (schema_drift || !data_out.is_open()) {
                    current_schema_json = archive_schema_json;
                    current_dict_json = archive_dict_json;

                    std::string group_name = archives.size() > 1 ? fmt::format("group_{:03d}", ++group_counter) : arch.sort_key;
                    fs::path out_path = fs::path(out_dir) / group_name;
                    fs::create_directories(out_path);
                    current_group_dir = out_path.string();

                    if (data_out.is_open())
                        data_out.close();
                    data_out.open(current_group_dir + "/data.jsonl", std::ios::app);

                    // Write schema.json
                    rapidjson::Document s_doc;
                    if (!archive_schema_json.empty() && archive_schema_json != "null") {
                        s_doc.Parse(archive_schema_json.c_str());
                    } else {
                        s_doc.SetObject();
                    }
                    if (has_dictionary) {
                        rapidjson::Document d_doc;
                        d_doc.Parse(archive_dict_json.c_str());
                        if (d_doc.HasMember("leaves")) {
                            rapidjson::Value leaves_copy(d_doc["leaves"], s_doc.GetAllocator());
                            s_doc.AddMember("leaves", leaves_copy, s_doc.GetAllocator());
                        }
                    }
                    rapidjson::StringBuffer sb;
                    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
                    s_doc.Accept(w);
                    std::ofstream schema_out(current_group_dir + "/schema.json");
                    schema_out << sb.GetString() << "\n";
                }

                rapidjson::Document doc_out;
                if (!id_to_path_map.empty()) {
                    if (!expandRecordKeys(doc, id_to_path_map, doc_out.GetAllocator(), doc_out).hasError()) {
                        data_out << serializeCompact(doc_out) << "\n";
                        continue;
                    }
                }
                data_out << serializeCompact(doc) << "\n";
            }
        }

        if (!current_group_dir.empty() && !manifest_doc.IsNull()) {
            if (!footer_doc.IsNull()) {
                if (footer_doc.HasMember("record_count")) {
                    rapidjson::Value count_copy(footer_doc["record_count"], manifest_doc.GetAllocator());
                    manifest_doc.AddMember("record_count", count_copy, manifest_doc.GetAllocator());
                }
                if (footer_doc.HasMember("last_anchor_line")) {
                    rapidjson::Value anchor_copy(footer_doc["last_anchor_line"], manifest_doc.GetAllocator());
                    manifest_doc.AddMember("last_anchor_line", anchor_copy, manifest_doc.GetAllocator());
                }
            }
            std::ofstream manifest_out(current_group_dir + "/manifest.json");
            manifest_out << serializeCompact(manifest_doc) << "\n";
        }
    }

    if (data_out.is_open()) {
        data_out.close();
    }

    fmt::print("Done.\n");
    return 0;
}
