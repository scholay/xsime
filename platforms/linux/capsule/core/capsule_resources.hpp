#pragma once
#include "capsule_store.hpp"
#include <atomic>
#include <cstdint>
namespace rimes::capsule {
// Opens every component with O_NOFOLLOW, never follows a link or reads a FIFO.
// Caller owns the returned fd. Only absolute, lexical paths are accepted.
int OpenResource(const std::filesystem::path& path);
bool ValidateResource(const Record& record, std::string* error);
std::string ReadResource(const std::filesystem::path& path, std::size_t limit,
                         const std::atomic<bool>& cancelled);
// Explicit attachment imports survive deletion of the source and backup moves.
// Materialization preserves the original filename for external applications.
std::filesystem::path ResolveResource(const Record& record, const ContentStore& store);
std::string ResourceUri(const std::filesystem::path& path);
struct ResourcePreview {
    std::string text, png, message;
    int pages = 0;
};
ResourcePreview PreviewResource(const Record& record, const ContentStore& store,
                                const std::atomic<bool>& cancelled);
} // namespace rimes::capsule
