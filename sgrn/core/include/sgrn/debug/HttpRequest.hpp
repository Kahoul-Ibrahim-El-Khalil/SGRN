#pragma once
#include <drogon/HttpRequest.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <algorithm> // for std::min

inline void printHttpRequest(drogon::HttpRequestPtr tsp_req) {
    if (!t_req) {
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "[DEBUG] HttpRequest is null!\n");
        fflush(stdout);
        return;
    }

    fmt::print("\n");
    fmt::print(fg(fmt::color::cyan) | fmt::emphasis::bold, "=====================================================\n");
    fmt::print(fg(fmt::color::cyan) | fmt::emphasis::bold, "               [DEBUG] HTTP REQUEST\n");
    fmt::print(fg(fmt::color::cyan) | fmt::emphasis::bold, "=====================================================\n");

    // --- Basic Info ---
    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Method:        ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->methodString());

    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Path:          ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->path());

    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Query:         ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->query());

    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Peer Address:  ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->peerAddr().toIpPort());

    fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Local Address: ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->localAddr().toIpPort());

    // --- Headers ---
    fmt::print("\n");
    fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "---------------- HEADERS ----------------\n");
    const auto& headers = t_req->headers();
    if (headers.empty()) {
        fmt::print(fg(fmt::color::gray), "(none)\n");
    } else {
        for (const auto& h : headers) {
            fmt::print(fg(fmt::color::light_blue), "{:<20}", h.first);
            fmt::print(": ");
            fmt::print(fg(fmt::color::white), "{}\n", h.second);
        }
    }

    // --- Cookies ---
    fmt::print("\n");
    fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "---------------- COOKIES ----------------\n");
    const auto& cookies = t_req->cookies();
    if (cookies.empty()) {
        fmt::print(fg(fmt::color::gray), "(none)\n");
    } else {
        for (const auto& c : cookies) {
            fmt::print(fg(fmt::color::light_blue), "{:<20}", c.first);
            fmt::print(" = ");
            fmt::print(fg(fmt::color::white), "{}\n", c.second);
        }
    }

    // --- Query Parameters ---
    fmt::print("\n");
    fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "------------- QUERY PARAMETERS -----------\n");
    const auto& params = t_req->parameters();
    if (params.empty()) {
        fmt::print(fg(fmt::color::gray), "(none)\n");
    } else {
        for (const auto& p : params) {
            fmt::print(fg(fmt::color::light_blue), "{:<20}", p.first);
            fmt::print(" = ");
            fmt::print(fg(fmt::color::white), "{}\n", p.second);
        }
    }

    // --- Body ---
    fmt::print("\n");
    fmt::print(fg(fmt::color::magenta) | fmt::emphasis::bold, "---------------- BODY -------------------\n");
    const std::string_view body = t_req->body();

    fmt::print(fg(fmt::color::yellow), "Content-Length: ");
    fmt::print(fg(fmt::color::white), "{} bytes\n", body.size());

    fmt::print(fg(fmt::color::yellow), "Content-Type:   ");
    fmt::print(fg(fmt::color::white), "{}\n", t_req->getHeader("content-type"));

    if (body.empty()) {
        fmt::print(fg(fmt::color::gray), "(empty body)\n");
    } else {
        size_t preview_size = std::min<size_t>(body.size(), 500);
        fmt::print("\n");
        fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold, "Raw Body Preview ({} chars):\n", preview_size);
        fmt::print(fg(fmt::color::white), "{}\n", body.substr(0, preview_size));
        if (body.size() > preview_size) {
            fmt::print(fg(fmt::color::gray), "... (truncated, total {} bytes)\n", body.size());
        }
    }

    // --- JSON Attempt ---
    fmt::print("\n");
    fmt::print(fg(fmt::color::magenta) | fmt::emphasis::bold, "-------------- JSON BODY ----------------\n");
    try {
        auto json_obj = t_req->getJsonObject();
        if (json_obj) {
            fmt::print(fg(fmt::color::white), "{}\n", json_obj->toStyledString());
        } else {
            fmt::print(fg(fmt::color::gray), "(not valid JSON)\n");
        }
    } catch (...) {
        fmt::print(fg(fmt::color::red), "(JSON parse exception)\n");
    }

    fmt::print(fg(fmt::color::cyan) | fmt::emphasis::bold, "=====================================================\n\n");
    fflush(stdout); // Ensure output is immediately visible
}

#ifdef DEBUG_HTTP_REQUESTS
#define DEBUG_HTTP_REQUEST(t_req) printHttpRequest(t_req)
#else
#define DEBUG_HTTP_REQUEST(t_req) (void)(0)
#endif
