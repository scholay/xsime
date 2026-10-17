#include "native_buffer_metrics.hpp"
#include "configuration.hpp"
#include "shortcuts.hpp"
#include <algorithm>
#include <cmath>
#include <curl/curl.h>
#include <memory>

namespace rimes::runtime {
namespace {
bool Identity(const std::string &id) {
  return id.size() == 32 &&
         id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Plain(const std::string &text, std::size_t maximum) {
  return text.size() <= maximum &&
         text.find_first_of("\0\r\n", 0, 3) == std::string::npos;
}
void Require(bool valid) {
  if (!valid)
    throw std::runtime_error("设置无效，请检查字段格式、选项和范围。");
}
Field Choice(std::string id, std::string title, std::string initial,
             std::vector<std::pair<std::string, std::string>> choices,
             std::string help = {}) {
  return {std::move(id),     std::move(title), std::move(help),
          FieldKind::Choice, initial,          std::move(choices)};
}
std::string URLPart(CURLU *url, CURLUPart part) {
  char *value = nullptr;
  if (curl_url_get(url, part, &value, 0) != CURLUE_OK)
    return {};
  std::string result(value);
  curl_free(value);
  return result;
}
} // namespace

std::vector<Field> PluginFields(std::string_view id) {
  if (id == "builtin.capsule.temporary")
    return {{"captureEnabled", "允许收录剪贴板内容",
             "XSIME 运行时后台收录；安全输入、锁屏和休眠期间不会读取。",
             FieldKind::Toggle, false, {}}};
  if (id == "builtin.apple-translation")
    return {
        Choice("sourceLanguage", "源语言", "zh-Hans",
               {{"auto", "自动识别"}, {"zh-Hans", "简体中文"}, {"en", "英语"}}),
        Choice("targetLanguage", "目标语言", "en",
               {{"zh-Hans", "简体中文"}, {"en", "英语"}}),
        {"continuous",
         "连续翻译",
         "输入新内容时更新翻译，原文不离开本机。",
         FieldKind::Toggle,
         true,
         {}},
        {"speech",
         "朗读译文",
         "点击或发送单块译文时朗读；生成完成不会自动朗读。",
         FieldKind::Toggle,
         false,
         {}}};
  if (id == "builtin.codex-cli")
    return {Choice("model", "模型", "gpt-6-sol",
                   {{"gpt-6-astra", "GPT-6 Astra"},
                    {"gpt-6-sol", "GPT-6 Sol"},
                    {"gpt-6-luna", "GPT-6 Luna"},
                    {"gpt-5.6-sol", "GPT-5.6 Sol"},
                    {"gpt-5.6-terra", "GPT-5.6 Terra"},
                    {"gpt-5.6-luna", "GPT-5.6 Luna"}}),
            Choice("effort", "推理深度", "medium",
                   {{"low", "low"},
                    {"medium", "medium"},
                    {"high", "high"},
                    {"xhigh", "xhigh"},
                    {"max", "max"},
                    {"ultra", "ultra"}})};
  if (id == "builtin.claude-code-cli")
    return {Choice("model", "模型", "claude-opus-5-5",
                   {{"claude-opus-5-5", "Claude Opus 5.5"},
                    {"opus", "Opus"},
                    {"sonnet", "Sonnet"},
                    {"haiku", "Haiku"}}),
            Choice("effort", "推理深度", "medium",
                   {{"low", "low"},
                    {"medium", "medium"},
                    {"high", "high"},
                    {"xhigh", "xhigh"},
                    {"max", "max"}})};
  if (id == "builtin.stream-input")
    return {
        Choice("connector", "AI 连接器", "openai-compatible",
               {{"openai-compatible", "AI API"},
                {"codex-cli", "ChatGPT"},
                {"claude-code-cli", "Claude"}}),
        {"candidateCount",
         "候选数量",
         "显示 1–5 个候选。",
         FieldKind::Number,
         5,
         {},
         1,
         5},
        Choice("latency", "响应节奏", "balanced",
               {{"fast", "灵敏"}, {"balanced", "平衡"}, {"stable", "稳定"}})};
  // AI API deliberately has no task prompt or channel picker. Its account
  // and model belong to Connectors; each AI plugin owns exactly one channel.
  return {};
}

std::vector<Field> ProviderFields() {
  return {{"name",
           "名称",
           "例如：OpenAI / OpenRouter",
           FieldKind::Text,
           "",
           {},
           1,
           128},
          {"baseURL",
           "API 地址",
           "HTTPS API 地址；本机服务可使用 HTTP 回环地址。",
           FieldKind::Text,
           "https://api.openai.com/v1",
           {},
           1,
           2048},
          {"documentationURL",
           "文档",
           "可选的 HTTPS 文档地址。",
           FieldKind::Text,
           "",
           {},
           0,
           2048},
          {"model",
           "模型",
           "普通文本模型（留空即停用）",
           FieldKind::Text,
           "",
           {},
           0,
           256},
          {"apiKey",
           "API Key",
           "留空保持不变；清除密钥需单独操作。",
           FieldKind::Secret,
           "",
           {},
           0,
           8192}};
}

Json FieldDefaults(const std::vector<Field> &fields) {
  auto result = Json::object();
  for (const auto &field : fields)
    if (field.kind != FieldKind::Secret)
      result[field.id] = field.default_value;
  return result;
}
Json ValidateFields(const std::vector<Field> &fields, const Json &values) {
  Require(values.is_object());
  auto result = FieldDefaults(fields);
  for (auto it = values.begin(); it != values.end(); ++it) {
    const auto field =
        std::find_if(fields.begin(), fields.end(),
                     [&](const Field &f) { return f.id == it.key(); });
    Require(field != fields.end() && field->kind != FieldKind::Secret);
    const auto &value = it.value();
    switch (field->kind) {
    case FieldKind::Toggle:
      Require(value.is_boolean());
      break;
    case FieldKind::Number:
      Require(value.is_number_integer() && value >= field->minimum &&
              value <= field->maximum);
      break;
    case FieldKind::Choice:
      Require(value.is_string() &&
              std::any_of(
                  field->choices.begin(), field->choices.end(),
                  [&](const auto &option) { return option.first == value; }));
      break;
    case FieldKind::Text:
      Require(value.is_string() &&
              Plain(value.get<std::string>(), field->maximum) &&
              value.get<std::string>().size() >=
                  static_cast<std::size_t>(field->minimum));
      break;
    case FieldKind::Secret:
      Require(false);
      break;
    }
    result[it.key()] = value;
  }
  return result;
}

bool ValidEndpoint(const std::string &text, bool optional) {
  if (text.empty())
    return optional;
  if (!Plain(text, 2048))
    return false;
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(),
                                                          curl_url_cleanup);
  if (!url ||
      curl_url_set(url.get(), CURLUPART_URL, text.c_str(), 0) != CURLUE_OK)
    return false;
  const auto scheme = URLPart(url.get(), CURLUPART_SCHEME),
             host = URLPart(url.get(), CURLUPART_HOST);
  return !host.empty() && URLPart(url.get(), CURLUPART_USER).empty() &&
         URLPart(url.get(), CURLUPART_PASSWORD).empty() &&
         URLPart(url.get(), CURLUPART_QUERY).empty() &&
         URLPart(url.get(), CURLUPART_FRAGMENT).empty() &&
         (scheme == "https" ||
          (scheme == "http" &&
           (host == "localhost" || host == "127.0.0.1" || host == "[::1]")));
}

Json ConfigurationStore::Defaults() {
  return {{"schemaVersion", 1},       {"revision", RandomID()},
          {"theme", "night"},         {"bufferWidth", 760},
          {"closeAfterLast", true},   {"resetOnAppSwitch", false},
          {"autoSend", false},        {"autoSendSeconds", 1},
          {"globalShortcuts", true},  {"shortcuts", DefaultShortcuts()},
          {"activeConnector", "openai-compatible"},
          {"activeProvider", ""},     {"providers", Json::array()},
          {"plugins", Json::object()}};
}
ConfigurationStore::ConfigurationStore(const std::filesystem::path &root)
    : files_(root), credentials_(root / "credentials") {
  auto lock = files_.Lock();
  if (!files_.Read("preferences.json", 1024 * 1024))
    files_.Write("preferences.json", Defaults().dump());
}
Json ConfigurationStore::NormalizePlugin(std::string_view id,
                                         const Json &input) {
  auto value = input;
  Require(value.is_object());
  // Retired translation cloud fields are ignored, never reactivated.
  if (id == "builtin.apple-translation")
    for (auto key : {"provider", "connector", "channel"})
      value.erase(key);
  if (id == "builtin.codex-cli" || id == "builtin.claude-code-cli") {
    for (auto key : {"model", "effort"}) {
      if (value.contains(key) && (value[key] == "" || value[key] == "default" ||
                                  value[key] == "follow-default"))
        value.erase(key);
    }
  }
  if (id == "builtin.stream-input" && !value.contains("latency")) {
    const auto debounce = value.value("debounceSeconds", 0.22),
               wait = value.value("maximumWaitSeconds", 0.8);
    Require(std::isfinite(debounce) && std::isfinite(wait) && debounce >= 0 &&
            wait >= 0);
    const std::vector<std::pair<std::string, std::pair<double, double>>> modes{
        {"fast", {0.14, 0.5}},
        {"balanced", {0.22, 0.8}},
        {"stable", {0.35, 1.2}}};
    auto best = modes.front();
    double distance = 1e100;
    for (const auto &mode : modes) {
      const double d = std::abs(debounce - mode.second.first) / 0.21 +
                       std::abs(wait - mode.second.second) / 0.7;
      if (d < distance) {
        best = mode;
        distance = d;
      }
    }
    value["latency"] = best.first;
  }
  value.erase("debounceSeconds");
  value.erase("maximumWaitSeconds");
  auto normalized = ValidateFields(PluginFields(id), value);
  if (id == "builtin.apple-translation")
    Require(normalized["sourceLanguage"] != normalized["targetLanguage"]);
  return normalized;
}
void ConfigurationStore::Validate(const Json &value) {
  Require(value.is_object() && value.size() == 14 &&
          value.at("schemaVersion") == 1 && Identity(value.at("revision")));
  Require(value.at("theme") == "night" || value.at("theme") == "day" ||
          value.at("theme") == "quiet" || value.at("theme") == "rasta" ||
          value.at("theme") == "liquidGlass");
  Require(value.at("bufferWidth").is_number_integer() &&
          value.at("bufferWidth") >= rimes::native::bufferMinimumWidth && value.at("bufferWidth") <= rimes::native::bufferMaximumWidth);
  for (auto key : {"closeAfterLast", "resetOnAppSwitch", "autoSend", "globalShortcuts"})
    Require(value.at(key).is_boolean());
  ValidateShortcuts(value.at("shortcuts"));
  Require(value.at("autoSendSeconds").is_number_integer() &&
          (value.at("autoSendSeconds") == 1 || value.at("autoSendSeconds") == 2 ||
           value.at("autoSendSeconds") == 3 || value.at("autoSendSeconds") == 5));
  Require(value.at("activeConnector") == "openai-compatible" ||
          value.at("activeConnector") == "codex-cli" ||
          value.at("activeConnector") == "claude-code-cli");
  Require(value.at("providers").is_array() &&
          value.at("providers").size() <= 50 &&
          value.at("plugins").is_object() && value.at("plugins").size() <= 64);
  std::set<std::string> ids;
  for (auto provider : value.at("providers")) {
    Require(provider.is_object() && provider.contains("id") &&
            Identity(provider.at("id")));
    Require(ids.insert(provider.at("id")).second);
    provider.erase("id");
    if (provider.contains("credential")) {
      Require(Identity(provider.at("credential")));
      provider.erase("credential");
    }
    const auto fields = ValidateFields(ProviderFields(), provider);
    Require(fields == provider && ValidEndpoint(fields.at("baseURL")) &&
            ValidEndpoint(fields.at("documentationURL"), true));
  }
  Require(value.at("activeProvider") == "" ||
          (value.at("activeProvider").is_string() &&
           ids.count(value.at("activeProvider"))));
  for (auto it = value.at("plugins").begin(); it != value.at("plugins").end();
       ++it) {
    Require(it.key().size() <= 128 && it.key().compare(0, 8, "builtin.") == 0);
    (void)NormalizePlugin(it.key(), it.value());
  }
}
Json ConfigurationStore::Load() const {
  const auto bytes = files_.Read("preferences.json", 1024 * 1024);
  if (!bytes)
    throw std::runtime_error("设置文件缺失，请恢复备份后重试。");
  auto value = Json::parse(*bytes);
  if (value.is_object() && !value.contains("autoSendSeconds")) value["autoSendSeconds"] = 1;
  if (value.is_object() && !value.contains("shortcuts")) value["shortcuts"] = DefaultShortcuts();
  if (value.is_object() && !value.contains("globalShortcuts")) value["globalShortcuts"] = true;
  Validate(value);
  return value;
}
bool ConfigurationStore::Save(
    Json next, const std::string &expected,
    const std::map<std::string, std::string> &changes) {
  auto lock = files_.Lock();
  if (next.is_object() && !next.contains("autoSendSeconds")) next["autoSendSeconds"] = 1;
  if (next.is_object() && !next.contains("shortcuts")) next["shortcuts"] = DefaultShortcuts();
  if (next.is_object() && !next.contains("globalShortcuts")) next["globalShortcuts"] = true;
  const auto current = Load();
  if (current.at("revision") != expected)
    throw std::runtime_error("设置已在其他窗口更新，请重新加载后再保存。");
  // UI callers cannot bind a provider to someone else's saved credential.
  for (const auto &provider : next.at("providers")) {
    const auto id = provider.at("id").get<std::string>();
    const auto old = std::find_if(
        current.at("providers").begin(), current.at("providers").end(),
        [&](const Json &p) { return p.at("id") == id; });
    const auto old_ref = old == current.at("providers").end()
                             ? ""
                             : old->value("credential", "");
    Require(provider.value("credential", "") == old_ref);
  }
  for (auto it = next.at("plugins").begin(); it != next.at("plugins").end();
       ++it)
    it.value() = NormalizePlugin(it.key(), it.value());
  Validate(next);
  for (const auto &[id, secret] : changes) {
    Require(Plain(secret, 8192));
    const auto provider =
        std::find_if(next["providers"].begin(), next["providers"].end(),
                     [&](const Json &p) { return p.at("id") == id; });
    Require(provider != next["providers"].end());
    if (secret.empty())
      provider->erase("credential");
    else {
      const auto reference = RandomID();
      credentials_.Write(id + "." + reference, secret);
      (*provider)["credential"] = reference;
    }
  }
  next["revision"] = RandomID();
  files_.Write("preferences.json", next.dump());
  // Retired credentials are removed only after the new references commit.
  // An interrupted save can leave an unreachable private blob, never cause
  // a published preference to point at a deleted or partially written key.
  bool cleaned = true;
  for (const auto &old : current.at("providers")) {
    if (!old.contains("credential"))
      continue;
    const auto still_used =
        std::any_of(next.at("providers").begin(), next.at("providers").end(),
                    [&](const Json &p) {
                      return p.at("id") == old.at("id") &&
                             p.value("credential", "") == old.at("credential");
                    });
    if (!still_used) {
      try {
        credentials_.Remove(old.at("id").get<std::string>() + "." +
                            old.at("credential").get<std::string>());
      } catch (const std::exception &) {
        cleaned = false;
      }
    }
  }
  return cleaned;
}
Json ConfigurationStore::Plugin(std::string_view id) const {
  const auto value = Load();
  return NormalizePlugin(
      id, value.at("plugins").value(std::string(id), Json::object()));
}
RequestConfiguration ConfigurationStore::Freeze(std::string_view id) const {
  auto lock = files_.Lock();
  const auto value = Load();
  RequestConfiguration result{
      value.at("revision"),
      NormalizePlugin(
          id, value.at("plugins").value(std::string(id), Json::object())),
      Json::object(),
      {}};
  const bool needs_api = id == "builtin.openai-compatible" ||
                         (id == "builtin.stream-input" &&
                          result.plugin.at("connector") == "openai-compatible");
  if (needs_api)
    for (const auto &provider : value.at("providers"))
      if (provider.at("id") == value.at("activeProvider")) {
        result.provider = provider;
        if (provider.contains("credential")) {
          const auto bytes = credentials_.Read(
              provider.at("id").get<std::string>() + "." +
                  provider.at("credential").get<std::string>(),
              8192);
          Require(bytes.has_value() && Plain(*bytes, 8192));
          result.credential = *bytes;
        }
        result.provider.erase("credential");
      }
  return result;
}
} // namespace rimes::runtime
