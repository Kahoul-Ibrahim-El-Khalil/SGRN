// s7shell_api.as — Built-in S7Shell API declarations for IDE / linter completion.
// Tooling-only: DO NOT load into AngelScript engine at runtime.

class PlcRuntime {
    PlcRuntime();
    PlcRuntime(const string &in schemaPath);
    void loadSclSchema(const string &in path);
    void loadJsonSchema(const string &in path);
    bool set(uint16 db, const string &in path, const string &in value);
    string get(uint16 db, const string &in path);
    void sync();
    void syncDb(uint16 db);
    string dirtySnapshot() const;
    bool hasDirty(uint16 db) const;
    bool defineTag(const string &in name, const string &in type, const string &in addr);
    string tagList() const;
    string tagList(const string &in table) const;
    string tagTables() const;
    string tagInfo(const string &in name) const;
    bool hasDirtyTags() const;
}

class S7Client {
    S7Client(const string &in ip, int rack = 0, int slot = 1, uint16 port = 102);
    S7Client(const string &in ip, int rack, int slot, uint16 port, PlcRuntime@ rt);
    bool isConnected() const;
    bool ping();
    void disconnect();
    bool reconnect();
    string lastError() const;
    int lastErrorCode() const;
    bool lastOpOk() const;
    void clearLastError();
    string read(const string &in address);
    bool write(const string &in address, const string &in hex);
}

class S7Server {
    S7Server(PlcRuntime@ rt, const string &in bindIp = "0.0.0.0", uint16 port = 102);
    void start();
    void stop();
    bool isRunning() const;
    int clientsCount() const;
    int getCpuStatus() const;
}

class Gateway {
    Gateway(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 8080);
    void stop();
    bool isRunning() const;
    void broadcast(const string &in json);
    void broadcast();
    void setAutoBroadcast(bool enable);
    bool autoBroadcast() const;
    void loadPolicy(const string &in path);
}

class HttpServer {
    HttpServer(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 8080);
    void stop();
    bool isRunning() const;
    void loadPolicy(const string &in path);
}

class WebSocketServer {
    WebSocketServer(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 9001);
    void stop();
    bool isRunning() const;
    void broadcast(const string &in json);
    void broadcast();
    void setAutoBroadcast(bool enable);
    bool autoBroadcast() const;
    void loadPolicy(const string &in path);
}

class Persistence {
    Persistence(PlcRuntime@ rt);
    Persistence(PlcRuntime@ rt, const string &in outDir);
    void configure(const string &in outDir);
    void configure(const string &in outDir, const string &in format, const string &in mode);
    void start();
    void flush();
    void stop();
    bool isActive() const;
    string outDir() const;
}

class SimParams {
    uint64 seed;
    uint timestep_ms;
    uint duration_s;
    double noise_level;
    string fault;
}

class SimEngine {
    SimEngine(PlcRuntime@ rt, SimParams@ params);
    void run();
    void step();
}

class WalReplayer {
    WalReplayer(const string &in archivePath);
    void speed(double s);
    bool run();
}

class S7ProxySession {
    S7ProxySession(S7Client@ src, S7Client@ dst);
    void addMapping(uint16 srcDb, uint16 dstDb, uint intervalMs = 100, uint size = 0);
    void start();
    void stop();
}

class GatewaySync {
    GatewaySync(PlcRuntime@ rt);
    void subscribeDb(uint16 db);
    void unsubscribeDb(uint16 db);
    void publishOnDirty(bool enable);
    void useBinary(bool enable);
    bool connect(const string &in url);
    void disconnect();
    bool connected() const;
    string lastError() const;
}

void print(const string &in str);
void sleep(uint ms);
