#pragma once
#include "private_files.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>
namespace rimes::mailbox {
using Json = nlohmann::json;
using Commit = std::function<void(const std::function<void()>&)>;
struct Limits {
    std::size_t threads = 200, messages = 200, characters = 1048576, bytes = 16 * 1048576;
};
struct Snapshot {
    Json document;
    std::string revision;
};
std::string Module(const Json& thread);
std::string Title(const Json& thread);
std::string Preview(const Json& thread);
std::string Status(const Json& thread);
std::vector<Json> Filter(const Snapshot& state, const std::string& module,
                         const std::string& query = "", const std::string& filter = "all");
std::size_t Unread(const Snapshot& state);
std::string Revision(const Json& thread);
double Now(); // Foundation Date's seconds since 2001-01-01, not Unix seconds.
void Validate(Json& document, const Limits& limits = {});
class Store {
  public:
    explicit Store(std::filesystem::path root = runtime::UserRoot() / "mailbox", Limits limits = {},
                   Commit commit = {});
    Snapshot Read() const;
    Json Get(const std::string& id) const;
    Json Create(const Json& source, const std::string& title, const std::string& body,
                const std::string& module = "inbox", const std::string& png = "");
    std::optional<std::string> Image(const std::string& filename) const;
    Json AddNote(const std::string& id, const std::string& expected, const std::string& body);
    Json Rename(const std::string& id, const std::string& expected, const std::string& title);
    Json Archive(const std::string& id, const std::string& expected, bool archived);
    Json MarkRead(const std::string& id, const std::string& expected);
    void Remove(const std::string& id, const std::string& expected);
    // Call only after the process has acquired exclusive primary-instance ownership.
    bool RecoverInterrupted();
    const std::filesystem::path& Root() const { return files_.Path(); }

  private:
    Snapshot Load() const;
    void Save(Json document) const;
    Json Change(const std::string& id, const std::string& expected,
                const std::function<void(Json&)>& change);
    runtime::PrivateDirectory files_;
    Limits limits_;
    Commit commit_;
};
} // namespace rimes::mailbox
