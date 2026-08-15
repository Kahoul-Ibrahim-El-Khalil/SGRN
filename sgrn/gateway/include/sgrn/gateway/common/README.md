# Common Adapter Utilities

This directory contains reusable utilities for all protocol adapters in the SGRN gateway. These utilities reduce code duplication and provide consistent patterns across all adapters.

## Contents

### 1. AdapterBase.hpp
CRTP base class for protocol adapters providing common lifecycle management without virtual dispatch.

**Usage:**
```cpp
class MyAdapter : public AdapterBase<MyAdapter> {
public:
    using AdapterBase::AdapterBase; // Inherit constructor
    
    sgrn::Result<void> start(const std::string& ip, uint16_t port) {
        // Pre-start configuration
        return AdapterBase::start(ip, port);
    }
    
private:
    bool configure(const std::string& ip, uint16_t port) {
        // Setup code
        return true;
    }
    
    void serveLoop() {
        while (runningFlag().load()) {
            // Serve loop
        }
    }
};
```

**Benefits:**
- No virtual functions (zero overhead)
- Common start/stop lifecycle
- Automatic thread management
- Access to memory() and security()

### 2. PathUtils.hpp
Static utility class for path manipulation between different formats (topics, PLC paths).

**Key Functions:**
- `topicToPlcPath()` - Convert "ReactorCore/speed" → "ReactorCore.speed" (returns new string)
- `topicToPlcPathInPlace()` - Convert in-place (no allocation)
- `plcPathToTopic()` - Convert "ReactorCore.speed" → "ReactorCore/speed" (returns new string)
- `plcPathToTopicInPlace()` - Convert in-place (no allocation)
- `parsePlcPath()` - Parse "ReactorCore.speed" → Result<pair<db_number, field_path>, error>
- `buildTopic()` - Build topic path from DB number/name (protocol-agnostic)

**Usage:**
```cpp
// Convert topic to PLC path (creates new string)
std::string plc_path = PathUtils::topicToPlcPath("ReactorCore/speed");

// Convert in-place (no allocation - better performance)
std::string path = "ReactorCore/speed";
PathUtils::topicToPlcPathInPlace(path); // path is now "ReactorCore.speed"

// Parse PLC path (with error handling)
auto result = PathUtils::parsePlcPath("ReactorCore.speed", schema_store);
if (result.hasError()) {
    return result.error(); // "DB not found: ReactorCore"
}
auto [db_num, field_path] = result.value();

// Build topic (protocol-agnostic - works for MQTT, HTTP, etc.)
std::string topic = PathUtils::buildTopic(1, "speed"); // "DB1/speed"
std::string topic2 = PathUtils::buildTopic("ReactorCore", "speed"); // "ReactorCore/speed"
```

### 3. SecurityHelper.hpp
Static helper functions for common security operations across all adapters.

**Key Functions:**
- `authorizeRead()` - Authorize a read operation
- `authorizeWrite()` - Authorize a write operation
- `authorizeConnection()` - Authorize connection (protocol-specific)

**Usage:**
```cpp
// Before (verbose):
if (!security_manager_->authorizeField(
        security::Protocol::HTTP, client_ip, db_num, field_path,
        false, origin, headers)) {
    return 403;
}

// After (concise):
if (!SecurityHelper::authorizeRead(
        *security_manager_, security::Protocol::HTTP,
        client_ip, db_num, field_path, origin, headers)) {
    return 403;
}
```

### 4. SchemaResolver.hpp
Static utility class for resolving paths to schema information.

**Key Functions:**
- `resolve()` - Resolve path to full Resolution struct
- `resolveDb()` - Resolve path to DB number only
- `resolveField()` - Resolve path to field path only

**Usage:**
```cpp
// Resolve full path
auto resolution = SchemaResolver::resolve("ReactorCore/speed", store);

// Get just DB number (with error handling)
auto db_result = SchemaResolver::resolveDb("ReactorCore/speed", store);
if (db_result.hasError()) {
    return db_result.error(); // "DB not found in path: ReactorCore/speed"
}
uint16_t db_num = db_result.value();

// Get just field path (with error handling)
auto field_result = SchemaResolver::resolveField("ReactorCore/speed", store);
if (field_result.hasError()) {
    return field_result.error();
}
std::string field_path = field_result.value();
```

### 5. EventFilter.hpp
Inline functions for filtering telemetry events based on subscriptions.

**Key Functions:**
- `shouldSend()` - Check if event should be sent to client
- `needsFieldFiltering()` - Check if field-level filtering is needed

**Usage:**
```cpp
// Check if client should receive this event
auto send_result = event_filter::shouldSend(dirty_paths, client_subscriptions);
if (send_result.hasError()) {
    // Handle error
}
bool send = send_result.value();

// Check if filtering is needed
auto filter_result = event_filter::needsFieldFiltering(dirty_paths, client_subscriptions);
bool filter = filter_result.value();
```

### 6. JsonHelper.hpp
Inline functions for JSON parsing, extraction, and filtering.

**Key Functions:**
- `parse()` - Parse JSON string to Document
- `extractField()` - Extract field value from JSON
- `filterFields()` - Filter JSON to specific fields
- `serializeValue()` - Serialize key-value pair
- `serializeObject()` - Serialize object from pairs

**Usage:**
```cpp
// Parse JSON
auto doc = json_helper::parse(json_string);

// Extract field
std::string value = json_helper::extractField(doc, "ReactorCore.speed");

// Filter fields
std::string filtered = json_helper::filterFields(doc, {"ReactorCore.speed", "Pump1.status"});

// Serialize
std::string json = json_helper::serializeValue("temperature", "25.5");
```

## Migration Guide

### Before (ModbusAdapter):
```cpp
class ModbusAdapter {
    twin::PlcMemory& memory_;
    std::shared_ptr<SecurityManager> security_manager_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    
    sgrn::Result<void> start(...) {
        // 50 lines of boilerplate
    }
    void stop() {
        // 10 lines of boilerplate
    }
};
```

### After (ModbusAdapter):
```cpp
class ModbusAdapter : public AdapterBase<ModbusAdapter> {
    using AdapterBase::AdapterBase; // Inherit constructor
    
    sgrn::Result<void> start(...) {
        // Pre-start setup
        return AdapterBase::start(ip, port);
    }
    
private:
    bool configure(...) { /* setup */ return true; }
    void serveLoop() { /* actual work */ }
};
```

## Benefits

1. **Reduced Code Duplication**: Common patterns extracted to single location
2. **Consistency**: All adapters use same patterns
3. **Maintainability**: Bug fixes in one place
4. **Type Safety**: Compile-time checks via CRTP
5. **Zero Overhead**: No virtual functions, all inlined
6. **Easy Testing**: Utilities can be tested independently

## Adding a New Adapter

When creating a new adapter (e.g., MQTT):

1. Inherit from `AdapterBase<MqttAdapter>`
2. Use `PathUtils` for topic/path conversion
3. Use `SecurityHelper` for authorization
4. Use `SchemaResolver` for schema lookups
5. Use `EventFilter` for subscription filtering
6. Use `JsonHelper` for JSON operations

**Result:** ~200 lines instead of ~600 lines for a new adapter.

## Compilation

All utilities are header-only and inline. No additional compilation units needed. Just include the headers you need:

```cpp
#include <sgrn/gateway/common/AdapterBase.hpp>
#include <sgrn/gateway/common/PathUtils.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
// ... etc
