#pragma once
#include "gateway_protocol.hpp"
#include <memory>
namespace rimes::mailbox {
// Owned by Fcitx, independent of the Mailbox window. Every control setter is
// memory-only; TCP parsing and persistence run on the dedicated worker.
class GatewayServer {
  public:
    explicit GatewayServer(std::filesystem::path user = runtime::UserRoot());
    ~GatewayServer();
    void Configure(const Json& configuration);
    void SetModuleEnabled(bool enabled);
    void SetSessionProtected(bool protected_session);
    void SetInputProtected(bool protected_input);
    Json Status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::mailbox
