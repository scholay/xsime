#include "capsule_hash.hpp"
#include "capsule_password.hpp"
#include "capsule_rich_text.hpp"
#include "private_files.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
using namespace rimes::capsule;
using Json = nlohmann::json;
namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void Reject(F work, const char* message) {
    bool rejected = false;
    try {
        work();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, message);
}
} // namespace
int main(int argc, char** argv) {
    const auto root =
        std::filesystem::temp_directory_path() / ("xsime-password-" + rimes::runtime::RandomID());
    try {
        std::ifstream input(NATIVE_PASSWORD_FIXTURES);
        const auto fixture = Json::parse(input);
        Check(fixture.at("purpose") == "public-parity-fixture", "non-public fixture");
        PasswordStore store(root);
        rimes::runtime::PrivateDirectory directory(root), files(root / "passwords");
        const auto key = Base64Decode(fixture.at("masterKey").get<std::string>());
        Check(key && key->size() == 32, "native key invalid");
        directory.Write("master-key", *key);
        for (const auto& doc : fixture.at("documents"))
            files.Write(doc.at("id").get<std::string>() + ".md",
                        doc.at("document").get<std::string>());
        const auto summaries = store.List();
        Check(summaries.size() == fixture.at("documents").size(), "native summaries unavailable");
        for (const auto& doc : fixture.at("documents")) {
            const auto id = doc.at("id").get<std::string>();
            const auto raw = doc.at("document").get<std::string>();
            auto value = store.Read(id, rimes::runtime::Sha256(raw));
            Check(value.body == doc.at("body") && value.title == doc.at("title") &&
                      value.summary == doc.at("summary") && value.category == doc.at("category"),
                  "native encrypted record mismatch");
            Check(PasswordCard(value).payload.empty() && PasswordCard(value).kind == Kind::Password,
                  "secret entered card payload");
            const auto saved = store.Put(value, value.revision);
            Check(store.Read(saved.id, saved.revision).body == value.body,
                  "Linux encryption roundtrip");
            Check(saved.revision != value.revision, "nonce was reused");
            Check(files.Read(saved.id + ".md", 1024 * 1024)->find("version: 2") !=
                      std::string::npos,
                  "legacy not upgraded");
            Reject([&] { store.Put(value, value.revision); }, "stale overwrite accepted");
        }
        const auto first = store.List().front();
        auto record = store.Read(first.id, first.revision);
        std::atomic<int> wins{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&] {
                try {
                    store.Put(record, record.revision);
                    ++wins;
                } catch (const std::exception&) {
                }
            });
        for (auto& thread : threads)
            thread.join();
        Check(wins == 1, "concurrent CAS did not select one winner");
        auto current = store.List().front();
        const auto valid = files.Read(current.id + ".md", 1024 * 1024).value();
        auto tampered = valid;
        const auto at = tampered.find("title: ");
        tampered.insert(at + 8, "changed");
        files.Write(current.id + ".md", tampered);
        Reject([&] { store.Read(current.id, rimes::runtime::Sha256(tampered)); },
               "unauthenticated title accepted");
        files.Write(current.id + ".md", valid);
        tampered = valid;
        const auto cipher = tampered.find("```capsule-password\n") + 20;
        tampered[cipher] = tampered[cipher] == 'A' ? 'B' : 'A';
        files.Write(current.id + ".md", tampered);
        Reject([&] { store.Read(current.id, rimes::runtime::Sha256(tampered)); },
               "corrupt ciphertext accepted");
        files.Write(current.id + ".md", valid);
        directory.Remove("master-key");
        PasswordRecord extra;
        extra.title = "Public extra";
        extra.body = "- 密码：public-test-only";
        Reject([&] { store.Put(extra, ""); }, "missing master key replaced existing vault");
        Check(!directory.Read("master-key", 32), "missing key recreated");
        directory.Write("master-key", *key);
        extra.summary = "public-test-only";
        Reject([&] { store.Put(extra, ""); }, "secret leaked through summary");
        extra.summary.clear();
        auto saved = store.Put(extra, "");
        struct stat mode {};
        Check(!stat((root / "passwords" / (saved.id + ".md")).c_str(), &mode) &&
                  (mode.st_mode & 0777) == 0600,
              "password mode");
        Check(files.Read(saved.id + ".md", 1024 * 1024)->find("public-test-only") ==
                  std::string::npos,
              "plaintext saved");
        Reject([&] { store.Remove(saved.id, std::string(64, 'a')); }, "stale deletion accepted");
        store.Remove(saved.id, saved.revision);
        const auto bad = NewUuidV4();
        files.Write(bad + ".md", "{broken");
        Check(store.List().size() == fixture.at("documents").size(),
              "broken record poisoned library");
        files.Remove(bad + ".md");
        std::filesystem::create_directory_symlink(root / "passwords", root / "link");
        Reject([&] { PasswordStore unsafe(root / "link"); }, "symlink vault accepted");
        // A complete backup carries the existing master key, never a newly generated replacement.
        const auto backup = root / "backup";
        std::filesystem::create_directory(backup);
        chmod(backup.c_str(), 0700);
        std::filesystem::copy_file(root / "master-key", backup / "master-key");
        std::filesystem::copy(root / "passwords", backup / "passwords",
                              std::filesystem::copy_options::recursive);
        PasswordStore restored(backup);
        Check(restored.Read(current.id, current.revision).body == record.body,
              "backup decrypt failed");
        ContentStore notes(root);
        Record note;
        note.title = "Public note";
        note.content = "- 密码：migration-fixture";
        note.summary = "public note summary must not migrate";
        std::string error;
        Check(notes.Put(note, "", &note, &error) == StoreStatus::Ok, "note fixture unavailable");
        Reject([&] { store.MigrateNote(note.id, std::string(64, 'a'), "login"); },
               "stale note migration accepted");
        const auto before_migration = store.List().size();
        int publication = 0;
        Reject(
            [&] {
                store.MigrateNote(note.id, note.revision, "login", "", [&](const auto& action) {
                    if (++publication == 1)
                        action(); // cancel after ciphertext is published
                });
            },
            "cancelled migration deleted the original note");
        Record preserved;
        Check(publication == 2 && store.List().size() == before_migration &&
                  notes.Get(note.id, &preserved, &error) == StoreStatus::Ok &&
                  preserved.revision == note.revision,
              "cancelled migration failed to roll back encrypted copy and retain note");
        const auto migrated = store.MigrateNote(note.id, note.revision, "login");
        Check(migrated.summary.empty(), "note summary disclosed during migration");
        Record removed;
        Check(notes.Get(note.id, &removed, &error) == StoreStatus::NotFound &&
                  store.Read(migrated.id, migrated.revision).body == note.content,
              "encrypted note migration failed");
        RevealCredential credential(root);
        Check(credential.Matches(DefaultRevealCode()), "default physical code rejected");
        RevealCode custom = {"qaz", "wsx", "edc", "rfv"};
        RevealCode wrong = {"a", "b", "c", "d"};
        const auto initial = credential.Revision();
        const auto unchanged = store.Inspect(migrated.id);
        const auto secret = store.Read(unchanged.id, unchanged.revision, initial);
        const SecretCommit cancelled = [](const auto&) {};
        Reject([&] { store.Put(secret, secret.revision, initial, cancelled); },
               "cancelled final write published");
        Reject([&] { store.Remove(secret.id, secret.revision, initial, cancelled); },
               "cancelled final remove published");
        Check(store.Inspect(secret.id).revision == secret.revision,
              "cancelled gate changed record");
        Reject([&] { credential.Change(DefaultRevealCode(), custom, initial, cancelled); },
               "cancelled credential replacement published");
        bool authorized = false;
        store.WithAuthorization(secret.id, secret.revision, initial, [&] { authorized = true; });
        Check(authorized, "current copy authorization denied");
        Reject([&] { store.WithAuthorization(secret.id, "stale", initial, [] {}); },
               "stale copy authority accepted");
        {
            auto held = directory.Lock();
            Reject([&] { store.WithAuthorization(secret.id, secret.revision, initial, [] {}); },
                   "busy copy gate blocked instead of rejecting");
        }
        Reject([&] { store.Read(secret.id, secret.revision, "changed-credential"); },
               "old authorization decrypted after credential change");
        Reject([&] { credential.Change(wrong, custom, initial); },
               "wrong current code changed credential");
        credential.Change(DefaultRevealCode(), custom, initial);
        Check(credential.Matches(custom) && !credential.Matches(DefaultRevealCode()),
              "custom code not active");
        Reject([&] { credential.Change(custom, std::nullopt, initial); }, "stale credential reset");
        credential.Change(custom, std::nullopt, credential.Revision());
        Check(credential.Matches(DefaultRevealCode()), "reset failed");
        directory.Write("reveal-credential-v1", fixture.at("credential").get<std::string>());
        Check(credential.Matches(custom) && !credential.Matches(wrong),
              "native salted credential mismatch");
        directory.Write("reveal-credential-v1", "corrupt");
        Check(!credential.Matches(DefaultRevealCode()) && !credential.Matches(custom),
              "corrupt credential fell back");
        Check(CanonicalChord("RHrh") == "rh" && !CanonicalChord("abcdefghi") &&
                  !CanonicalChord("!") && RevealPhysicalLetter(24) == 'q' &&
                  RevealPhysicalLetter(52) == 'z' && RevealPhysicalLetter(65) == 0,
              "physical chord contract");
        std::size_t code_count = 0;
        for (const auto& item : fixture.at("totp")) {
            const auto parameters =
                ParseTotp(item.at("label").get<std::string>(), item.at("value").get<std::string>());
            Check(bool(parameters) == item.at("valid").get<bool>(), "native TOTP parse drift");
            if (parameters)
                for (const auto& sample : item.at("samples")) {
                    const auto code =
                        TotpCode(*parameters, sample.at("seconds").get<std::int64_t>());
                    Check(code.first == sample.at("code") && code.second == sample.at("remaining"),
                          "native TOTP value drift");
                    ++code_count;
                }
        }
        const auto standard =
            ParseTotp("", "otpauth://totp/Public?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&digits=8");
        const std::array<std::int64_t, 6> times = {59,         1111111109, 1111111111,
                                                   1234567890, 2000000000, 20000000000};
        const std::array<const char*, 6> codes = {"94287082", "07081804", "14050471",
                                                  "89005924", "69279037", "65353130"};
        for (std::size_t i = 0; i < times.size(); ++i)
            Check(TotpCode(*standard, times[i]).first == codes[i], "RFC 6238 SHA-1 vector failed");
        Reject([&] { TotpCode(*standard, -1); }, "negative TOTP timestamp accepted");
        SecretAccess access;
        const auto now = SecretAccess::Time{} + std::chrono::hours(1);
        access.Authorize("record", "revision", "credential", now);
        access.Reveal(2, now);
        Check(access.Valid("record", "revision", "credential", now) &&
                  access.Revealed(2, now + std::chrono::seconds(14)) &&
                  !access.Revealed(2, now + std::chrono::seconds(15)),
              "15-second reveal boundary");
        Check(!access.Valid("record", "revision", "changed", now) &&
                  !access.Valid("record", "changed", "credential", now) &&
                  !access.Valid("record", "revision", "credential", now + std::chrono::seconds(60)),
              "60-second/identity revocation");
        access.Conceal();
        Check(!access.Valid("record", "revision", "credential", now), "conceal left authorization");
        if (argc == 2) {
            const auto output = std::filesystem::path(argv[1]);
            Check(!std::filesystem::exists(output), "public fixture output already exists");
            PasswordStore exported(output);
            Json bodies = Json::object();
            for (const auto& doc : fixture.at("documents")) {
                PasswordRecord value;
                value.title = doc.at("title");
                value.body = doc.at("body");
                value.category = doc.at("category");
                value.summary = doc.at("summary");
                exported.Put(value, "");
                bodies[value.title] = value.body;
            }
            rimes::runtime::PrivateDirectory(output).Write(
                "linux-public-fixture.json",
                Json{{"purpose", "public-parity-fixture"}, {"bodies", bodies}}.dump());
        }
        std::filesystem::remove_all(root);
        std::cout << "Password native/v2/legacy crypto, CAS, backup, migration, private files, "
                     "passcode and expiry passed; "
                  << code_count << " native TOTP cases and 6 RFC vectors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
