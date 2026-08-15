#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptHexTable.hpp>

#include <fmt/format.h>
#include <cctype>

namespace sgrn::s7shell::shell
{

ScriptHexTable::ScriptHexTable(ScriptDataBlock* tp_db)
    : db_(tp_db) {
    if (db_) {
        db_->addRef();
    }
}

ScriptHexTable::~ScriptHexTable() {
    if (db_) {
        db_->release();
    }
}

void ScriptHexTable::addRef() {
    ++ref_count_;
}

void ScriptHexTable::release() {
    if (--ref_count_ == 0) {
        delete this;
    }
}

void ScriptHexTable::print() const {
    if (!db_) {
        return;
    }
    const uint8_t* p_data = db_->getBufferDataPointer();
    size_t size = db_->getBufferSize();
    fmt::print("Hex Dump of DB{} ({} bytes):\n", db_->getDbNumber(), size);
    for (size_t i = 0; i < size; i += 16) {
        fmt::print("{:04X}  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size) {
                fmt::print("{:02X} ", p_data[i + j]);
            } else {
                fmt::print("   ");
            }
            if (j == 7)
                fmt::print(" ");
        }
        fmt::print(" |");
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size) {
                char c = static_cast<char>(p_data[i + j]);
                if (std::isprint(static_cast<unsigned char>(c))) {
                    fmt::print("{}", c);
                } else {
                    fmt::print(".");
                }
            }
        }
        fmt::print("|\n");
    }
}

std::string ScriptHexTable::toString() const {
    if (!db_)
        return "";
    const uint8_t* p_data = db_->getBufferDataPointer();
    size_t size = db_->getBufferSize();
    std::string out;
    out += fmt::format("Hex Dump of DB{} ({} bytes):\n", db_->getDbNumber(), size);
    for (size_t i = 0; i < size; i += 16) {
        out += fmt::format("{:04X}  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size) {
                out += fmt::format("{:02X} ", p_data[i + j]);
            } else {
                out += "   ";
            }
            if (j == 7)
                out += " ";
        }
        out += " |";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size) {
                char c = static_cast<char>(p_data[i + j]);
                if (std::isprint(static_cast<unsigned char>(c))) {
                    out += c;
                } else {
                    out += ".";
                }
            }
        }
        out += "|\n";
    }
    return out;
}

ScriptHexTable* DataBlockToHexTableCast(ScriptDataBlock* tp_db) {
    if (!tp_db) {
        return nullptr;
    }
    return new ScriptHexTable(tp_db);
}

} // namespace sgrn::s7shell::shell
