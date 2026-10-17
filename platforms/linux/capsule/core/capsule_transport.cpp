#include "capsule_transport.hpp"
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
namespace rimes::capsule {
bool EncodeFrame(std::string_view payload, std::string* frame) {
    if (!frame || payload.empty() || payload.size() > kMaxFrameBytes)
        return false;
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame->resize(4);
    for (int i = 0; i < 4; ++i)
        (*frame)[i] = static_cast<char>(size >> (24 - i * 8));
    frame->append(payload);
    return true;
}
bool DecodeFrameHeader(const char header[4], std::uint32_t* length) {
    if (!length)
        return false;
    *length = 0;
    for (int i = 0; i < 4; ++i)
        *length = (*length << 8) | static_cast<unsigned char>(header[i]);
    return *length > 0 && *length <= kMaxFrameBytes;
}
bool SnapshotWriter::Queue(std::shared_ptr<const std::string> frame, bool revoke) {
    // A partially transmitted frame cannot be replaced. Disconnect it on
    // revocation so the receiver discards it without applying stale copy intent.
    if (revoke && sent_) {
        current_.reset();
        next_.reset();
        sent_ = 0;
        return false;
    }
    if (!sent_) {
        current_ = std::move(frame);
        next_.reset();
    } else
        next_ = std::move(frame);
    return true;
}
bool SnapshotWriter::Flush(int fd) {
    // Bound each writable event so a client cannot starve other sockets.
    std::size_t budget = 256 * 1024;
    while (current_ && budget) {
        const auto count =
            send(fd, current_->data() + sent_, std::min(budget, current_->size() - sent_),
                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        if (count <= 0)
            return false;
        sent_ += static_cast<std::size_t>(count);
        budget -= static_cast<std::size_t>(count);
        if (sent_ == current_->size()) {
            current_ = std::move(next_);
            sent_ = 0;
        }
    }
    return true;
}
} // namespace rimes::capsule
