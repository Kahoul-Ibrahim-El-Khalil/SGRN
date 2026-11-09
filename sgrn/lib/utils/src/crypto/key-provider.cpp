#include "sgrn/utils/crypto/key-provider.hpp"
#include "sgrn/utils/crypto.hpp"

#include <iomanip>
#include <openssl/rand.h>
#include <sstream>

namespace sgrn::utils::crypto
{

std::string keyStatusToString(KeyStatus t_status) {
    switch (t_status) {
        case KeyStatus::PENDING:
            return "PENDING";
        case KeyStatus::ACTIVE:
            return "ACTIVE";
        case KeyStatus::SHREDDED:
            return "SHREDDED";
    }
    return "UNKNOWN";
}

Result<KeyStatus> keyStatusFromString(std::string_view t_str) {
    if (t_str == "PENDING") {
        return KeyStatus::PENDING;
    }
    if (t_str == "ACTIVE") {
        return KeyStatus::ACTIVE;
    }
    if (t_str == "SHREDDED") {
        return KeyStatus::SHREDDED;
    }
    return ::sgrn::Error("Unknown key status string");
}

Result<KeyRecord> KeyRecord::clone() const {
    KeyRecord copy;
    copy.key_id = key_id;
    copy.file_id = file_id;
    copy.object_key = object_key;
    copy.status = status;
    copy.wrap_scheme = wrap_scheme;
    copy.kek_ref = kek_ref;
    copy.algorithm = algorithm;
    copy.chunk_size = chunk_size;
    copy.created_at = created_at;
    copy.activated_at = activated_at;
    copy.shredded_at = shredded_at;

    if (dek.isValid()) {
        auto dek_copy = dek.clone();
        RETURN_ERROR_IF(dek_copy.hasError(), dek_copy.error());
        copy.dek = std::move(dek_copy.value());
    }
    return copy;
}

InMemoryKeyProvider::~InMemoryKeyProvider() {
    std::lock_guard lock(m_mutex);
    for (auto& [_, record] : m_keys) {
        record.dek.zeroize();
    }
    m_keys.clear();
}

static std::string generateRandomUuid() {
    uint8_t bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        return "";
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // variant 1

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << '-';
        }
        ss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return ss.str();
}

Result<KeyRecord> InMemoryKeyProvider::createPending(const std::string& t_object_key, const std::string& t_wrap_scheme) {
    auto dek_res = Dek::generate();
    RETURN_ERROR_IF(dek_res.hasError(), dek_res.error());

    std::string key_id = generateRandomUuid();
    RETURN_ERROR_IF(key_id.empty(), "Failed to generate UUID for key_id");

    KeyRecord record;
    record.key_id = key_id;
    record.file_id = std::nullopt;
    record.object_key = t_object_key;
    record.status = KeyStatus::PENDING;
    record.wrap_scheme = t_wrap_scheme.empty() ? "none" : t_wrap_scheme;
    record.algorithm = "AES-256-GCM";
    record.chunk_size = 1024 * 1024;
    record.dek = std::move(dek_res.value());
    record.created_at = std::chrono::system_clock::now();

    auto out_clone = record.clone();
    RETURN_ERROR_IF(out_clone.hasError(), out_clone.error());

    {
        std::lock_guard lock(m_mutex);
        m_keys.emplace(key_id, std::move(record));
    }

    return out_clone;
}

Result<void> InMemoryKeyProvider::activate(const std::string& t_key_id, const std::string& t_file_id) {
    RETURN_ERROR_IF(t_key_id.empty(), "key_id cannot be empty");
    RETURN_ERROR_IF(t_file_id.empty(), "file_id cannot be empty");

    std::lock_guard lock(m_mutex);
    auto it = m_keys.find(t_key_id);
    RETURN_ERROR_IF(it == m_keys.end(), "Key not found");

    RETURN_ERROR_IF(it->second.status == KeyStatus::SHREDDED, "Cannot activate a shredded key");

    it->second.file_id = t_file_id;
    it->second.status = KeyStatus::ACTIVE;
    it->second.activated_at = std::chrono::system_clock::now();
    return {};
}

Result<KeyRecord> InMemoryKeyProvider::get(const std::string& t_key_id) {
    std::lock_guard lock(m_mutex);
    auto it = m_keys.find(t_key_id);

    RETURN_ERROR_IF(it == m_keys.end(), "Key not found");
    RETURN_ERROR_IF(it->second.status == KeyStatus::SHREDDED, "Key is shredded and unrecoverable");

    return it->second.clone();
}

Result<void> InMemoryKeyProvider::shred(const std::string& t_key_id) {
    std::lock_guard lock(m_mutex);
    auto it = m_keys.find(t_key_id);
    RETURN_ERROR_IF(it == m_keys.end(), "Key not found");

    it->second.dek.zeroize();
    it->second.status = KeyStatus::SHREDDED;
    it->second.shredded_at = std::chrono::system_clock::now();
    return {};
}

Result<std::vector<KeyRecord>> InMemoryKeyProvider::listForReconcile() {
    std::lock_guard lock(m_mutex);
    std::vector<KeyRecord> out;
    out.reserve(m_keys.size());
    for (const auto& [_, record] : m_keys) {
        auto copy = record.clone();
        if (copy.hasValue()) {
            out.push_back(std::move(copy.value()));
        }
    }
    return out;
}

} // namespace sgrn::utils::crypto
