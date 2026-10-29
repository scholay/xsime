#include "chord/chord_session.hpp"
#include "chord_deployment.hpp"
#include "chord_store.hpp"
#include "engine/rime_key.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace rimes::linuxime;
using namespace rimes::chord;
namespace fs = std::filesystem;
void Check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: rimes-chord-engine-tests SHARED_DATA\n";
        return 2;
    }
    char directory[] = "/tmp/xsime-chord-engine-XXXXXX";
    if (!mkdtemp(directory))
        return 1;
    const fs::path root(directory);
    try {
        RimeEngine engine;
        RimeEngineOptions options;
        options.shared_data_dir = argv[1];
        options.user_data_dir = root;
        options.log_dir = root / "log";
        options.full_maintenance_check = true;
        std::size_t verified = 0;
        for (const auto& item : Contract().at("compilers")) {
            const auto profile = Profile::Parse(item.at("profile"));
            DeploymentFiles files(root);
            (void)files.Prepare(profile, true, Json::array({"rime_ice"}));
            std::string error;
            Check(engine.Start(options, &error), "start: " + error);
            const auto session = engine.CreateSession(&error);
            Check(session && engine.SelectSchema(session, profile.SchemaID(), &error),
                  "select: " + error);
            Check(engine.SetOption(session, "ascii_mode", false), "ascii mode");
            EngineSnapshot snapshot;
            Check(engine.Refresh(session, &snapshot) && snapshot.schema_id == profile.SchemaID(),
                  "actual schema differs");
            for (const auto& mapping : profile.mappings) {
                Check(engine.ClearComposition(session, &snapshot), "clear verification session");
                for (char key : mapping.keys)
                    Check(engine.ProcessKey(session, key, 0, &snapshot), "mapping press failed");
                for (char key : mapping.keys)
                    Check(engine.ProcessKey(session, key, kReleaseMask, &snapshot),
                          "mapping release failed");
                Check(snapshot.raw_input == *profile.Encoded(mapping),
                      "actual compiled mapping differs: " + mapping.keys);
                ++verified;
            }
            ChordSession chord;
            auto clear = [&] {
                chord.Cancel();
                Check(engine.ClearComposition(session, &snapshot), "clear");
            };
            auto stroke = [&](const std::string& keys) {
                const auto before = snapshot;
                for (char key : keys)
                    Check(chord.Stage(key, profile, before), "stage");
                auto result = chord.Flush(engine, session, profile, [] { return true; });
                Check(result.has_value(), "settlement discarded");
                snapshot = *result;
                return snapshot.raw_input;
            };
            clear();
            if (profile.legacy) {
                Check(stroke("q") == "q", "literal left singleton");
                Check(stroke("km") == "qiong", "guarded q + km pairing");
                clear();
                Check(stroke("dv") == "n" && stroke("i") == "ni", "dv + i pairing");
                clear();
                Check(stroke("q") == "q" && stroke("y") == "qy",
                      "two singletons incorrectly paired");
                clear();
                (void)stroke("q");
                chord.BreakPairing();
                Check(stroke("km") == "q'ong", "explicit boundary failed to retire pairing");
            } else {
                const auto first = profile.ziranma ? "ni'" : "ni'";
                const auto full = profile.ziranma ? "ni'hk'" : "ni'hao'";
                Check(stroke("cd") == first && stroke("ae") == full,
                      "custom complete syllable boundaries");
                clear();
                Check(stroke("ac") == "n", "fragment was closed");
                Check(stroke("ef") == (profile.ziranma ? "ns'" : "nong'"),
                      "custom exact cross-batch mapping");
            }
            clear();
            const auto before = snapshot;
            Check(chord.Stage('q', profile, before) && !chord.Stage('q', profile, before),
                  "duplicate physical key");
            Check(!chord.Flush(engine, session, profile, [] { return false; }),
                  "stale target executed");
            Check(engine.Refresh(session, &snapshot) && snapshot.raw_input.empty(),
                  "stale target mutated private Rime session");
            clear();
            Check(chord.Stage('q', profile, snapshot), "stage before edit");
            Check(engine.ProcessKey(session, 'a', 0, &snapshot), "independent input edit");
            Check(engine.ProcessKey(session, 'a', kReleaseMask, &snapshot),
                  "independent input edit release");
            Check(!snapshot.raw_input.empty(), "edit fixture did not change composition");
            Check(!chord.Flush(engine, session, profile, [] { return true; }),
                  "composition edit did not retire pending batch");
            Check(engine.DestroySession(session), "destroy");
            engine.Stop();
        }
        std::size_t native_traces = 0;
        for (const auto& fixture : Contract().at("nativeTraces")) {
            const auto profile = Profile::Parse(fixture.at("profile"));
            DeploymentFiles files(root);
            (void)files.Prepare(profile, true, Json::array({"rime_ice"}));
            std::string error;
            Check(engine.Start(options, &error), "native start: " + error);
            const auto session = engine.CreateSession(&error);
            Check(session && engine.SelectSchema(session, profile.SchemaID(), &error),
                  "native select: " + error);
            Check(engine.SetOption(session, "ascii_mode", false), "native ascii mode");
            EngineSnapshot snapshot;
            for (const auto& trace : fixture.at("traces")) {
                Check(engine.ClearComposition(session, &snapshot), "native clear");
                std::string commits;
                for (const auto& value : trace.at("chords")) {
                    const auto keys = value.get<std::string>();
                    for (char key : keys) {
                        Check(engine.ProcessKey(session, key, 0, &snapshot), "native press");
                        commits += snapshot.commit_text;
                    }
                    for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
                        Check(engine.ProcessKey(session, *key, kReleaseMask, &snapshot),
                              "native release");
                        commits += snapshot.commit_text;
                    }
                }
                Check(commits == trace.at("commit").get<std::string>(),
                      "native committed text differs: " + profile.SchemaID());
                if (trace.contains("firstCandidate"))
                    Check(!snapshot.candidates.empty() &&
                              snapshot.candidates.front().text == trace.at("firstCandidate").get<std::string>(),
                          "native candidate differs: " + profile.SchemaID());
                ++native_traces;
            }
            engine.DestroySession(session);
            engine.Stop();
        }
        fs::remove_all(root);
        std::cout << "PASS: " << verified
                  << " actual compiled mappings; full/Ziranma batches, cross-batch pairing, "
                     "literal singletons, exact-context cancellation; "
                  << native_traces << " shared native traces\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
