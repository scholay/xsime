#pragma once
#include "capsule_protocol.hpp"
#include <functional>
#include <memory>
namespace rimes::capsule {
class ClipboardController {
  public:
    using Send = std::function<void(std::string)>;
    using Status = std::function<void(std::string)>;
    ClipboardController(Send send, Status status);
    ~ClipboardController();
    void Apply(const Snapshot& snapshot);
    void CopyText(const std::string& text);
    void CopyResource(const Record& record);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
