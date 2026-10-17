#pragma once
#include "capsule_protocol.hpp"
#include <functional>
#include <gtk/gtk.h>
#include <memory>
namespace rimes::capsule {
class CaptureController {
  public:
    explicit CaptureController(std::function<void(std::string)> send);
    ~CaptureController();
    void Apply(const Snapshot& state);
    void Open();
    GtkWidget* Window() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
