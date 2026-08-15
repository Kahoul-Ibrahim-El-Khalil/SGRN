#pragma once

#include <json/json.h>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace sgrn
{

/**
 * @brief Lightweight tagged union for protocol-agnostic data exchange.
 *
 * Replaces JSON strings in the hot path to eliminate parsing overhead.
 */
class Value {
public:
    enum class Type { Null, Bool, Int64, UInt64, Double, String, Array, Object };

    Value()
        : data_(std::monostate{}) {
    }
    Value(bool t_v)
        : data_(t_v) {
    }
    Value(int64_t t_v)
        : data_(t_v) {
    }
    Value(int t_v)
        : data_(static_cast<int64_t>(t_v)) {
    }
    Value(uint64_t t_v)
        : data_(t_v) {
    }
    Value(uint32_t t_v)
        : data_(static_cast<uint64_t>(t_v)) {
    }
    Value(double t_v)
        : data_(t_v) {
    }
    Value(float t_v)
        : data_(static_cast<double>(t_v)) {
    }
    Value(std::string t_v)
        : data_(std::move(t_v)) {
    }
    Value(const char* tp_v)
        : data_(std::string(tp_v)) {
    }

    Type type() const {
        return std::visit(
            [](auto&& t_arg) -> Type {
                using T = std::decay_t<decltype(t_arg)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return Type::Null;
                else if constexpr (std::is_same_v<T, bool>)
                    return Type::Bool;
                else if constexpr (std::is_same_v<T, int64_t>)
                    return Type::Int64;
                else if constexpr (std::is_same_v<T, uint64_t>)
                    return Type::UInt64;
                else if constexpr (std::is_same_v<T, double>)
                    return Type::Double;
                else if constexpr (std::is_same_v<T, std::string>)
                    return Type::String;
                else if constexpr (std::is_same_v<T, std::vector<Value>>)
                    return Type::Array;
                else if constexpr (std::is_same_v<T, std::map<std::string, Value>>)
                    return Type::Object;
                return Type::Null;
            },
            data_);
    }

    bool isNull() const {
        return std::holds_alternative<std::monostate>(data_);
    }
    bool isBool() const {
        return std::holds_alternative<bool>(data_);
    }
    bool isInt() const {
        return std::holds_alternative<int64_t>(data_);
    }
    bool isUInt() const {
        return std::holds_alternative<uint64_t>(data_);
    }
    bool isDouble() const {
        return std::holds_alternative<double>(data_);
    }
    bool isString() const {
        return std::holds_alternative<std::string>(data_);
    }
    bool isArray() const {
        return std::holds_alternative<std::vector<Value>>(data_);
    }
    bool isObject() const {
        return std::holds_alternative<std::map<std::string, Value>>(data_);
    }

    bool asBool() const {
        return std::get<bool>(data_);
    }
    int64_t asInt() const {
        return std::get<int64_t>(data_);
    }
    uint64_t asUInt() const {
        return std::get<uint64_t>(data_);
    }
    double asDouble() const {
        return std::get<double>(data_);
    }
    const std::string& asString() const {
        return std::get<std::string>(data_);
    }
    const std::vector<Value>& asArray() const {
        return std::get<std::vector<Value>>(data_);
    }
    const std::map<std::string, Value>& asObject() const {
        return std::get<std::map<std::string, Value>>(data_);
    }

    Json::Value toJson() const {
        return std::visit(
            [](auto&& t_arg) -> Json::Value {
                using T = std::decay_t<decltype(t_arg)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return Json::Value::null;
                else if constexpr (std::is_same_v<T, bool>)
                    return Json::Value(t_arg);
                else if constexpr (std::is_same_v<T, int64_t>)
                    return Json::Value(static_cast<Json::Int64>(t_arg));
                else if constexpr (std::is_same_v<T, uint64_t>)
                    return Json::Value(static_cast<Json::UInt64>(t_arg));
                else if constexpr (std::is_same_v<T, double>)
                    return Json::Value(t_arg);
                else if constexpr (std::is_same_v<T, std::string>)
                    return Json::Value(t_arg);
                else if constexpr (std::is_same_v<T, std::vector<Value>>) {
                    Json::Value arr(Json::arrayValue);
                    for (const auto& t_v : t_arg)
                        arr.append(t_v.toJson());
                    return arr;
                } else if constexpr (std::is_same_v<T, std::map<std::string, Value>>) {
                    Json::Value obj(Json::objectValue);
                    for (const auto& [k, t_v] : t_arg)
                        obj[k] = t_v.toJson();
                    return obj;
                }
                return Json::Value::null;
            },
            data_);
    }

    static Value fromJson(const Json::Value& t_json) {
        if (t_json.isNull())
            return Value();
        if (t_json.isBool())
            return Value(t_json.asBool());
        if (t_json.isInt64())
            return Value(static_cast<int64_t>(t_json.asInt64()));
        if (t_json.isUInt64())
            return Value(static_cast<uint64_t>(t_json.asUInt64()));
        if (t_json.isDouble())
            return Value(t_json.asDouble());
        if (t_json.isString())
            return Value(t_json.asString());
        if (t_json.isArray()) {
            std::vector<Value> arr;
            for (const auto& t_v : t_json)
                arr.push_back(fromJson(t_v));
            Value val;
            val.data_ = std::move(arr);
            return val;
        }
        if (t_json.isObject()) {
            std::map<std::string, Value> obj;
            for (auto it = t_json.begin(); it != t_json.end(); ++it) {
                obj[it.name()] = fromJson(*it);
            }
            Value val;
            val.data_ = std::move(obj);
            return val;
        }
        return Value();
    }

private:
    std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string, std::vector<Value>, std::map<std::string, Value>> data_;
};

} // namespace sgrn
