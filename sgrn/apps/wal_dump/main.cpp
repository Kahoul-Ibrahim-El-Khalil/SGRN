#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <sgrn/utils/json.hpp>

#include <fmt/core.h>
#include <stdexcept>
#include <string>
#include <string_view>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace sgrn::gateway::twin;

std::string serializeCompact(const sgrn::json::dom::Value& t_value) {
    return sgrn::utils::json::serializeCompact(t_value);
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
        std::vector<std::string> path_by_id;
        sgrn::json::dom::Document manifest_doc;
        sgrn::json::dom::Document footer_doc;

        bool has_dictionary = false;

        while (reader.readLine(line)) {
            sgrn::json::dom::Document doc;
            doc.parse(line.data(), line.size());
            if (doc.hasParseError() || !doc.is_object())
                continue;

            const char* type = doc.hasMember("type") && doc["type"].is_string() ? doc["type"].getString() : "";

            if (std::string_view(type) == "schema") {
                archive_schema_json = serializeCompact(doc["schema"]);
            } else if (std::string_view(type) == "dictionary") {
                has_dictionary = true;
                archive_dict_json = line; // Stash the whole dictionary line JSON
                if (doc.hasMember("leaves") && doc["leaves"].is_array()) {
                    for (const auto& item : doc["leaves"].elements()) {
                        if (item.is_object() && item.hasMember("id") && item["id"].isUint() && item.hasMember("path") &&
                            item["path"].is_string()) {
                            auto id = item["id"].getUint();
                            if (id >= path_by_id.size())
                                path_by_id.resize(id + 1);
                            path_by_id[id] = item["path"].getString();
                        }
                    }
                }
            } else if (std::string_view(type) == "manifest") {
                manifest_doc.copyFrom(doc, manifest_doc.allocator());
            } else if (std::string_view(type) == "footer") {
                footer_doc.copyFrom(doc, footer_doc.allocator());
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
                    sgrn::json::dom::Document s_doc;
                    if (!archive_schema_json.empty() && archive_schema_json != "null") {
                        s_doc.parse(archive_schema_json.c_str());
                    } else {
                        s_doc.setObject();
                    }
                    if (has_dictionary) {
                        sgrn::json::dom::Document d_doc;
                        d_doc.parse(archive_dict_json.c_str());
                        if (d_doc.hasMember("leaves")) {
                            s_doc.addMember("leaves", d_doc["leaves"], s_doc.allocator());
                        }
                    }
                    std::ofstream schema_out(current_group_dir + "/schema.json");
                    schema_out << serializeCompact(s_doc) << "\n";
                }

                sgrn::json::dom::Document doc_out;
                if (!path_by_id.empty()) {
                    if (!expandRecordKeys(doc, path_by_id, doc_out.allocator(), doc_out).hasError()) {
                        data_out << serializeCompact(doc_out) << "\n";
                        continue;
                    }
                }
                data_out << serializeCompact(doc) << "\n";
            }
        }

        if (!current_group_dir.empty() && !manifest_doc.is_null()) {
            if (!footer_doc.is_null()) {
                if (footer_doc.hasMember("record_count")) {
                    manifest_doc.addMember("record_count", footer_doc["record_count"], manifest_doc.allocator());
                }
                if (footer_doc.hasMember("last_anchor_line")) {
                    manifest_doc.addMember("last_anchor_line", footer_doc["last_anchor_line"], manifest_doc.allocator());
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
