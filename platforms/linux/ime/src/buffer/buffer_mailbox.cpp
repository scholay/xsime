#include "background_jobs.hpp"
#include "buffer_service.hpp"
#include "mailbox_store.hpp"
#include "official_catalog.hpp"
#include <condition_variable>
#include <fcitx-utils/event.h>
#include <poll.h>
#include <sys/socket.h>

namespace fcitx {
namespace {
using rimes::mailbox::Json;
using rimes::mailbox::Store;
struct Receipt {
    rimes::buffer::Command command;
    rimes::plugins::Lease lease;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> delivered{false};
    std::optional<rimes::buffer::BufferModel> candidate;
};
// Only the worker waits. The event loop runs one short memory-only operation.
// Timeout and completion share the mutex, preventing a late callback from
// staging content after the worker has already rolled its receipt back.
bool OnMain(const BufferService::PostFn& post, const std::shared_ptr<std::atomic<bool>>& life,
            const std::shared_ptr<Receipt>& receipt, const std::function<bool()>& action) {
    struct Await {
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false, expired = false, ok = false;
    };
    const auto state = std::make_shared<Await>();
    post([life, receipt, state, action] {
        std::lock_guard lock(state->mutex);
        if (!state->expired && life->load() && !receipt->cancelled->load()) {
            try {
                state->ok = action();
            } catch (...) {
            }
        }
        state->done = true;
        state->ready.notify_all();
    });
    std::unique_lock lock(state->mutex);
    if (!state->ready.wait_for(lock, std::chrono::seconds(2), [&] { return state->done; }))
        state->expired = true;
    return !state->expired && state->ok;
}
} // namespace
struct BufferService::Inbox {
    std::string session = rimes::runtime::RandomID();
    std::uint64_t epoch = 0;
    rimes::runtime::BackgroundJobs jobs;
    std::shared_ptr<Receipt> active;
    std::unique_ptr<rimes::runtime::FileLock> owner;
    std::vector<std::pair<rimes::buffer::Command, bool>> cleanup;
    std::unique_ptr<EventSourceTime> retry;
    bool cleanup_running = false;
    std::atomic<bool> ready{false};
    bool preparing = false;
    std::unique_ptr<EventSourceTime> startup_retry;
};
void BufferService::InitializeInbox() {
    inbox_ = std::make_shared<Inbox>();
    PrepareMailbox();
}
void BufferService::PrepareMailbox() {
    const auto state = inbox_;
    const auto life = alive_;
    if (!state || state->ready.load() || state->preparing)
        return;
    state->preparing = true;
    if (!state->jobs.Run(std::make_shared<std::atomic<bool>>(false), [this, state, life] {
            try {
                const auto root = rimes::runtime::UserRoot() / "mailbox";
                auto owner = rimes::runtime::PrivateDirectory(root / "transfer-host").Lock(false);
                Store(root).RecoverBufferTransfers();
                state->owner = std::make_unique<rimes::runtime::FileLock>(std::move(owner));
                state->ready.store(true);
            } catch (const std::exception&) {
            }
            post_([this, state, life] {
                if (!life->load())
                    return;
                state->preparing = false;
                Publish();
                if (!state->ready.load() && instance_)
                    state->startup_retry = instance_->eventLoop().addTimeEvent(
                        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000000, 0,
                        [this, life](EventSourceTime* timer, std::uint64_t) {
                            timer->setEnabled(false);
                            if (life->load())
                                PrepareMailbox();
                            return true;
                        });
            });
        }))
        state->preparing = false;
}
void BufferService::StopInbox() {
    if (!inbox_)
        return;
    if (inbox_->active)
        inbox_->active->cancelled->store(true);
    inbox_->retry.reset();
    inbox_->startup_retry.reset();
    inbox_->jobs.Shutdown();
}
std::string BufferService::MailboxScope() const {
    return inbox_ && inbox_->ready.load() ? inbox_->session + ":" + std::to_string(inbox_->epoch)
                                          : "";
}
void BufferService::InvalidateMailboxFocus() {
    if (!inbox_)
        return;
    ++inbox_->epoch;
    if (inbox_->active && !inbox_->active->delivered.load())
        inbox_->active->cancelled->store(true);
}
void BufferService::CancelMailboxPeer(int peer, std::uint64_t generation) {
    if (inbox_ && inbox_->active && inbox_->active->command.peer == peer &&
        inbox_->active->command.peer_generation == generation && !inbox_->active->delivered.load())
        inbox_->active->cancelled->store(true);
}
bool BufferService::MailboxPeerAlive(const rimes::buffer::Command& request) {
    std::lock_guard<std::recursive_mutex> lock(clients_mu_);
    for (const auto& client : clients_)
        if (client.fd == request.peer && client.generation == request.peer_generation) {
            pollfd state{client.fd, POLLIN, 0};
            if (poll(&state, 1, 0) < 0 || (state.revents & (POLLHUP | POLLERR | POLLNVAL)))
                return false;
            char byte;
            const auto size = recv(client.fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            return size > 0 || (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        }
    return false;
}
void BufferService::SynchronizeMailbox() {
    if (!inbox_ || !inbox_->active || inbox_->active->delivered.load())
        return;
    const auto& current = *inbox_->active;
    if (current.command.scope != MailboxScope() ||
        current.command.generation != model_.generation() || model_.secure() ||
        model_.password_field() || !plugins_ || !plugins_->Accepts(current.lease))
        current.cancelled->store(true);
}
void BufferService::ReceiveMailbox(const rimes::buffer::Command& command) {
    auto result = [this, id = command.id](bool ok, const std::string& message) {
        WriteAll(
            Json{{"v", 1}, {"type", "mailbox_result"}, {"id", id}, {"ok", ok}, {"message", message}}
                .dump());
    };
    if (!inbox_ || !inbox_->ready.load() || inbox_->active || inbox_->cleanup_running ||
        !plugins_ || model_.secure() || model_.password_field() ||
        command.scope != MailboxScope() || command.generation != model_.generation()) {
        result(false, "输入状态已变化，请重新接收");
        return;
    }
    const auto lease = plugins_->Authorize("builtin.mailbox." + command.module, "mailbox.navigate");
    if (!lease) {
        result(false, "Mailbox 插件已停用");
        return;
    }
    auto receipt = std::make_shared<Receipt>();
    receipt->command = command;
    receipt->lease = *lease;
    inbox_->active = receipt;
    const auto state = inbox_;
    const auto life = alive_;
    auto current = [this, receipt] {
        return inbox_->active == receipt && MailboxPeerAlive(receipt->command) &&
               !receipt->cancelled->load() && receipt->command.scope == MailboxScope() &&
               receipt->command.generation == model_.generation() && !model_.secure() &&
               !model_.password_field() && plugins_ && plugins_->Accepts(receipt->lease);
    };
    if (!state->jobs.Run(std::make_shared<std::atomic<bool>>(false), [this, state, life, receipt,
                                                                      current, result] {
            bool began = false, delivered = false;
            std::string failure = "接收未完成，原文已保留；请刷新后重试";
            const auto root = rimes::runtime::UserRoot() / "mailbox";
            try {
                // Finish a previously interrupted local cleanup before another receipt.
                for (const auto& [previous, accepted] : state->cleanup)
                    Store(root).FinishBufferTransfer(previous.thread, previous.id, accepted);
                state->cleanup.clear();
                rimes::plugins::PackageStore packages(
                    rimes::runtime::UserRoot() / "plugins", rimes::plugins::OfficialCatalog(),
                    rimes::plugins::BundledPackages(), rimes::plugins::CompiledAdapters(),
                    RIMES_PRODUCT_VERSION, false);
                const auto grant = packages.Inspect(receipt->lease.plugin_id);
                packages.WithEnabledGrant(
                    grant.id, grant.grant,
                    [&] {
                        const auto& request = receipt->command;
                        Store store(root, {}, [life, receipt](const auto& write) {
                            if (life->load() && !receipt->cancelled->load())
                                write();
                        });
                        store.WithMessage(
                            request.thread, request.revision, request.message,
                            [&](const Json& thread, const Json& message,
                                const rimes::mailbox::Commit& gate) {
                                if (rimes::mailbox::Module(thread) != request.module ||
                                    (message.contains("imageFileName") &&
                                     !message.at("imageFileName").is_null()))
                                    throw std::runtime_error("消息来源或格式已变化");
                                const auto text = message.at("body").get<std::string>();
                                gate([&] {
                                    if (!OnMain(post_, life, receipt,
                                                [this, receipt, current, text] {
                                                    if (!current())
                                                        return false;
                                                    auto candidate = model_;
                                                    if (!candidate.stage_mailbox(text))
                                                        return false;
                                                    receipt->candidate = std::move(candidate);
                                                    return true;
                                                }))
                                        throw std::runtime_error("Buffer 容量不足或输入状态已变化");
                                });
                            });
                        const auto accepted = store.BeginBufferTransfer(
                            request.thread, request.revision, request.message, request.id);
                        began = true;
                        store.WithMessage(
                            request.thread, rimes::mailbox::Revision(accepted), request.message,
                            [&](const Json&, const Json&, const rimes::mailbox::Commit& gate) {
                                gate([&] {
                                    delivered =
                                        OnMain(post_, life, receipt, [this, receipt, current] {
                                            if (!current() || !receipt->candidate)
                                                return false;
                                            model_ = std::move(*receipt->candidate);
                                            receipt->candidate.reset();
                                            receipt->delivered.store(true);
                                            // Detached staging never sends into Mailbox's composer
                                            // or reuses an earlier target. The user explicitly
                                            // opens Buffer on a target.
                                            target_token_.clear();
                                            model_.set_target_name({});
                                            model_.set_preedit({});
                                            raw_input_.clear();
                                            pending_unfocus_token_.clear();
                                            focus_suspended_ = true;
                                            gesture_.cancel();
                                            CancelHoldTimer();
                                            CancelFocusGraceTimer();
                                            CancelDragHardTimer();
                                            dragging_ = false;
                                            CancelDerived();
                                            auto_send_clock_.Reset();
                                            Publish();
                                            return true;
                                        });
                                });
                            });
                    },
                    false);
            } catch (const std::exception& error) {
                failure = error.what();
            }
            if (began) {
                try {
                    Store(root).FinishBufferTransfer(receipt->command.thread, receipt->command.id,
                                                     delivered);
                } catch (...) {
                    state->cleanup.emplace_back(receipt->command, delivered);
                }
            }
            post_([this, state, life, receipt, result, delivered, failure] {
                if (!life->load())
                    return;
                if (state->active == receipt)
                    state->active.reset();
                result(delivered, delivered ? "已加入 Buffer，打开 Buffer 后可继续编辑" : failure);
                Publish();
                if (!state->cleanup.empty() && instance_) {
                    state->retry = instance_->eventLoop().addTimeEvent(
                        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000000, 1000000,
                        [this, state, life](EventSourceTime* timer, std::uint64_t) {
                            timer->setEnabled(false);
                            if (state->active)
                                return true;
                            state->cleanup_running = true;
                            if (!state->jobs.Run(
                                    std::make_shared<std::atomic<bool>>(false),
                                    [this, state, life] {
                                        auto pending = std::move(state->cleanup);
                                        state->cleanup.clear();
                                        for (const auto& [request, accepted] : pending) {
                                            try {
                                                Store().FinishBufferTransfer(request.thread,
                                                                             request.id, accepted);
                                            } catch (...) {
                                                state->cleanup.emplace_back(request, accepted);
                                            }
                                        }
                                        post_([state, life] {
                                            if (!life->load())
                                                return;
                                            state->cleanup_running = false;
                                            if (!state->retry || state->cleanup.empty())
                                                return;
                                            state->retry->setNextInterval(1000000);
                                            state->retry->setEnabled(true);
                                        });
                                    })) {
                                state->cleanup_running = false;
                                timer->setNextInterval(1000000);
                                timer->setEnabled(true);
                            }
                            return true;
                        });
                }
            });
        })) {
        state->active.reset();
        result(false, "后台正忙，请稍后重试");
    }
}
} // namespace fcitx
