#include "user_lexicon.hpp"
#include <windows.h>
#include <objbase.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace wb = rimes::windows::workbench;
namespace engine = rimes::windows::engine;
namespace fs = std::filesystem;
namespace {
void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Reject(F&& operation, const char* message) {
  bool rejected = false;
  try { operation(); } catch (const std::exception&) { rejected = true; }
  Require(rejected, message);
}
void Write(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  Require(static_cast<bool>(file), "fixture write failed");
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  Require(static_cast<bool>(file), "fixture read failed");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
fs::path Fixture(const fs::path& parent) {
  GUID guid{}; wchar_t name[40]{};
  Require(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, name, 40), "fixture name failed");
  auto path = fs::absolute(parent / (L"xsime-learning-" + std::wstring(name))).lexically_normal();
  fs::create_directories(path); return path;
}
std::string Snapshot(const std::string& name) {
  return "# Rime user dictionary\n#@/db_type\tuserdb\n#@/db_name\t" + name +
      "\n#@/user_id\tfixture\n#@/tick\t10\na \t快照词条\tc=3 d=0.5 t=10\n";
}
void Validation() {
  const auto chinese = wb::UserLexiconKind::Chinese;
  auto result = wb::UserLexiconService::Validate(chinese, "\xef\xbb\xbf词条\tci tiao\t3\r\n");
  Require(result.entries == 1 && !result.snapshot && !result.text.starts_with("\xef\xbb\xbf"), "legacy TSV/BOM failed");
  result = wb::UserLexiconService::Validate(chinese, Snapshot("rime_ice"));
  Require(result.entries == 1 && result.snapshot && result.dictionary == "rime_ice", "packed snapshot failed");
  for (const auto& bad : std::vector<std::string>{"", "# empty\n", "word\tcode\tnan\n", "word\tcode\t-1\n", "word\tcode\tinf\n",
      "word\t\t1\n", "word\tcode\t1\ncode \tword\tc=1 d=1 t=1\n", "#@/db_name\twubi86\nword\tcode\t1\n",
      "#@/db_name\trime_ice\n#@/db_name\twubi86\nword\tcode\t1\n", "code \tword\tc=1 d=1 t=1\n",
      "#@/db_name\trime_ice\ncode \tword\tc=1 d=nan t=1\n", "#@/db_name\trime_ice\ncode \tword\tc=1 c=2 d=1 t=1\n",
      "#@/db_name\trime_ice\ncode \tword\tc=2147483648 d=1 t=1\n",
      "#@/db_name\trime_ice\ncode \tword\tc=1 d=1 t=4294967296\n",
      std::string("word\0\tcode\t1", 12), std::string("\xc0\xaf\tcode\t1\n"), std::string(65537, 'x') + "\tcode\t1\n"})
    Reject([&] { wb::UserLexiconService::Validate(chinese, bad); }, "invalid learning input accepted");
  Reject([&] { wb::UserLexiconService::Validate(chinese, std::string(wb::UserLexiconService::kFileLimit + 1, 'x')); }, "file size bound not enforced");
  result = wb::UserLexiconService::Validate(chinese, "# no comment\n#@/db_name   rime_ice\na \t词条\tc=1 d=1 t=1\n");
  Require(!result.text.starts_with("# no comment") && result.text.starts_with("#@/db_name\trime_ice\n"), "TSV metadata interpreter disagrees with validation");
}
void Service(const fs::path& root) {
  using Kind = wb::UserLexiconKind;
  const auto source = root / L"selected.tsv", destination = root / L"export.tsv", temporary = root / L"private";
  Write(source, "学习词条\ta\t4\n"); Write(destination, "keep original");
  bool available = true, export_failure = false, import_failure = false;
  int imports = 0, restores = 0;
  wb::UserLexiconBackend backend;
  backend.dictionaries = [&](auto* names, auto* error) {
    if (!available) { *error = "fixture offline"; return false; }
    *names = {"rime_ice"}; return true;
  };
  backend.import_data = [&](const auto& dictionary, const auto& path, auto* error) {
    ++imports;
    Require(dictionary == "rime_ice" && path != source && path.parent_path() == temporary, "import bypassed private stage");
    Require(Read(path) == "学习词条\ta\t4\n", "private stage changed");
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Require(writer == INVALID_HANDLE_VALUE, "private import was writable during backend read");
    if (import_failure) { *error = "fixture import failure"; return -1; }
    return 1;
  };
  backend.restore = [&](const auto& path, auto*) {
    ++restores;
    Require(path.filename().wstring().ends_with(L".userdb.txt"), "snapshot suffix missing");
    Require(Read(path) == Snapshot("rime_ice"), "snapshot was converted lossily"); return true;
  };
  backend.export_data = [&](const auto& dictionary, const auto& path, auto* error) {
    Require(dictionary == "rime_ice" && path != destination, "export bypassed private stage");
    Write(path, export_failure ? "partial" : "学习词条\ta\t4\n");
    if (export_failure) { *error = "fixture export failure"; return -1; } return 1;
  };
  wb::UserLexiconService service(std::move(backend), temporary);
  auto statuses = service.Status();
  Require(statuses[0].state == wb::UserLexiconState::DatabasePresent && statuses[1].state == wb::UserLexiconState::NoDatabase, "status lost existence distinction");
  available = false; Require(service.Status()[0].state == wb::UserLexiconState::Unavailable, "offline status wrong"); available = true;
  auto result = service.Import(Kind::Chinese, source);
  Require(result.entries == 1 && !result.self_describing && imports == 1 && fs::is_empty(temporary), "legacy import/cleanup failed");
  import_failure = true; Reject([&] { service.Import(Kind::Chinese, source); }, "backend import failure ignored");
  Require(fs::is_empty(temporary), "failed import left a private file"); import_failure = false;
  Write(source, Snapshot("rime_ice")); result = service.Import(Kind::Chinese, source);
  Require(result.snapshot && result.self_describing && restores == 1 && fs::is_empty(temporary), "snapshot route failed");
  Reject([&] { service.Import(Kind::Wubi86, source); }, "wrong dictionary reached backend");
  Require(restores == 1 && imports == 2, "invalid import touched backend");
  export_failure = true; Reject([&] { service.Export(Kind::Chinese, destination); }, "backend export failure ignored");
  Require(Read(destination) == "keep original" && fs::is_empty(temporary), "failed export damaged destination"); export_failure = false;
  result = service.Export(Kind::Chinese, destination);
  Require(result.entries == 1 && result.self_describing && Read(destination).find("#@/db_name\trime_ice\n") != std::string::npos, "export lacks dictionary identity");
  Require(fs::is_empty(temporary), "successful export left a private file");
  Reject([&] { service.Import(Kind::Chinese, root); }, "directory import accepted");
  Reject([&] { service.Export(Kind::Chinese, root); }, "directory export accepted");
  Reject([&] { service.Import(Kind::Chinese, L"relative.tsv"); }, "relative import accepted");
}
void EngineFixture(const fs::path& root) {
  fs::create_directories(root / L"shared"); fs::create_directories(root / L"user"); fs::create_directories(root / L"logs");
  Write(root / L"shared/default.yaml", "config_version: '1'\nschema_list:\n  - schema: xsime_learning_test\nmenu: {page_size: 5}\nswitcher: {hotkeys: []}\n");
  Write(root / L"shared/xsime_learning_test.schema.yaml",
      "schema:\n  schema_id: xsime_learning_test\n  name: Isolated learning test\n  version: '1'\n"
      "switches:\n  - name: ascii_mode\n    reset: 0\n    states: [Chinese, ASCII]\n"
      "  - name: ascii_punct\n    reset: 0\n    states: [Full, Half]\n"
      "engine:\n  processors: [ascii_composer, speller, selector, navigator, express_editor]\n"
      "  segmentors: [ascii_segmentor, abc_segmentor, fallback_segmentor]\n  translators: [table_translator]\n  filters: []\n"
      "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n  delimiter: ' '\n"
      "translator:\n  dictionary: xsime_learning_test\n  user_dict: rime_ice\n"
      "  enable_user_dict: true\n  enable_sentence: false\n  enable_completion: false\n");
  Write(root / L"shared/xsime_learning_test.dict.yaml", "---\nname: xsime_learning_test\nversion: '1'\nsort: original\n...\n测试\ta\t100\n");
}
void LiveEngine(const fs::path& dll, const fs::path& root) {
  using Kind = wb::UserLexiconKind;
  EngineFixture(root);
  engine::RimeEngineOptions options;
  options.dll_path = fs::absolute(dll); options.shared_data_dir = root / L"shared";
  options.user_data_dir = root / L"user"; options.log_dir = root / L"logs"; options.full_maintenance_check = true;
  engine::RimeEngine rime; std::string error;
  Require(rime.Start(options, &error), "real Rime start: " + error);
  const auto session = rime.CreateSession(&error), second = rime.CreateSession(&error);
  Require(session && second, "real Rime session: " + error);
  Require(rime.Configure(session, "xsime_learning_test", false, false, true, &error, 7), "real Rime configure: " + error);
  Require(rime.Configure(second, "xsime_learning_test", true, false, false, &error, 7), "second session configure: " + error);
  const auto original_schema = Read(root / L"user/build/xsime_learning_test.schema.yaml");
  wb::UserLexiconService service(rime, root / L"private");
  const auto input = root / L"import.tsv", output = root / L"export.tsv";
  Write(input, "#@/db_name\trime_ice\n学习校验\ta\t8\n");
  Require(service.Import(Kind::Chinese, input).entries == 1, "real import count");
  Require(service.Export(Kind::Chinese, output).entries >= 1 && Read(output).find("学习校验\ta\t8") != std::string::npos, "real imported row was not exported");
  Require(!rime.IsAsciiMode(session) && rime.IsAsciiMode(second), "schema swap lost session modes");
  engine::EngineSnapshot state;
  Require(rime.ProcessKey(session, 'a', 0, &state, &error) && state.composing && !state.candidates.empty(), "preserved session cannot compose: " + error);
  Require(state.page_size == 7, "schema swap lost candidate page override");
  const auto composition = state.composition; bool composition_rejected = false;
  try { service.Import(Kind::Chinese, input); }
  catch (const std::exception& exception) { composition_rejected = std::string(exception.what()).find("组字") != std::string::npos; }
  Require(composition_rejected, "active composition was not explicitly protected");
  Require(rime.ProcessKey(session, 0xffe1, 1 << 30, &state, &error) && state.composition == composition, "rejected transfer changed live composition");
  Require(rime.ProcessKey(session, ' ', 0, &state, &error) && !state.commit_text.empty(), "preserved session cannot commit: " + error);
  Reject([&] { service.Import(Kind::Wubi86, input); }, "real wrong-dictionary input accepted");
  Reject([&] { service.Export(Kind::English, root / L"missing.tsv"); }, "missing user dictionary exported");
  Require(rime.ProcessKey(session, 'a', 0, &state, &error) && state.composing, "failed transfer broke live session");
  Require(rime.ProcessKey(session, 0xff1b, 0, &state, &error) && !state.composing, "fixture composition cancel failed");
  Write(input, Snapshot("wubi86")); Require(service.Import(Kind::Wubi86, input).snapshot, "real packed snapshot import failed");
  service.Export(Kind::Wubi86, output);
  Require(Read(output).find("快照词条\ta\t3") != std::string::npos, "packed snapshot was not restored");
  Write(input, "#@/db_name\tenglish\nlearning\tlearning\t2\n"); service.Import(Kind::English, input);
  for (const auto& status : service.Status()) Require(status.state == wb::UserLexiconState::DatabasePresent, "real dictionary enumeration failed");
  Require(rime.IsAsciiMode(second), "later transfers lost second session mode");
  Require(Read(root / L"user/build/xsime_learning_test.schema.yaml") == original_schema, "temporary component tree escaped to schema file");
  Require(rime.DestroySession(second, &error) && rime.DestroySession(session, &error), "live session IDs were destroyed by transfer");
  rime.Stop(); options.full_maintenance_check = false;
  Require(rime.Start(options, &error), "real Rime restart: " + error);
  service.Export(Kind::Chinese, output);
  Require(Read(output).find("学习校验\ta\t") != std::string::npos, "learning did not survive restart");
  const auto resumed = rime.CreateSession(&error);
  Require(resumed && rime.ProcessKey(resumed, 'a', 0, &state, &error) && state.composing, "restart session cannot type");
  Require(rime.ProcessKey(resumed, ' ', 0, &state, &error) && !state.commit_text.empty(), "restart session cannot commit");
  rime.Stop();
  Require(Read(root / L"user/user.yaml").find("__xsime_learning_guard_") == std::string::npos, "internal schema ID escaped to history");
  for (const auto& entry : fs::recursive_directory_iterator(root))
    Require(entry.path().filename().wstring().find(L"__xsime_learning_guard_") == std::wstring::npos, "internal schema escaped to disk");
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 4 && std::wstring(argv[1]) == L"--engine") {
      const auto root = Fixture(fs::absolute(argv[3]));
      std::wcout << L"Private real-engine fixture: " << root.wstring() << std::endl;
      LiveEngine(argv[2], root);
      std::cout << "PASS real Rime learning import/export, snapshot, composition protection, live sessions, restart\n";
    } else {
      Require(argc == 1, "usage: RimesUserLexiconTests [--engine <absolute-rime.dll> <private-output-parent>]");
      Validation(); const auto root = Fixture(fs::temp_directory_path()); Service(root);
      // Only the uniquely-created fixture is recursively removed.
      fs::remove_all(root);
      std::cout << "PASS learning validation, private immutable staging, atomic export, status and failure cleanup\n";
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return EXIT_FAILURE; }
}
