#pragma once
#include "capsule_model.hpp"
#include <atomic>
#include <filesystem>
#include <optional>
namespace rimes::capsule {
struct CaptureRecord {
    std::string id, title, kind = "image", source, original, output, revision, sha256;
    std::string collection_id;
    double created = 0, duration = 0;
    int width = 0, height = 0;
    std::uint64_t bytes = 0;
    bool incomplete = false;
};
struct CaptureAsset {
    CaptureRecord record;
    std::filesystem::path path;
    std::shared_ptr<void> lease;
};
std::string CaptureDuration(double seconds);
class CaptureStore {
  public:
    explicit CaptureStore(std::filesystem::path root);
    static std::filesystem::path DefaultRoot();
    std::vector<CaptureRecord> List() const;
    std::optional<CaptureRecord> Get(const std::string& id, const std::string& revision = "") const;
    std::optional<CaptureAsset> Acquire(const std::string& id, const std::string& revision) const;
    CaptureRecord Import(const std::filesystem::path& path, CaptureRecord metadata,
                         const std::atomic<bool>& cancelled,
                         const std::function<bool(const std::function<void()>&)>& publish = {});
    CaptureRecord Collect(const std::string& id, const std::string& revision);
    bool Remove(const std::string& id, const std::string& revision);
    std::size_t Prune(double now, double retention = 30 * 24 * 3600,
                      std::uint64_t capacity = 10ULL * 1024 * 1024 * 1024);
    std::filesystem::path File(const CaptureRecord& record) const;
    const std::filesystem::path& root() const { return root_; }
    static constexpr std::uint64_t kMaxFileBytes = 1024ULL * 1024 * 1024;

  private:
    std::filesystem::path root_;
};
Card CaptureCard(const CaptureRecord& record);
} // namespace rimes::capsule
