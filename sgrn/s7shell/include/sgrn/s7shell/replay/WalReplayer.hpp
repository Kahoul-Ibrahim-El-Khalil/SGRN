#pragma once
// =============================================================================
// WalReplayer.hpp — Rate-controlled replay of binary/JSONL WAL archives
// =============================================================================

#include <sgrn/s7shell/runtime/PlcRuntime.hpp>

#include <functional>
#include <memory>
#include <string>

namespace sgrn::s7shell::replay
{

using FrameCallback = std::function<void(uint16_t db, uint64_t timestamp)>;

class WalReplayer {
public:
    explicit WalReplayer(std::string archive_path, std::shared_ptr<runtime::PlcRuntime> runtime = nullptr);
    ~WalReplayer() = default;

    void speed(double factor) {
        speed_factor_ = factor;
    }
    double speed() const {
        return speed_factor_;
    }

    void onFrame(FrameCallback cb) {
        callback_ = std::move(cb);
    }

    /// Replay the archive file into the PlcRuntime state.
    bool run();

private:
    /// Frame-by-frame replay of a decompressed binary WAL buffer with
    /// real-time pacing. Returns false only on a fatal framing error.
    bool replayBinaryArchive(const std::string& t_decompressed, uint64_t& t_replayed_frames);
    /// Line-by-line replay of a decompressed JSONL WAL buffer with
    /// real-time pacing. Returns false only on a fatal framing error.
    bool replayJsonlArchive(const std::string& t_decompressed, uint64_t& t_replayed_frames);

    std::string archive_path_;
    std::shared_ptr<runtime::PlcRuntime> runtime_;
    double speed_factor_{1.0};
    FrameCallback callback_;
};

} // namespace sgrn::s7shell::replay
