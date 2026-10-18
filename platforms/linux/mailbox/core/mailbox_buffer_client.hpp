#pragma once
#include "mailbox_store.hpp"
namespace rimes::mailbox {
// An explicit receipt command carries identities only. Fcitx loads the trusted
// source and checks its revision again; no renderer-supplied body is accepted.
std::string ReceiveInBuffer(const Json& thread, const std::string& message,
                            const std::function<bool()>& current);
}
