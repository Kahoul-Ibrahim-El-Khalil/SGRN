#pragma once

#include <cstdint>
#include <string>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7Memory {
public:
    explicit ScriptS7Memory(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    std::string readArea(int t_area, uint16_t t_db, int t_start, int t_size, int t_word_len = 2); // S7WLByte is 2
    void writeArea(int t_area, uint16_t t_db, int t_start, const std::string& t_hex, int t_word_len = 2);

    void writeAreaInt(int t_area, uint16_t t_db, int t_start, int64_t t_value, int t_size_bytes = 1, int t_word_len = 2);
    void writeAreaByte(int t_area, uint16_t t_db, int t_start, int t_value);

    std::string readAddress(const std::string& t_addr, int t_size = 0);
    void writeAddress(const std::string& t_addr, const std::string& t_hex);

    std::string readTag(const std::string& t_name);
    void writeTag(const std::string& t_name, const std::string& t_hex);
    std::string tagInfo(const std::string& t_name) const;
    std::string decodeTag(const std::string& t_name, const std::string& t_hex) const;

    std::string readDB(uint16_t t_db, int t_start, int t_size);
    void writeDB(uint16_t t_db, int t_start, const std::string& t_hex);
    void writeDBInt(uint16_t t_db, int t_start, int64_t t_value, int t_size_bytes = 1);

    std::string readMB(int t_start, int t_size);
    void writeMB(int t_start, const std::string& t_hex);
    void writeMBInt(int t_start, int64_t t_value, int t_size_bytes = 1);

    std::string readEB(int t_start, int t_size);
    void writeEB(int t_start, const std::string& t_hex);

    std::string readAB(int t_start, int t_size);
    void writeAB(int t_start, const std::string& t_hex);

    std::string readTM(int t_start, int t_count);
    void writeTM(int t_start, int t_count, const std::string& t_hex);

    std::string readCT(int t_start, int t_count);
    void writeCT(int t_start, int t_count, const std::string& t_hex);

    bool saveHexToFile(const std::string& t_path, const std::string& t_hex) const;
    std::string loadHexFromFile(const std::string& t_path) const;

    std::string listTags() const;

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
};

} // namespace sgrn::s7shell::shell
