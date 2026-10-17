#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rimes::capsule {
// Native notes allow 8 MiB documents. JSON quoting and bounded card/detail
// projections fit in this frame without truncating copied Unicode text.
inline constexpr std::uint32_t kMaxFrameBytes = 16 * 1024 * 1024;
bool EncodeFrame(std::string_view payload, std::string* frame);
bool DecodeFrameHeader(const char header[4], std::uint32_t* length);

// One in-flight frame and the latest complete snapshot, shared across clients.
// A slow UI never blocks the input-method thread or accumulates every update.
class SnapshotWriter {
  public:
    bool Queue(std::shared_ptr<const std::string> frame, bool revoke);
    bool Flush(int fd);
    bool Pending() const { return bool(current_); }

  private:
    std::shared_ptr<const std::string> current_, next_;
    std::size_t sent_ = 0;
};
} // namespace rimes::capsule
