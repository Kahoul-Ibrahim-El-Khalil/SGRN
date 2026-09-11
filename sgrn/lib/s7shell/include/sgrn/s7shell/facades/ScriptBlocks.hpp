#pragma once

#include <cstdint>
#include <string>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7Blocks {
public:
    explicit ScriptS7Blocks(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    std::string upload(int t_block_type, uint16_t t_block_number, int t_max_size = 65536);
    std::string fullUpload(int t_block_type, uint16_t t_block_number, int t_max_size = 65536);
    void download(uint16_t t_block_number, const std::string& t_hex);
    void deleteBlock(int t_block_type, uint16_t t_block_number);

    std::string dbGet(uint16_t t_db_number, int t_max_size = 65536);
    void dbFill(uint16_t t_db_number, int t_fill_char);

    std::string pgBlockInfo(const std::string& t_hex) const;
    bool saveHex(const std::string& t_path, const std::string& t_hex) const;
    std::string loadHex(const std::string& t_path) const;

    bool uploadToFile(int t_block_type, uint16_t t_block_number, const std::string& t_path, bool t_full = true, int t_max_size = 65536);
    bool downloadFromFile(uint16_t t_block_number, const std::string& t_path);
    bool dbGetToFile(uint16_t t_db_number, const std::string& t_path, int t_max_size = 65536);
    bool dbDownloadFromFile(uint16_t t_block_number, const std::string& t_path);

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
};

} // namespace sgrn::s7shell::shell
