#pragma once
#include "capsule_model.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
namespace rimes::capsule {
void EraseSecret(std::string& value);
// Synchronous final-publication gate. It is entered after acquiring the store
// lock, so waiting for another process must never block immediate UI conceal.
using SecretCommit = std::function<void(const std::function<void()>&)>;
void CommitSecret(const SecretCommit& gate, const std::function<void()>& action);
struct PasswordSummary {
    std::string id, title, summary, category = "other", updated_at, revision;
};
struct PasswordRecord : PasswordSummary {
    std::string body;
    ~PasswordRecord() { EraseSecret(body); }
};
// Wire-compatible with macOS v1/v2 encrypted Markdown + 32-byte master-key.
// The UI's reveal credential is separate from the local encryption key.
class PasswordStore {
  public:
    explicit PasswordStore(std::filesystem::path root);
    std::vector<PasswordSummary> List() const;
    // Worker-only: bounded, cancellable retry for a concurrent Capsule writer.
    std::vector<PasswordSummary> ListWhileActive(const std::atomic<bool>& cancelled) const;
    PasswordSummary Inspect(const std::string& id) const;
    void WithAuthorization(const std::string& id, const std::string& revision,
                           const std::string& credential,
                           const std::function<void()>& action) const;
    PasswordRecord Read(const std::string& id, const std::string& expected_revision,
                        const std::string& credential_revision = "") const;
    PasswordSummary Put(const PasswordRecord& request, const std::string& expected_revision,
                        const std::string& credential_revision = "", const SecretCommit& gate = {});
    void Remove(const std::string& id, const std::string& expected_revision,
                const std::string& credential_revision = "", const SecretCommit& gate = {});
    PasswordSummary MigrateNote(const std::string& id, const std::string& expected_revision,
                                const std::string& category,
                                const std::string& credential_revision = "",
                                const SecretCommit& gate = {});

  private:
    std::filesystem::path root_;
};
Card PasswordCard(const PasswordSummary& value);
struct TotpParameters {
    std::string secret;
    int digits = 6, period = 30;
    ~TotpParameters() { EraseSecret(secret); }
};
std::optional<TotpParameters> ParseTotp(std::string_view label, std::string_view value);
std::pair<std::string, int> TotpCode(const TotpParameters& parameters, std::int64_t unix_seconds);
using RevealCode = std::array<std::string, 4>;
RevealCode DefaultRevealCode();
std::optional<std::string> CanonicalChord(std::string_view keys);
// GDK hardware keycodes use the XKB evdev mapping, independent of text layout.
char RevealPhysicalLetter(unsigned hardware_keycode);
class RevealCredential {
  public:
    explicit RevealCredential(std::filesystem::path root);
    std::string Revision() const;
    bool Matches(const RevealCode& code) const;
    void Change(const RevealCode& current, const std::optional<RevealCode>& next,
                const std::string& expected_revision, const SecretCommit& gate = {});

  private:
    std::filesystem::path root_;
};
// Monotonic, non-persistent authority; identity, credential and focus changes revoke it.
class SecretAccess {
  public:
    using Time = std::chrono::steady_clock::time_point;
    void Authorize(std::string id, std::string record_revision, std::string credential_revision,
                   Time now);
    bool Valid(const std::string& id, const std::string& record_revision,
               const std::string& credential_revision, Time now) const;
    void Reveal(std::size_t row, Time now);
    bool Revealed(std::size_t row, Time now) const;
    void Conceal();

  private:
    std::string id_, record_revision_, credential_revision_;
    Time until_{}, reveal_until_{};
    std::optional<std::size_t> row_;
};
} // namespace rimes::capsule
