#pragma once
#include "plugin_store.hpp"
#include <functional>

namespace rimes::runtime {
using Json = plugins::Json;
enum class FieldKind { Text, Secret, Toggle, Choice, Number };
struct Field {
  std::string id, title, help;
  FieldKind kind;
  Json default_value;
  std::vector<std::pair<std::string, std::string>> choices;
  int minimum = 0, maximum = 4096;
};
std::vector<Field> PluginFields(std::string_view id);
std::vector<Field> ProviderFields();
Json FieldDefaults(const std::vector<Field> &fields);
Json ValidateFields(const std::vector<Field> &fields, const Json &values);
bool ValidEndpoint(const std::string &url, bool optional = false);

struct RequestConfiguration {
  std::string revision;
  Json plugin;
  Json provider;
  std::string credential; // Never serialized in a snapshot or diagnostic.
};

// A save publishes one immutable preferences revision. Credential blobs are
// private, separately owned files; preferences contain opaque references only.
class ConfigurationStore {
public:
  explicit ConfigurationStore(const std::filesystem::path &root, bool wait_for_seed = true);
  Json Load() const;
  // False means preferences committed but a retired credential could not be
  // removed. A caller must not report that the transaction rolled back.
  bool Save(Json next, const std::string &expected_revision,
            const std::map<std::string, std::string> &credential_changes = {});
  RequestConfiguration Freeze(std::string_view plugin_id) const;
  Json Plugin(std::string_view plugin_id) const;
  void WithGateway(const std::string& revision, const std::function<void()>& action) const;
  const std::filesystem::path &Path() const { return files_.Path(); }
  static Json Defaults();
  static Json NormalizePlugin(std::string_view id, const Json &value);

private:
  static void Validate(const Json &value);
  PrivateDirectory files_;
  PrivateDirectory credentials_;
};
} // namespace rimes::runtime
