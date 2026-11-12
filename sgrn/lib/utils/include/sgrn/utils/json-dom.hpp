#pragma once

// Transitional DOM surface for code that needs mutable JSON tree operations.
// Storage, parsing, and serialization are provided by Glaze's generic JSON
// implementation. New code should prefer sgrn::json::Value and its free
// helpers; this API keeps the migration mechanical for existing DOM-heavy
// gateway paths while the last call sites are converted to value semantics.

#include <sgrn/utils/glaze.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sgrn::json::dom
{

namespace detail
{
template <uint8_t Width>
struct PrettyOptions : glz::opts {
    uint8_t indentation_width = Width;
};
template <uint8_t Width>
inline constexpr PrettyOptions<Width> prettyOptions = [] {
    PrettyOptions<Width> options{};
    options.prettify = true;
    return options;
}();
} // namespace detail

using SizeType = std::size_t;
using Allocator = std::monostate;
template <class = void>
using MemoryPoolAllocator = Allocator;

// JSON value kinds used by the type-dispatch helpers in json.hpp.
enum class Type : uint8_t {
    Null,
    False,
    True,
    Object,
    Array,
    String,
    Number,
};

// ---------------------------------------------------------------------------
// Legacy (RapidJSON-style) spellings. Every old name forwards to its new one
// and is marked deprecated so stragglers show up as compiler warnings.
// Define SGRN_JSON_DOM_NO_LEGACY to remove them entirely.
// ---------------------------------------------------------------------------
#ifndef SGRN_JSON_DOM_NO_LEGACY
#define SGRN_JSON_DOM_LEGACY_(OLD, NEW)                                                                                                    \
    template <class... TArgs>                                                                                                              \
    [[deprecated("renamed to " #NEW)]] decltype(auto) OLD(TArgs&&... t_args) {                                                             \
        return NEW(std::forward<TArgs>(t_args)...);                                                                                        \
    }                                                                                                                                      \
    template <class... TArgs>                                                                                                              \
    [[deprecated("renamed to " #NEW)]] decltype(auto) OLD(TArgs&&... t_args) const {                                                       \
        return NEW(std::forward<TArgs>(t_args)...);                                                                                        \
    }
#endif

inline constexpr unsigned kParseCommentsFlag = 1u << 0;
inline constexpr unsigned kParseTrailingCommasFlag = 1u << 1;

#ifndef SGRN_JSON_DOM_NO_LEGACY
[[deprecated("renamed to Type::Null")]] inline constexpr Type kNullType = Type::Null;
[[deprecated("renamed to Type::False")]] inline constexpr Type kFalseType = Type::False;
[[deprecated("renamed to Type::True")]] inline constexpr Type kTrueType = Type::True;
[[deprecated("renamed to Type::Object")]] inline constexpr Type kObjectType = Type::Object;
[[deprecated("renamed to Type::Array")]] inline constexpr Type kArrayType = Type::Array;
[[deprecated("renamed to Type::String")]] inline constexpr Type kStringType = Type::String;
[[deprecated("renamed to Type::Number")]] inline constexpr Type kNumberType = Type::Number;
#endif

struct StringRefValue {
    std::string_view value;
};

inline StringRefValue stringRef(const char* value) {
    return {value ? std::string_view{value} : std::string_view{}};
}

inline StringRefValue stringRef(const char* value, SizeType length) {
    return {value ? std::string_view{value, length} : std::string_view{}};
}
#ifndef SGRN_JSON_DOM_NO_LEGACY
[[deprecated("renamed to stringRef")]] inline StringRefValue StringRef(const char* value) {
    return stringRef(value);
}
[[deprecated("renamed to stringRef")]] inline StringRefValue StringRef(const char* value, SizeType length) {
    return stringRef(value, length);
}
#endif

class Value {
public:
    using Generic = glz::generic_u64;
    using object_t = Generic::object_t;
    using array_t = Generic::array_t;

private:
    std::optional<Generic> owner_;
    Generic* mutable_value_{nullptr};
    const Generic* value_{nullptr};

    explicit Value(Generic& value, int) noexcept
        : mutable_value_(&value)
        , value_(&value) {
    }

    explicit Value(const Generic& value, long) noexcept
        : value_(&value) {
    }

    void bind(Generic& value) noexcept {
        owner_.reset();
        mutable_value_ = &value;
        value_ = &value;
    }

    void bind(const Generic& value) noexcept {
        owner_.reset();
        mutable_value_ = nullptr;
        value_ = &value;
    }

    [[nodiscard]] Generic& mutableRaw() {
        if (!mutable_value_)
            throw std::logic_error("attempted to mutate a const JSON value");
        return *mutable_value_;
    }

    [[nodiscard]] const Generic& raw() const noexcept {
        return *value_;
    }

    // Key lookup without materialising a std::string per call. Uses the
    // transparent (std::less<>) comparator of Glaze's object map when it is
    // available and falls back to the allocating path otherwise.
    template <class Obj>
    [[nodiscard]] static auto findKey(Obj& obj, std::string_view key) {
        if constexpr (requires { obj.find(key); })
            return obj.find(key);
        else
            return obj.find(std::string(key));
    }
    template <class Obj>
    [[nodiscard]] static auto& atKey(Obj& obj, std::string_view key) {
        auto it = findKey(obj, key);
        if (it == obj.end())
            throw std::out_of_range("JSON object has no such member");
        return it->second;
    }

public:
    class Name {
        std::string_view value_{};

    public:
        Name() = default;
        explicit Name(std::string_view value)
            : value_(value) {
        }
        void bind(std::string_view value) noexcept {
            value_ = value;
        }
        [[nodiscard]] const char* getString() const noexcept {
            return value_.data();
        }
        [[nodiscard]] SizeType getStringLength() const noexcept {
            return value_.size();
        }
#ifndef SGRN_JSON_DOM_NO_LEGACY
        SGRN_JSON_DOM_LEGACY_(GetString, getString)
        SGRN_JSON_DOM_LEGACY_(GetStringLength, getStringLength)
#endif
    };

    // Defined after Value is complete: each member proxy contains a Value
    // view, and a class cannot embed itself by value while still incomplete.
    struct Member;
    template <bool IsConst>
    class MemberIterator;

    class ArrayRange {
        const array_t* values_{nullptr};

    public:
        class iterator {
            const array_t* values_{nullptr};
            SizeType index_{0};

        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Value;
            using difference_type = std::ptrdiff_t;

            iterator() = default;
            iterator(const array_t* values, SizeType index)
                : values_(values)
                , index_(index) {
            }
            [[nodiscard]] Value operator*() const;
            iterator& operator++() {
                ++index_;
                return *this;
            }
            [[nodiscard]] bool operator==(const iterator& other) const {
                return values_ == other.values_ && index_ == other.index_;
            }
            [[nodiscard]] bool operator!=(const iterator& other) const {
                return !(*this == other);
            }
        };

        ArrayRange() = default;
        explicit ArrayRange(const array_t& values)
            : values_(&values) {
        }
        [[nodiscard]] iterator begin() const {
            return {values_, 0};
        }
        [[nodiscard]] iterator end() const {
            return {values_, values_ ? values_->size() : 0};
        }
        [[nodiscard]] SizeType size() const noexcept {
            return values_ ? values_->size() : 0;
        }
        [[nodiscard]] bool empty() const noexcept {
            return size() == 0;
        }
#ifndef SGRN_JSON_DOM_NO_LEGACY
        SGRN_JSON_DOM_LEGACY_(Size, size)
        SGRN_JSON_DOM_LEGACY_(Empty, empty)
#endif
    };

    Value()
        : owner_(std::in_place)
        , mutable_value_(&*owner_)
        , value_(&*owner_) {
    }

    explicit Value(Type kind)
        : Value() {
        switch (kind) {
            case Type::Object:
                mutableRaw() = object_t{};
                break;
            case Type::Array:
                mutableRaw() = array_t{};
                break;
            case Type::True:
                mutableRaw() = true;
                break;
            case Type::False:
                mutableRaw() = false;
                break;
            case Type::String:
                mutableRaw() = std::string{};
                break;
            case Type::Number:
                mutableRaw() = int64_t{0};
                break;
            case Type::Null:
            default:
                mutableRaw() = nullptr;
                break;
        }
    }

    explicit Value(bool value)
        : Value() {
        mutableRaw() = value;
    }
    explicit Value(const char* value)
        : Value() {
        mutableRaw() = std::string(value ? value : "");
    }
    Value(const char* value, Allocator&)
        : Value(value) {
    }
    Value(const char* value, SizeType length, Allocator&)
        : Value() {
        mutableRaw() = std::string(value ? value : "", value ? length : 0);
    }
    explicit Value(std::string value)
        : Value() {
        mutableRaw() = std::move(value);
    }
    explicit Value(std::string_view value)
        : Value(std::string(value)) {
    }
    explicit Value(StringRefValue value)
        : Value(value.value) {
    }
    template <std::integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    explicit Value(T value)
        : Value() {
        if constexpr (std::is_signed_v<T>)
            mutableRaw() = static_cast<int64_t>(value);
        else
            mutableRaw() = static_cast<uint64_t>(value);
    }
    template <std::floating_point T>
    explicit Value(T value)
        : Value() {
        mutableRaw() = static_cast<double>(value);
    }

    Value(const Value& other)
        : owner_(std::in_place, other.raw())
        , mutable_value_(&*owner_)
        , value_(&*owner_) {
    }
    Value(Value&& other) noexcept
        : owner_(std::move(other.owner_))
        , mutable_value_(other.mutable_value_)
        , value_(other.value_) {
        if (owner_) {
            mutable_value_ = &*owner_;
            value_ = &*owner_;
        }
        other.mutable_value_ = nullptr;
        other.value_ = nullptr;
    }
    Value& operator=(const Value& other) {
        mutableRaw() = other.raw();
        return *this;
    }
    Value& operator=(Value&& other) {
        mutableRaw() = other.raw();
        return *this;
    }
    Value& operator=(std::nullptr_t) {
        mutableRaw() = nullptr;
        return *this;
    }
    Value& operator=(bool value) {
        mutableRaw() = value;
        return *this;
    }
    Value& operator=(int64_t value) {
        mutableRaw() = value;
        return *this;
    }
    Value& operator=(uint64_t value) {
        mutableRaw() = value;
        return *this;
    }
    Value& operator=(double value) {
        mutableRaw() = value;
        return *this;
    }
    Value& operator=(std::string value) {
        mutableRaw() = std::move(value);
        return *this;
    }

    [[nodiscard]] static Value view(Generic& value) {
        return Value(value, 0);
    }
    [[nodiscard]] static Value view(const Generic& value) {
        return Value(value, 0L);
    }

    // Rebind a cursor wrapper to an existing node without copying or mutating
    // either tree. This is intentionally explicit: operator= keeps value-copy
    // semantics for DOM mutations.
    void rebindTo(Value& other) noexcept {
        if (other.mutable_value_)
            bind(*other.mutable_value_);
        else
            bind(other.raw());
    }
    void rebindTo(const Value& other) noexcept {
        if (other.mutable_value_)
            bind(*other.mutable_value_);
        else
            bind(other.raw());
    }

    [[nodiscard]] Generic& rawValue() {
        return mutableRaw();
    }
    [[nodiscard]] const Generic& rawValue() const noexcept {
        return raw();
    }

    // Glaze-native accessors keep the facade composable with newer traversal code.
    [[nodiscard]] bool is_object() const noexcept {
        return raw().is_object();
    }
    [[nodiscard]] bool is_array() const noexcept {
        return raw().is_array();
    }
    [[nodiscard]] bool is_string() const noexcept {
        return raw().is_string();
    }
    [[nodiscard]] bool is_number() const noexcept {
        return raw().is_number();
    }
    [[nodiscard]] bool is_boolean() const noexcept {
        return raw().is_boolean();
    }
    [[nodiscard]] bool is_null() const noexcept {
        return raw().is_null();
    }
    [[nodiscard]] auto& get_object() {
        return mutableRaw().get_object();
    }
    [[nodiscard]] const auto& get_object() const {
        return raw().get_object();
    }
    [[nodiscard]] auto& get_array() {
        return mutableRaw().get_array();
    }
    [[nodiscard]] const auto& get_array() const {
        return raw().get_array();
    }
    template <class T>
    [[nodiscard]] T get() const {
        return raw().template get<T>();
    }

    [[nodiscard]] bool isInt64() const noexcept {
        if (raw().is_int64())
            return true;
        return raw().is_uint64() && raw().get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    }
    [[nodiscard]] bool isUint64() const noexcept {
        if (raw().is_uint64())
            return true;
        return raw().is_int64() && raw().get<int64_t>() >= 0;
    }
    [[nodiscard]] bool isInt() const noexcept {
        if (raw().is_int64()) {
            const auto v = raw().get<int64_t>();
            return v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max();
        }
        return raw().is_uint64() && raw().get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    }
    [[nodiscard]] bool isUint() const noexcept {
        if (!isUint64())
            return false;
        return getUint64() <= std::numeric_limits<uint32_t>::max();
    }
    [[nodiscard]] bool isDouble() const noexcept {
        return raw().is_double();
    }
    [[nodiscard]] bool isFloat() const noexcept {
        if (!is_number())
            return false;
        const double value = getDouble();
        return std::isfinite(value) && value >= -std::numeric_limits<float>::max() && value <= std::numeric_limits<float>::max();
    }

    [[nodiscard]] const char* getString() const {
        return raw().get<std::string>().c_str();
    }
    [[nodiscard]] SizeType getStringLength() const {
        return raw().get<std::string>().size();
    }
    [[nodiscard]] bool getBool() const {
        return raw().get<bool>();
    }
    [[nodiscard]] int getInt() const {
        return raw().as<int>();
    }
    [[nodiscard]] unsigned getUint() const {
        return raw().as<unsigned>();
    }
    [[nodiscard]] int64_t getInt64() const {
        return raw().as<int64_t>();
    }
    [[nodiscard]] uint64_t getUint64() const {
        return raw().as<uint64_t>();
    }
    [[nodiscard]] double getDouble() const {
        return raw().as_number();
    }
    [[nodiscard]] float getFloat() const {
        return static_cast<float>(getDouble());
    }

    [[nodiscard]] Type type() const noexcept {
        if (is_null())
            return Type::Null;
        if (is_boolean())
            return getBool() ? Type::True : Type::False;
        if (is_number())
            return Type::Number;
        if (is_string())
            return Type::String;
        if (is_array())
            return Type::Array;
        if (is_object())
            return Type::Object;
        return Type::Null;
    }

    [[nodiscard]] SizeType size() const noexcept {
        return raw().size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return raw().empty();
    }
    [[nodiscard]] bool objectEmpty() const noexcept {
        return is_object() && raw().get_object().empty();
    }
    [[nodiscard]] bool arrayEmpty() const noexcept {
        return is_array() && raw().get_array().empty();
    }

    [[nodiscard]] bool hasMember(std::string_view key) const {
        return is_object() && findKey(raw().get_object(), key) != raw().get_object().end();
    }
    [[nodiscard]] Value operator[](const char* key) {
        return view(atKey(mutableRaw().get_object(), std::string_view{key ? key : ""}));
    }
    [[nodiscard]] Value operator[](std::string_view key) {
        return view(atKey(mutableRaw().get_object(), key));
    }
    [[nodiscard]] Value operator[](const char* key) const {
        return view(atKey(raw().get_object(), std::string_view{key ? key : ""}));
    }
    [[nodiscard]] Value operator[](std::string_view key) const {
        return view(atKey(raw().get_object(), key));
    }
    [[nodiscard]] Value operator[](SizeType index) {
        return view(mutableRaw().get_array().at(index));
    }
    [[nodiscard]] Value operator[](SizeType index) const {
        return view(raw().get_array().at(index));
    }

    // Member iteration is read-only: the iterators bind const views, so they
    // work on owning values and const-bound views alike (calling the mutable
    // variants used to throw on views — see docs/json-glaze-decisions.md).
    [[nodiscard]] MemberIterator<true> memberBegin() const;
    [[nodiscard]] MemberIterator<true> memberEnd() const;
    [[nodiscard]] MemberIterator<true> findMember(const char* key) const;
    // string_view overloads avoid a strlen/c_str() round trip; lookup is
    // allocation-free (see findKey).
    [[nodiscard]] MemberIterator<true> findMember(std::string_view key) const;
    [[nodiscard]] SizeType memberCount() const noexcept {
        return is_object() ? raw().get_object().size() : 0;
    }

    [[nodiscard]] ArrayRange elements() const {
        return ArrayRange{raw().get_array()};
    }

    Value& setObject() {
        mutableRaw() = object_t{};
        return *this;
    }
    Value& setArray() {
        mutableRaw() = array_t{};
        return *this;
    }
    Value& setNull() {
        mutableRaw() = nullptr;
        return *this;
    }
    Value& setBool(bool value) {
        mutableRaw() = value;
        return *this;
    }
    Value& setInt(int value) {
        mutableRaw() = static_cast<int64_t>(value);
        return *this;
    }
    Value& setUint(unsigned value) {
        mutableRaw() = static_cast<uint64_t>(value);
        return *this;
    }
    Value& setInt64(int64_t value) {
        mutableRaw() = value;
        return *this;
    }
    Value& setUint64(uint64_t value) {
        mutableRaw() = value;
        return *this;
    }
    Value& setDouble(double value) {
        mutableRaw() = value;
        return *this;
    }
    Value& setString(const char* value, Allocator&) {
        mutableRaw() = std::string(value ? value : "");
        return *this;
    }
    Value& setString(const char* value, SizeType length, Allocator&) {
        mutableRaw() = std::string(value ? value : "", value ? length : 0);
        return *this;
    }

    void copyFrom(const Value& other, Allocator&) {
        mutableRaw() = other.raw();
    }
    void copyFrom(const Name& other, Allocator&) {
        mutableRaw() = std::string(other.getString(), other.getStringLength());
    }
    void swap(Value& other) {
        std::swap(mutableRaw(), other.mutableRaw());
    }

    friend bool operator==(const Value& lhs, const Value& rhs);
    friend bool operator!=(const Value& lhs, const Value& rhs);
    void removeMember(const char* key) {
        removeMember(std::string_view{key ? key : ""});
    }
    void removeMember(std::string_view key) {
        // find + erase(iterator): no std::string temporary, and it does not
        // depend on C++23 heterogeneous erase being available in the stdlib.
        auto& object = mutableRaw().get_object();
        if (const auto it = findKey(object, key); it != object.end())
            object.erase(it);
    }

    void addMember(std::string_view key, const Value& value, Allocator&) {
        auto& object = mutableRaw().get_object();
        if (const auto it = findKey(object, key); it != object.end())
            it->second = value.raw(); // overwrite: no key allocation
        else
            object.emplace(std::string(key), value.raw()); // allocate the key only on insert
    }
    void addMember(const char* key, const Value& value, Allocator& alloc) {
        addMember(std::string_view{key ? key : ""}, value, alloc);
    }
    void addMember(StringRefValue key, const Value& value, Allocator& alloc) {
        addMember(key.value, value, alloc);
    }
    Value(const Name& value, Allocator&)
        : Value(std::string(value.getString(), value.getStringLength())) {
    }

    void addMember(const Value& key, const Value& value, Allocator& alloc) {
        addMember(std::string_view{key.getString(), key.getStringLength()}, value, alloc);
    }
    template <class T>
        requires(std::is_arithmetic_v<T> && !std::same_as<std::remove_cv_t<T>, bool>)
    void addMember(const char* key, T value, Allocator& alloc) {
        addMember(key, Value(value), alloc);
    }
    void addMember(const char* key, bool value, Allocator& alloc) {
        addMember(key, Value(value), alloc);
    }
    void addMember(const char* key, const char* value, Allocator& alloc) {
        addMember(key, Value(value), alloc);
    }
    void pushBack(const Value& value, Allocator&) {
        mutableRaw().get_array().push_back(value.raw());
    }

    template <class Writer>
    bool accept(Writer& writer) const {
        return writer.writeValue(*this);
    }

#ifndef SGRN_JSON_DOM_NO_LEGACY
    SGRN_JSON_DOM_LEGACY_(IsObject, is_object)
    SGRN_JSON_DOM_LEGACY_(IsArray, is_array)
    SGRN_JSON_DOM_LEGACY_(IsString, is_string)
    SGRN_JSON_DOM_LEGACY_(IsBool, is_boolean)
    SGRN_JSON_DOM_LEGACY_(IsNull, is_null)
    SGRN_JSON_DOM_LEGACY_(IsNumber, is_number)
    SGRN_JSON_DOM_LEGACY_(IsInt, isInt)
    SGRN_JSON_DOM_LEGACY_(IsInt64, isInt64)
    SGRN_JSON_DOM_LEGACY_(IsUint, isUint)
    SGRN_JSON_DOM_LEGACY_(IsUint64, isUint64)
    SGRN_JSON_DOM_LEGACY_(IsDouble, isDouble)
    SGRN_JSON_DOM_LEGACY_(IsFloat, isFloat)
    SGRN_JSON_DOM_LEGACY_(GetString, getString)
    SGRN_JSON_DOM_LEGACY_(GetStringLength, getStringLength)
    SGRN_JSON_DOM_LEGACY_(GetBool, getBool)
    SGRN_JSON_DOM_LEGACY_(GetInt, getInt)
    SGRN_JSON_DOM_LEGACY_(GetUint, getUint)
    SGRN_JSON_DOM_LEGACY_(GetInt64, getInt64)
    SGRN_JSON_DOM_LEGACY_(GetUint64, getUint64)
    SGRN_JSON_DOM_LEGACY_(GetDouble, getDouble)
    SGRN_JSON_DOM_LEGACY_(GetFloat, getFloat)
    SGRN_JSON_DOM_LEGACY_(GetType, type)
    SGRN_JSON_DOM_LEGACY_(Size, size)
    SGRN_JSON_DOM_LEGACY_(GetSize, size)
    SGRN_JSON_DOM_LEGACY_(Empty, empty)
    SGRN_JSON_DOM_LEGACY_(ObjectEmpty, objectEmpty)
    SGRN_JSON_DOM_LEGACY_(ArrayEmpty, arrayEmpty)
    SGRN_JSON_DOM_LEGACY_(HasMember, hasMember)
    SGRN_JSON_DOM_LEGACY_(MemberBegin, memberBegin)
    SGRN_JSON_DOM_LEGACY_(MemberEnd, memberEnd)
    SGRN_JSON_DOM_LEGACY_(FindMember, findMember)
    SGRN_JSON_DOM_LEGACY_(MemberCount, memberCount)
    SGRN_JSON_DOM_LEGACY_(GetArray, elements)
    SGRN_JSON_DOM_LEGACY_(SetObject, setObject)
    SGRN_JSON_DOM_LEGACY_(SetArray, setArray)
    SGRN_JSON_DOM_LEGACY_(SetNull, setNull)
    SGRN_JSON_DOM_LEGACY_(SetBool, setBool)
    SGRN_JSON_DOM_LEGACY_(SetInt, setInt)
    SGRN_JSON_DOM_LEGACY_(SetUint, setUint)
    SGRN_JSON_DOM_LEGACY_(SetInt64, setInt64)
    SGRN_JSON_DOM_LEGACY_(SetUint64, setUint64)
    SGRN_JSON_DOM_LEGACY_(SetDouble, setDouble)
    SGRN_JSON_DOM_LEGACY_(SetString, setString)
    SGRN_JSON_DOM_LEGACY_(CopyFrom, copyFrom)
    SGRN_JSON_DOM_LEGACY_(Swap, swap)
    SGRN_JSON_DOM_LEGACY_(RemoveMember, removeMember)
    SGRN_JSON_DOM_LEGACY_(AddMember, addMember)
    SGRN_JSON_DOM_LEGACY_(PushBack, pushBack)
    SGRN_JSON_DOM_LEGACY_(Accept, accept)
#endif
};

// Member and its iterator proxy must be defined after Value is complete.
struct Value::Member {
    Value::Name name;
    Value value;
};

template <bool IsConst>
class Value::MemberIterator {
    using ObjectIterator = std::conditional_t<IsConst, typename Value::object_t::const_iterator, typename Value::object_t::iterator>;
    ObjectIterator iterator_{};
    mutable Member proxy_{};

public:
    MemberIterator() = default;
    explicit MemberIterator(ObjectIterator it)
        : iterator_(it) {
    }
    MemberIterator& operator++() {
        ++iterator_;
        return *this;
    }
    [[nodiscard]] bool operator==(const MemberIterator& other) const {
        return iterator_ == other.iterator_;
    }
    [[nodiscard]] bool operator!=(const MemberIterator& other) const {
        return !(*this == other);
    }
    Member& operator*() const {
        return *operator->();
    }
    Member* operator->() const {
        proxy_.name.bind(iterator_->first);
        if constexpr (IsConst) {
            proxy_.value.bind(static_cast<const Value::Generic&>(iterator_->second));
        } else {
            proxy_.value.bind(const_cast<Value::Generic&>(iterator_->second));
        }
        return &proxy_;
    }
};

inline Value Value::ArrayRange::iterator::operator*() const {
    return Value::view((*values_)[index_]);
}

inline bool operator==(const Value& lhs, const Value& rhs) {
    if (lhs.is_number() && rhs.is_number()) {
        const auto& left = lhs.raw();
        const auto& right = rhs.raw();
        if (left.is_uint64() && right.is_uint64())
            return left.get<uint64_t>() == right.get<uint64_t>();
        if (left.is_int64() && right.is_int64())
            return left.get<int64_t>() == right.get<int64_t>();
        if (left.is_int64() && right.is_uint64()) {
            const auto value = left.get<int64_t>();
            return value >= 0 && static_cast<uint64_t>(value) == right.get<uint64_t>();
        }
        if (left.is_uint64() && right.is_int64()) {
            const auto value = right.get<int64_t>();
            return value >= 0 && left.get<uint64_t>() == static_cast<uint64_t>(value);
        }
        return left.as_number() == right.as_number();
    }
    if (lhs.type() != rhs.type())
        return false;
    if (lhs.is_null())
        return true;
    if (lhs.is_boolean())
        return lhs.getBool() == rhs.getBool();
    if (lhs.is_string())
        return lhs.raw().get<std::string>() == rhs.raw().get<std::string>();
    if (lhs.is_array()) {
        if (lhs.size() != rhs.size())
            return false;
        for (SizeType i = 0; i < lhs.size(); ++i) {
            if (!(lhs[i] == rhs[i]))
                return false;
        }
        return true;
    }
    if (lhs.is_object()) {
        if (lhs.memberCount() != rhs.memberCount())
            return false;
        for (auto it = lhs.memberBegin(); it != lhs.memberEnd(); ++it) {
            const std::string_view key{it->name.getString(), it->name.getStringLength()};
            if (!rhs.hasMember(key) || !(it->value == rhs[key]))
                return false;
        }
        return true;
    }
    return false;
}
inline bool operator!=(const Value& lhs, const Value& rhs) {
    return !(lhs == rhs);
}

inline Value::MemberIterator<true> Value::memberBegin() const {
    return MemberIterator<true>{raw().get_object().begin()};
}
inline Value::MemberIterator<true> Value::memberEnd() const {
    return MemberIterator<true>{raw().get_object().end()};
}
inline Value::MemberIterator<true> Value::findMember(std::string_view key) const {
    return MemberIterator<true>{findKey(raw().get_object(), key)};
}
inline Value::MemberIterator<true> Value::findMember(const char* key) const {
    return MemberIterator<true>{findKey(raw().get_object(), std::string_view{key ? key : ""})};
}

class Document : public Value {
    Allocator allocator_{};
    std::string parse_error_{};
    SizeType error_offset_{0};
    uint32_t error_code_{0};

    static std::string stripJsonCommentsAndTrailingCommas(std::string_view input) {
        std::string clean(input);
        bool in_string = false;
        bool escaped = false;
        for (SizeType i = 0; i < clean.size(); ++i) {
            const char c = clean[i];
            if (in_string) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    in_string = false;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '/') {
                clean[i++] = ' ';
                clean[i] = ' ';
                while (i + 1 < clean.size() && clean[i + 1] != '\n' && clean[i + 1] != '\r')
                    clean[++i] = ' ';
            } else if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '*') {
                const SizeType comment_end = clean.find("*/", i + 2);
                // Leave an unterminated block comment untouched so the strict
                // Glaze reader rejects it instead of accepting a truncated file.
                if (comment_end == std::string::npos)
                    return std::string(input);
                for (SizeType pos = i; pos < comment_end + 2; ++pos) {
                    if (clean[pos] != '\n' && clean[pos] != '\r')
                        clean[pos] = ' ';
                }
                i = comment_end + 1;
            }
        }
        in_string = false;
        escaped = false;
        for (SizeType i = 0; i < clean.size(); ++i) {
            const char c = clean[i];
            if (in_string) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    in_string = false;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == ',') {
                SizeType next = i + 1;
                while (next < clean.size() && (clean[next] == ' ' || clean[next] == '\t' || clean[next] == '\r' || clean[next] == '\n'))
                    ++next;
                if (next < clean.size() && (clean[next] == '}' || clean[next] == ']'))
                    clean[i] = ' ';
            }
        }
        return clean;
    }

    Document& parseImpl(std::string_view input, bool relaxed) {
        parse_error_.clear();
        error_offset_ = 0;
        error_code_ = 0;
        const std::string normalized = relaxed ? stripJsonCommentsAndTrailingCommas(input) : std::string(input);
        Generic parsed{};
        const auto ec = glz::read<kReadOpts>(parsed, normalized);
        if (ec) {
            parse_error_ = glz::format_error(ec, normalized);
            error_offset_ = ec.count;
            error_code_ = static_cast<uint32_t>(ec.ec);
            setNull();
            return *this;
        }
        rawValue() = std::move(parsed);
        return *this;
    }

public:
    using AllocatorType = Allocator;

    Document& parse(const char* input) {
        return parseImpl(input ? std::string_view{input} : std::string_view{}, false);
    }
    Document& parse(std::string_view input) {
        return parseImpl(input, false);
    }
    Document& parse(const char* input, SizeType length) {
        return parseImpl(input ? std::string_view{input, length} : std::string_view{}, false);
    }
    template <unsigned Flags>
    Document& parse(const char* input) {
        return parseImpl(input ? std::string_view{input} : std::string_view{}, Flags != 0);
    }
    template <unsigned Flags>
    Document& parse(std::string_view input) {
        return parseImpl(input, Flags != 0);
    }
    template <unsigned Flags>
    Document& parse(const char* input, SizeType length) {
        return parseImpl(input ? std::string_view{input, length} : std::string_view{}, Flags != 0);
    }

    [[nodiscard]] bool hasParseError() const noexcept {
        return !parse_error_.empty();
    }
    [[nodiscard]] SizeType errorOffset() const noexcept {
        return error_offset_;
    }
    [[nodiscard]] uint32_t errorCode() const noexcept {
        return error_code_;
    }
    [[nodiscard]] const std::string& errorMessage() const noexcept {
        return parse_error_;
    }
    [[nodiscard]] Allocator& allocator() noexcept {
        return allocator_;
    }

#ifndef SGRN_JSON_DOM_NO_LEGACY
    SGRN_JSON_DOM_LEGACY_(HasParseError, hasParseError)
    SGRN_JSON_DOM_LEGACY_(GetErrorOffset, errorOffset)
    SGRN_JSON_DOM_LEGACY_(GetParseError, errorCode)
    SGRN_JSON_DOM_LEGACY_(GetParseErrorMessage, errorMessage)
    SGRN_JSON_DOM_LEGACY_(GetAllocator, allocator)
#endif
#ifndef SGRN_JSON_DOM_NO_LEGACY
    template <class... TArgs>
    [[deprecated("renamed to parse")]] Document& Parse(TArgs&&... t_args) {
        return parse(std::forward<TArgs>(t_args)...);
    }
    template <unsigned Flags, class... TArgs>
    [[deprecated("renamed to parse")]] Document& Parse(TArgs&&... t_args) {
        return parse<Flags>(std::forward<TArgs>(t_args)...);
    }
#endif
};

class StringBuffer {
    std::string buffer_;

public:
    StringBuffer() = default;
    [[nodiscard]] const char* getString() const noexcept {
        return buffer_.c_str();
    }
    [[nodiscard]] std::string_view getStringView() const noexcept {
        return buffer_;
    }
    [[nodiscard]] std::string extractString() noexcept {
        return std::move(buffer_);
    }
    [[nodiscard]] SizeType size() const noexcept {
        return buffer_.size();
    }
    void clear() noexcept {
        buffer_.clear();
    }
    void put(char c) {
        buffer_.push_back(c);
    }
    void set(std::string value) {
        buffer_ = std::move(value);
    }
    void append(std::string_view value) {
        buffer_.append(value);
    }
    [[nodiscard]] std::string& mutableString() noexcept {
        return buffer_;
    }

#ifndef SGRN_JSON_DOM_NO_LEGACY
    SGRN_JSON_DOM_LEGACY_(GetString, getString)
    SGRN_JSON_DOM_LEGACY_(GetSize, size)
    SGRN_JSON_DOM_LEGACY_(Clear, clear)
    SGRN_JSON_DOM_LEGACY_(Put, put)
    SGRN_JSON_DOM_LEGACY_(Set, set)
    SGRN_JSON_DOM_LEGACY_(Append, append)
#endif
};

template <class Buffer>
class Writer {
protected:
    Buffer& buffer_;
    bool pretty_{false};
    unsigned indent_width_{4};
    Value root_{};
    std::vector<Value> stack_{};
    std::string pending_key_{};
    bool has_root_{false};

    [[nodiscard]] Value attach(const Value& value) {
        if (stack_.empty()) {
            root_ = value;
            has_root_ = true;
            return Value::view(root_.rawValue());
        }
        Value& parent = stack_.back();
        if (parent.is_array()) {
            auto& array = parent.rawValue().get_array();
            array.push_back(value.rawValue());
            return Value::view(array.back());
        }
        if (parent.is_object()) {
            const std::string key = std::exchange(pending_key_, {});
            auto& object = parent.rawValue().get_object();
            auto [it, inserted] = object.insert_or_assign(key, value.rawValue());
            (void)inserted;
            return Value::view(it->second);
        }
        throw std::logic_error("JSON writer parent is not a container");
    }

    bool writeScalar(const Value& value) {
        (void)attach(value);
        if (stack_.empty())
            return flush();
        return true;
    }

    template <uint8_t Width>
    bool flushPretty() {
        std::string encoded;
        const auto ec = glz::write<detail::prettyOptions<Width>>(root_.rawValue(), encoded);
        if (ec)
            return false;
        buffer_.set(std::move(encoded));
        return true;
    }

    template <uint8_t Width>
    bool writePrettyValue(const Value& value) {
        std::string encoded;
        const auto ec = glz::write<detail::prettyOptions<Width>>(value.rawValue(), encoded);
        if (ec)
            return false;
        buffer_.set(std::move(encoded));
        return true;
    }

public:
    explicit Writer(Buffer& buffer, bool pretty = false)
        : buffer_(buffer)
        , pretty_(pretty) {
    }

    bool startObject() {
        Value value(Type::Object);
        Value child = attach(value);
        stack_.push_back(std::move(child));
        return true;
    }
    bool endObject(SizeType = 0) {
        if (stack_.empty() || !stack_.back().is_object())
            return false;
        stack_.pop_back();
        return stack_.empty() ? flush() : true;
    }
    bool startArray() {
        Value value(Type::Array);
        Value child = attach(value);
        stack_.push_back(std::move(child));
        return true;
    }
    bool endArray(SizeType = 0) {
        if (stack_.empty() || !stack_.back().is_array())
            return false;
        stack_.pop_back();
        return stack_.empty() ? flush() : true;
    }
    bool key(const char* key) {
        pending_key_ = key ? key : "";
        return true;
    }
    bool key(const char* key, SizeType length) {
        pending_key_.assign(key ? key : "", key ? length : 0);
        return true;
    }
    bool key(std::string_view key) {
        pending_key_.assign(key);
        return true;
    }
    bool string(const char* value) {
        return writeScalar(Value(value));
    }
    bool string(const char* value, SizeType length) {
        return writeScalar(Value(value ? std::string_view{value, length} : std::string_view{}));
    }
    bool string(std::string_view value) {
        return writeScalar(Value(value));
    }
    bool boolean(bool value) {
        return writeScalar(Value(value));
    }
    bool int32(int value) {
        return writeScalar(Value(value));
    }
    bool uint32(unsigned value) {
        return writeScalar(Value(value));
    }
    bool int64(int64_t value) {
        return writeScalar(Value(value));
    }
    bool uint64(uint64_t value) {
        return writeScalar(Value(value));
    }
    bool float64(double value) {
        return writeScalar(Value(value));
    }
    bool nullValue() {
        return writeScalar(Value{});
    }
    bool rawJson(const char* value, SizeType length, Type = Type::Null) {
        Document parsed;
        parsed.parse(value, length);
        if (parsed.hasParseError())
            return false;
        return writeScalar(parsed);
    }
    bool flush() {
        if (!has_root_)
            return true;
        if (!pretty_) {
            // writeTo clears the string but preserves its capacity, allowing
            // thread-local StringBuffer users to reuse the previous allocation.
            auto result = sgrn::json::writeTo(buffer_.mutableString(), root_.rawValue());
            return !result.hasError();
        }
        switch (indent_width_) {
            case 1:
                return flushPretty<1>();
            case 2:
                return flushPretty<2>();
            case 3:
                return flushPretty<3>();
            case 4:
                return flushPretty<4>();
            case 8:
                return flushPretty<8>();
            default:
                return flushPretty<4>();
        }
    }
    bool writeValue(const Value& value) {
        // accept() is the common path for serializing an already-built tree.
        // Write it directly: assigning to root_ here deep-copies the complete
        // Glaze DOM before performing the serialization walk.
        stack_.clear();
        pending_key_.clear();
        if (has_root_)
            root_.setNull();
        has_root_ = false;

        if (!pretty_) {
            const auto result = sgrn::json::writeTo(buffer_.mutableString(), value.rawValue());
            return !result.hasError();
        }

        switch (indent_width_) {
            case 1:
                return writePrettyValue<1>(value);
            case 2:
                return writePrettyValue<2>(value);
            case 3:
                return writePrettyValue<3>(value);
            case 4:
                return writePrettyValue<4>(value);
            case 8:
                return writePrettyValue<8>(value);
            default:
                return writePrettyValue<4>(value);
        }
    }

#ifndef SGRN_JSON_DOM_NO_LEGACY
    SGRN_JSON_DOM_LEGACY_(StartObject, startObject)
    SGRN_JSON_DOM_LEGACY_(EndObject, endObject)
    SGRN_JSON_DOM_LEGACY_(StartArray, startArray)
    SGRN_JSON_DOM_LEGACY_(EndArray, endArray)
    SGRN_JSON_DOM_LEGACY_(Key, key)
    SGRN_JSON_DOM_LEGACY_(String, string)
    SGRN_JSON_DOM_LEGACY_(Bool, boolean)
    SGRN_JSON_DOM_LEGACY_(Int, int32)
    SGRN_JSON_DOM_LEGACY_(Uint, uint32)
    SGRN_JSON_DOM_LEGACY_(Int64, int64)
    SGRN_JSON_DOM_LEGACY_(Uint64, uint64)
    SGRN_JSON_DOM_LEGACY_(Double, float64)
    SGRN_JSON_DOM_LEGACY_(Null, nullValue)
    SGRN_JSON_DOM_LEGACY_(RawValue, rawJson)
    SGRN_JSON_DOM_LEGACY_(Flush, flush)
    SGRN_JSON_DOM_LEGACY_(WriteValue, writeValue)
#endif
};

template <class Buffer>
class PrettyWriter : public Writer<Buffer> {
public:
    explicit PrettyWriter(Buffer& buffer)
        : Writer<Buffer>(buffer, true) {
    }
    void setIndent(char, unsigned width) {
        // The current call sites use spaces; preserve the widths used by the
        // gateway/shell (2) and the writer default (4).
        this->indent_width_ = width;
    }
#ifndef SGRN_JSON_DOM_NO_LEGACY
    [[deprecated("renamed to setIndent")]] void SetIndent(char c, unsigned width) {
        setIndent(c, width);
    }
#endif
};

} // namespace sgrn::json::dom
