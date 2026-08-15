#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::s7shell::shell
{

struct ScriptS7Connection;

class ScriptS7Async {
public:
    explicit ScriptS7Async(ScriptS7Connection* tp_conn);

    void addRef();
    void release();

    void reset();

    bool beginReadArea(int t_area, uint16_t t_db, int t_start, int t_size, int t_word_len = 2); // S7WLByte is 2
    bool beginReadDB(uint16_t t_db, int t_start, int t_size);
    bool beginFullUpload(int t_block_type, uint16_t t_block_number, int t_max_size = 65536);
    bool beginDownload(uint16_t t_block_number, const std::string& t_hex);

    bool isDone();
    bool wait(int t_timeout_ms = 5000);

    int getLastOpError() const;
    std::string resultHex() const;

private:
    int ref_count_{1};
    ScriptS7Connection* conn_{nullptr};
    std::vector<uint8_t> buffer_;
    int io_size_{0};
    bool active_{false};
    int last_op_error_{0};
};

} // namespace sgrn::s7shell::shell
