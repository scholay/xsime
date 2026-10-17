#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rimes::capsule {
struct MimeData {
    std::string type, bytes;
    int format = 8; // X11 selection format; Wayland offers byte streams.
    bool operator==(const MimeData& other) const {
        return type == other.type && bytes == other.bytes && format == other.format;
    }
};
struct ClipboardArchive {
    std::vector<MimeData> representations;
    // Runtime ownership only; never serialized into clipboard history. Keeps
    // copied capture files alive until the desktop relinquishes the offer.
    std::shared_ptr<void> resource_lease = {};
    void Validate() const;
    std::string Encode() const;
    static ClipboardArchive Decode(const std::string& bytes);
    static bool AllowsTypes(const std::vector<std::string>& types);
    static constexpr std::size_t maxTypeBytes = 128ull * 1024 * 1024;
    static constexpr std::size_t maxTotalBytes = 192ull * 1024 * 1024;
    static constexpr std::size_t maxArchiveBytes = 256ull * 1024 * 1024;
};
struct ClipboardProjection {
    std::string kind, title, preview, text, search, filter = "other";
    bool insertable = false;
};
ClipboardProjection ProjectClipboard(const ClipboardArchive& archive);

// The counter/offer identity is metadata, never a request for its contents.
// Resuming consumes a baseline only. Async completions carry this generation.
class ClipboardCaptureGate {
  public:
    void Allow(bool allowed);
    std::optional<std::uint64_t> Observe(std::uint64_t identity);
    bool Current(std::uint64_t generation) const;
    void Baseline(std::uint64_t identity);
    void Cancel();

  private:
    bool allowed_ = false, baseline_ = true;
    std::uint64_t generation_ = 0, observed_ = 0;
};

struct ClipboardSource {
    std::string backend, application, identity;
};
struct ClipboardEntry {
    std::string id, revision, kind, title, preview, filter;
    ClipboardSource source;
    std::int64_t captured = 0, bytes = 0;
    bool favorite = false, insertable = false;
};
struct ClipboardPage {
    std::vector<ClipboardEntry> entries;
    int count = 0, offset = 0, selected = 0;
};
struct ClipboardLimits {
    std::size_t items = 100000;
    std::int64_t bytes = 64ll * 1024 * 1024 * 1024;
};

// Metadata queries never load opaque MIME payloads. All transactions and
// payload reads run on background workers, never the Fcitx or GTK event loop.
class ClipboardHistoryStore {
  public:
    explicit ClipboardHistoryStore(const std::filesystem::path& root, ClipboardLimits limits = {});
    ~ClipboardHistoryStore();
    ClipboardHistoryStore(const ClipboardHistoryStore&) = delete;
    ClipboardHistoryStore& operator=(const ClipboardHistoryStore&) = delete;
    static std::filesystem::path DefaultRoot();
    ClipboardEntry Capture(const ClipboardArchive& archive, const ClipboardSource& source,
                           const std::function<bool()>& current = {});
    ClipboardPage Page(std::string query, std::string filter, int selected = 0) const;
    std::vector<ClipboardEntry> List(std::string query = {}, std::size_t limit = 200,
                                     std::size_t offset = 0, std::string filter = "all") const;
    std::optional<ClipboardEntry> Get(const std::string& id) const;
    std::optional<ClipboardArchive> Read(const std::string& id, const std::string& revision) const;
    bool Favorite(const std::string& id, const std::string& revision, bool value);
    bool Remove(const std::string& id, const std::string& revision);
    void ClearTemporary();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rimes::capsule
