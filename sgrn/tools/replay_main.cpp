#include <sgrn/gateway/tools/sgrn_replay.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <csignal>
#include <cxxopts.hpp>
#include <iostream>

static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    g_shutdown_requested.store(true);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    cxxopts::Options options("sgrn_replay", "SGRN Gateway History Archive Replayer & Sync Gateway");

    options.add_options()("c,config", "Path to gateway.json (optional — omit for headless HTTP+WebSocket mode)",
        cxxopts::value<std::string>())("a,archive", "Path to history archive (.bin.zst or .jsonl.zst)", cxxopts::value<std::string>())(
        "s,schema", "Path to SCL schema file (.scl) or symbols directory", cxxopts::value<std::string>())("r,speed",
        "Replay speed multiplier (e.g. 1.0, 2.0, 0.5)", cxxopts::value<double>()->default_value("1.0"))("l,loop", "Loop replay infinitely",
        cxxopts::value<bool>()->default_value("false"))("n,no-delay", "Replay as fast as possible without timestamp delays",
        cxxopts::value<bool>()->default_value("false"))("g,gui", "Open the embedded dashboard in a browser once replay starts",
        cxxopts::value<bool>()->default_value("false"))("http-port", "HTTP dashboard port in headless mode (default: 8080)",
        cxxopts::value<uint16_t>()->default_value("8080"))("ws-port", "WebSocket port in headless mode (default: 8081)",
        cxxopts::value<uint16_t>()->default_value("8081"))("man", "Display detailed manual page")("h,help", "Print usage help");

    options.parse_positional({"archive"});

    auto result = options.parse(argc, argv);

    if (result.count("man")) {
        std::cout << R"(SGRN_REPLAY(1)                  User Commands                  SGRN_REPLAY(1)

NAME
       sgrn_replay - SGRN Gateway History Archive Replayer & Sync Tool

SYNOPSIS
       # Full mode (gateway.json required):
       sgrn_replay -c gateway.json -a ARCHIVE.bin.zst [-s SCHEMA.scl] [-r SPEED] [--loop]

       # Headless mode (no gateway.json — HTTP+WebSocket only, auto-opens GUI):
       sgrn_replay -a ARCHIVE.bin.zst -s SCHEMA.scl [--http-port 8080] [--ws-port 8081]

DESCRIPTION
       sgrn_replay initializes SGRN gateway Northbound protocol interfaces and
       replays historical state frames from a .bin.zst or .jsonl.zst archive
       into the digital twin memory in real-time or scaled speed.

       When -c/--config is omitted the tool runs in HEADLESS mode: only HTTP
       and WebSocket adapters are started (no S7/OPC-UA/Modbus/EIP, no
       persistence, no cloud upload). The browser dashboard is opened
       automatically. This is the zero-config path for development and
       simulation replay.

OPTIONS
       -c, --config FILE.json
              Path to gateway.json. Optional — omit for headless mode.

       -a, --archive FILE.bin.zst|FILE.jsonl.zst
              Path to input history telemetry archive.

       -s, --schema FILE.scl|DIR
              SCL schema file or symbols directory.
              Required in headless mode; optional override in full mode.

       -r, --speed SPEED
              Replay speed multiplier (default: 1.0).

       -l, --loop
              Loop replay infinitely until terminated.

       -n, --no-delay
              Disable real-time pacing and replay as fast as possible.

       -g, --gui
              Open the embedded HTTP dashboard in a browser window.
              Always true in headless mode.

       --http-port PORT
              HTTP port in headless mode (default: 8080).

       --ws-port PORT
              WebSocket port in headless mode (default: 8081).
)" << std::endl;
        return 0;
    }

    if (result.count("help") || !result.count("archive")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    const bool has_config = result.count("config") > 0;

    sgrn::gateway::ReplayConfig config;
    config.archive_path = result["archive"].as<std::string>();
    config.replay_speed = result["speed"].as<double>();
    config.loop = result["loop"].as<bool>();
    config.no_delay = result["no-delay"].as<bool>();

    if (result.count("schema")) {
        config.scl_schema_path = result["schema"].as<std::string>();
    }

    if (has_config) {
        config.gateway_config_path = result["config"].as<std::string>();
        config.headless_mode = false;
    } else {
        // Headless: HTTP + WebSocket only — no gateway.json required.
        config.headless_mode = true;
        config.headless_http_port = result["http-port"].as<uint16_t>();
        config.headless_ws_port = result["ws-port"].as<uint16_t>();
    }

    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    fmt::print(fg(fmt::color::cyan), " SGRN Gateway History Replayer & Sync Engine\n");
    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    if (has_config) {
        fmt::print("Mode:           Full gateway ({})\n", config.gateway_config_path);
    } else {
        fmt::print(fg(fmt::color::yellow), "Mode:           Headless (HTTP :{}, WebSocket :{})\n", config.headless_http_port,
            config.headless_ws_port);
    }
    fmt::print("Archive Path:   {}\n", config.archive_path);
    if (!config.scl_schema_path.empty())
        fmt::print("Schema:         {}\n", config.scl_schema_path);
    fmt::print("Replay Speed:   {}x\n", config.replay_speed);
    fmt::print("Looping:        {}\n", config.loop ? "Enabled" : "Disabled");
    fmt::print("Realtime Delay: {}\n", config.no_delay ? "Disabled (max speed)" : "Enabled");

    sgrn::gateway::GatewayReplayer replayer(config);
    if (auto r = replayer.initialize(); r.hasError()) {
        fmt::print(fg(fmt::color::red), "Failed to initialize gateway replayer.\n");
        fmt::print("{}", r.error());
        return 1;
    }

    fmt::print(fg(fmt::color::green), "[sgrn_replay] Gateway protocols & twin initialized successfully.\n");

    replayer.start();

    // Always open the GUI in headless mode; also honour an explicit --gui flag.
    const bool open_gui = result["gui"].as<bool>() || config.headless_mode;
    if (open_gui) {
        const uint16_t http_port = replayer.httpPort();
        if (http_port == 0) {
            fmt::print(fg(fmt::color::yellow), "[sgrn_replay] GUI requested but no HTTP adapter is configured.\n");
        } else {
            const std::string url = fmt::format("http://127.0.0.1:{}/", http_port);
            fmt::print(fg(fmt::color::cyan), "[sgrn_replay] Opening dashboard at {}\n", url);
#if defined(_WIN32)
            ::system(fmt::format("start msedge --app=\"{}\" || start chrome --app=\"{}\" || start \"\" \"{}\"", url, url, url).c_str());
#elif defined(__APPLE__)
            ::system(fmt::format("open -n -a \"Google Chrome\" --args --app=\"{}\" 2>/dev/null || open \"{}\"", url, url).c_str());
#else
            const char* display = ::getenv("DISPLAY");
            const std::string env = display ? fmt::format("DISPLAY=\"{}\" ", display) : "";
            ::system(fmt::format("(which chromium > /dev/null 2>&1 && {0}chromium --app=\"{1}\" > /dev/null 2>&1 &) || "
                                 "(which google-chrome > /dev/null 2>&1 && {0}google-chrome --app=\"{1}\" > /dev/null 2>&1 &) || "
                                 "{0}xdg-open \"{1}\" > /dev/null 2>&1 &",
                env, url)
                    .c_str());
#endif
        }
    }

    while (replayer.isRunning() && !g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    replayer.stop();
    fmt::print(fg(fmt::color::green), "[sgrn_replay] Shutdown complete.\n");
    return 0;
}
