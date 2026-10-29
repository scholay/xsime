#include "chord_deployment.hpp"
#include "chord_protocol.hpp"
#include "chord_service.hpp"
#include "engine/rime_key.hpp"
#include "fcitx/rimes_ime.hpp"
#include "fcitx/rimes_state.hpp"
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontextmanager.h>
#include <fstream>
#include <algorithm>
#include <yaml-cpp/yaml.h>

namespace fcitx {
using namespace rimes::chord;
namespace rt = rimes::runtime;
struct ChordService::Impl {
    RimesIme& ime;
    rimes::linuxime::RimeEngineOptions options;
    Store store;
    ActiveProfile active;
    Json state, previous_state, default_schemas = Json::array();
    bool imported = false, enabled = false, bootstrapped = false, verified = false;
    bool applying = false, rolling_back = false, queued_enable = false;
    std::uint64_t revision = 1, duration = 100000;
    std::string phase = "starting", message, operation;
    std::optional<rimes::chord::Profile> candidate;
    std::optional<rimes::plugins::Lease> lease;
    std::optional<DeploymentFiles::Snapshot> files;
    std::unique_ptr<DeploymentFiles> deployment;
    std::unique_ptr<EventSourceTime> timer;
    std::unique_ptr<dbus::Bus> bus;
    std::unique_ptr<dbus::Slot> service;
    rimes::linuxime::RimeEngine::SessionId verification = 0;
    std::size_t verification_index = 0;
    Impl(RimesIme& owner, const rimes::linuxime::RimeEngineOptions& engine_options,
         bool is_imported)
        : ime(owner), options(engine_options), store(rt::UserRoot()), active(store.Active()),
          imported(is_imported) {
        options.full_maintenance_check = true;
        options.wait_for_maintenance = false;
        try {
            state = store.State();
        } catch (...) {
            state = {{"selectedSchema", ""},
                     {"lastOrdinarySchema", "rime_ice"},
                     {"recovery", "并击运行配置不可读，请重新应用键位。"}};
        }
        if (!active.error.empty()) {
            state["recovery"] = active.error;
            message = active.error;
        }
        try {
            const auto path = options.shared_data_dir / "default.yaml";
            if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
                throw std::runtime_error("默认方案文件过大");
            const auto source = YAML::LoadFile(path.string());
            if (!source["schema_list"].IsSequence() || source["schema_list"].size() > 128)
                throw std::runtime_error("默认方案列表不可用");
            for (const auto& entry : source["schema_list"]) {
                const auto id = entry["schema"].as<std::string>();
                if (!IsChordSchema(id))
                    default_schemas.push_back(id);
            }
        } catch (...) {
            default_schemas = Json::array({"rime_ice"});
        }
        try {
            bus = std::make_unique<dbus::Bus>(dbus::BusType::Session);
            if (!bus->isOpen())
                return;
            bus->attachEventLoop(&ime.instance()->eventLoop());
            if (!bus->requestName(kBusName, dbus::RequestNameFlag::None))
                return;
            service = bus->addObject(kBusPath, [this](dbus::Message& request) {
                if (request.type() != dbus::MessageType::MethodCall ||
                    request.interface() != kBusInterface || request.member() != "Command" ||
                    request.signature() != "s")
                    return false;
                std::string bytes;
                request >> bytes;
                Json result;
                try {
                    if (!request || bytes.size() > kMaximumProfileBytes + 4096)
                        throw std::runtime_error("并击命令过大或格式错误");
                    result = Command(Json::parse(bytes));
                } catch (const std::exception& error) {
                    result = {{"ok", false}, {"message", error.what()}};
                }
                auto reply = request.createReply();
                reply << result.dump();
                reply.send();
                return true;
            });
        } catch (...) {
            service.reset();
            bus.reset();
        }
    }
    ~Impl() {
        timer.reset();
        service.reset();
        bus.reset();
        EndVerification();
    }
    void EndVerification() {
        if (verification) {
            ime.engine().DestroySession(verification);
            verification = 0;
        }
    }
    void Retire(bool destroy) {
        ++revision;
        ime.instance()->inputContextManager().foreach ([&](InputContext* ic) {
            if (auto* input = ic->propertyFor(&ime.factory())) {
                if (destroy)
                    input->retireSession();
                else
                    input->cancelChord(true);
            }
            return true;
        });
    }
    bool Effective() const {
        return enabled && verified && !applying && !queued_enable && !imported &&
               state.value("recovery", std::string{}).empty();
    }
    std::string Fallback() const {
        const auto selected = state.value("lastOrdinarySchema", std::string("rime_ice"));
        return selected.empty() || IsChordSchema(selected) ? "rime_ice" : selected;
    }
    void SaveState() { store.SaveState(state); }
    void Arm() {
        const auto deadline = now(CLOCK_MONOTONIC) + 10000;
        if (!timer)
            timer = ime.instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, deadline, 1000, [this](EventSourceTime*, std::uint64_t) {
                    Poll();
                    return true;
                });
        else {
            timer->setTime(deadline);
            timer->setOneShot();
        }
    }
    void StartEngine() {
        EndVerification();
        Retire(true);
        ime.engine().Stop();
        std::string error;
        if (!ime.engine().Start(options, &error))
            throw std::runtime_error("Rime 重新部署启动失败");
        phase = rolling_back ? "restoring" : "deploying";
        Arm();
    }
    void Begin(const rimes::chord::Profile& profile, bool lifecycle = false) {
        if (applying || ime.engine().IsDeploying())
            throw std::runtime_error("正在部署输入方案，请稍后重试");
        if (imported)
            throw std::runtime_error("当前使用导入方案库；请先切回内置方案库再应用并击");
        if (!lifecycle && !enabled)
            throw std::runtime_error("请先启用并击扩展");
        previous_state = state;
        candidate = rimes::chord::Profile::Parse(profile.Value());
        lease = ime.plugins().Authorize(kPluginID, "input.chord");
        if (enabled && (!lease || !ime.acceptsPluginResult(*lease)))
            throw std::runtime_error("并击授权已变化，请重试");
        deployment = std::make_unique<DeploymentFiles>(options.user_data_dir);
        files = deployment->Prepare(*candidate, enabled, default_schemas);
        applying = true;
        rolling_back = false;
        operation = rt::RandomID();
        message.clear();
        try {
            StartEngine();
        } catch (const std::exception& error) {
            Rollback(error.what());
        }
    }
    void Fail(const std::string& reason) {
        EndVerification();
        applying = false;
        verified = false;
        phase = "error";
        message = reason;
        state["recovery"] = reason;
        state["selectedSchema"] = Fallback();
        try {
            SaveState();
        } catch (...) {
            message += "；恢复状态未能保存";
        }
        files.reset();
        candidate.reset();
        lease.reset();
        Retire(false);
    }
    void Rollback(const std::string& reason) {
        message = reason;
        if (rolling_back) {
            Fail("旧键位部署验证失败，已暂停并击并退回普通输入；请重新应用键位。");
            return;
        }
        EndVerification();
        try {
            if (files)
                deployment->Restore(*files);
            rolling_back = true;
            StartEngine();
        } catch (...) {
            Fail("键位方案恢复失败，已暂停并击并退回普通输入；原数据保留，请检查部署文件。");
        }
    }
    const Json& NativeTraces(const rimes::chord::Profile& profile) const {
        for (const auto& fixture : Contract().at("nativeTraces"))
            if (fixture.at("profile").at("id") == profile.id)
                return fixture.at("traces");
        throw std::runtime_error("原生并击方案缺少验证样例");
    }
    void Poll() {
        if (!applying) {
            if (queued_enable) {
                if (ime.engine().IsDeploying()) {
                    Arm();
                    return;
                }
                queued_enable = false;
                try {
                    Begin(active.profile, true);
                } catch (const std::exception& error) {
                    Fail(error.what());
                }
            }
            return;
        }
        std::string error;
        if (!ime.engine().PollMaintenance(&error)) {
            if (ime.engine().IsDeploying()) {
                Arm();
                return;
            }
            Rollback("Rime 部署失败，未应用新键位。");
            return;
        }
        const auto& profile = rolling_back ? active.profile : *candidate;
        if (!rolling_back && enabled && (!lease || !ime.acceptsPluginResult(*lease))) {
            Rollback("部署期间配置或插件授权发生变化，已取消应用。");
            return;
        }
        // Disabling a plugin removes its schema from the switcher and returns
        // to ordinary input; no hidden chord selection is required to verify it.
        if (!verification) {
            verification = ime.engine().CreateSession();
            verification_index = 0;
            const auto schema = enabled || rolling_back ? profile.SchemaID() : Fallback();
            rimes::linuxime::EngineSnapshot snapshot;
            if (!verification || !ime.engine().SelectSchema(verification, schema) ||
                !ime.engine().Refresh(verification, &snapshot) || snapshot.schema_id != schema) {
                Rollback("部署后的实际输入方案无法选中，未应用新键位。");
                return;
            }
            ime.engine().SetOption(verification, "ascii_mode", false);
            phase = rolling_back ? "restoring" : "verifying";
        }
        // Bound work per event-loop iteration; ordinary input never waits for
        // a full 4096-mapping validation sweep.
        const auto* traces = profile.native_schema.empty() ? nullptr : &NativeTraces(profile);
        const auto count = traces ? traces->size() : profile.mappings.size();
        for (unsigned batch = 0;
             (enabled || rolling_back) && batch < 8 && verification_index < count;
             ++batch, ++verification_index) {
            rimes::linuxime::EngineSnapshot snapshot;
            bool ok = ime.engine().ClearComposition(verification, &snapshot);
            std::string commits;
            auto stroke = [&](const std::string& keys) {
                for (char key : keys) {
                    ok = ime.engine().ProcessKey(verification, key, 0, &snapshot) && ok;
                    commits += snapshot.commit_text;
                }
                for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
                    ok = ime.engine().ProcessKey(verification, *key, rimes::linuxime::kReleaseMask,
                                                 &snapshot) &&
                         ok;
                    commits += snapshot.commit_text;
                }
            };
            if (traces) {
                const auto& trace = traces->at(verification_index);
                for (const auto& keys : trace.at("chords"))
                    stroke(keys.get<std::string>());
                ok = ok && commits == trace.at("commit").get<std::string>();
                if (trace.contains("firstCandidate"))
                    ok = ok && !snapshot.candidates.empty() &&
                         snapshot.candidates.front().text == trace.at("firstCandidate").get<std::string>();
            } else {
                const auto& entry = profile.mappings[verification_index];
                stroke(entry.keys);
                ok = ok && commits.empty() &&
                     snapshot.raw_input == profile.Encoded(entry).value_or("");
            }
            if (!ok) {
                Rollback("部署后的键位映射与已保存快照不一致，未应用新键位。");
                return;
            }
        }
        if ((enabled || rolling_back) && verification_index < count) {
            Arm();
            return;
        }
        EndVerification();
        try {
            if (!rolling_back) {
                if (enabled) {
                    store.Activate(profile);
                    if (IsChordSchema(state.value("selectedSchema", std::string{})))
                        state["selectedSchema"] = profile.SchemaID();
                } else if (IsChordSchema(state.value("selectedSchema", std::string{})))
                    state["selectedSchema"] = Fallback();
                state["recovery"] = "";
                SaveState();
                if (enabled)
                    active = store.Active();
                message = enabled ? "键位已应用并通过部署验证" : "并击已停用，普通输入可继续使用";
            }
        } catch (const std::exception&) {
            // Restore the effective snapshot before compiling the old schema.
            try {
                store.Activate(active.profile);
                state = previous_state;
                SaveState();
            } catch (...) {
                Fail("应用快照保存失败，已暂停并击");
                return;
            }
            Rollback("键位状态保存失败，正在恢复旧方案。");
            return;
        }
        applying = false;
        verified = state.value("recovery", std::string{}).empty();
        phase = rolling_back ? "rolled-back" : "ready";
        files.reset();
        candidate.reset();
        lease.reset();
        Retire(false);
        if (queued_enable) {
            queued_enable = false;
            try {
                Begin(active.profile, true);
            } catch (const std::exception& error) {
                Fail(error.what());
            }
        }
    }
    void Enable(bool next) {
        if (enabled == next)
            return;
        enabled = next;
        if (next)
            verified = false;
        Retire(false);
        if (!next && IsChordSchema(state.value("selectedSchema", std::string{}))) {
            state["selectedSchema"] = Fallback();
            try {
                SaveState();
            } catch (...) {
            }
        }
        if (!bootstrapped)
            return;
        // Registry marks the adapter enabled after its start callback returns.
        // Acquire the live grant on the next loop turn, never inside start().
        queued_enable = true;
        if (!applying)
            Arm();
    }
    void EngineReady() {
        if (bootstrapped || applying)
            return;
        bootstrapped = true;
        if (enabled && active.error.empty()) {
            try {
                Begin(active.profile, true);
            } catch (const std::exception& error) {
                Fail(error.what());
            }
        } else {
            verified = active.error.empty();
            phase = "ready";
        }
    }
    Json Status() const {
        return {{"ok", true},
                {"enabled", enabled},
                {"available", Effective()},
                {"busy", applying || queued_enable},
                {"phase", phase},
                {"message", message},
                {"operation", operation},
                {"duration", duration / 1000000.0},
                {"active", active.profile.Value()},
                {"activeRevision", active.revision},
                {"selectedSchema", state.at("selectedSchema")},
                {"defaultSchemas", default_schemas},
                {"recovery", state.at("recovery")}};
    }
    Json Command(const Json& request) {
        if (!request.is_object())
            throw std::runtime_error("并击命令格式错误");
        const auto action = request.at("op").get<std::string>();
        if (action == "status")
            return Status();
        if (action == "apply") {
            if (request.value("revision", std::string{}) != active.revision)
                throw std::runtime_error("已应用版本发生变化，请刷新后重试");
            Begin(rimes::chord::Profile::Parse(request.at("profile")));
            return Status();
        }
        if (action == "selectBuiltin") {
            if (imported) throw std::runtime_error("请先在方案管理中恢复内置方案并重启 Fcitx5");
            if (applying || queued_enable) throw std::runtime_error("方案正在部署，请稍后选择");
            auto schema = request.value("schema", std::string{});
            if (schema == "isaac2026") {
                const auto grant = ime.plugins().Authorize(kPluginID, "input.chord");
                if (!Effective() || !grant || !ime.acceptsPluginResult(*grant)) throw std::runtime_error("请先启用并应用并击扩展");
                schema = active.profile.SchemaID();
            } else {
                if (std::find(default_schemas.begin(), default_schemas.end(), Json(schema)) == default_schemas.end()) throw std::runtime_error("不是可用的内置方案");
                state["lastOrdinarySchema"] = schema;
            }
            state["selectedSchema"] = schema; SaveState();
            ime.instance()->inputContextManager().foreach ([&](InputContext* ic) {
                if (auto* input = ic->propertyFor(&ime.factory())) input->requestSchema(schema);
                return true;
            });
            return Status();
        }
        if (action == "makeCurrent") {
            const auto grant = ime.plugins().Authorize(kPluginID, "input.chord");
            if (!Effective() || !grant || !ime.acceptsPluginResult(*grant))
                throw std::runtime_error("请先启用并完成键位应用");
            state["selectedSchema"] = active.profile.SchemaID();
            SaveState();
            Retire(true);
            return Status();
        }
        throw std::runtime_error("未知的并击操作");
    }
};
ChordService::ChordService(RimesIme& ime, const rimes::linuxime::RimeEngineOptions& options,
                           bool imported)
    : impl_(std::make_unique<Impl>(ime, options, imported)) {}
ChordService::~ChordService() = default;
void ChordService::Enable(bool enabled) { impl_->Enable(enabled); }
void ChordService::Configure(const Json& configuration) {
    const auto fields = rt::ConfigurationStore::NormalizePlugin(
        kPluginID, configuration.at("plugins").value(kPluginID, Json::object()));
    const auto duration = fields.at("durationMilliseconds").get<std::uint64_t>() * 1000;
    if (duration != impl_->duration) {
        impl_->duration = duration;
        impl_->Retire(false);
    }
}
void ChordService::EngineReady() { impl_->EngineReady(); }
bool ChordService::Enabled() const { return impl_->Effective(); }
bool ChordService::Busy() const { return impl_->applying || impl_->queued_enable; }
const rimes::chord::Profile& ChordService::Profile() const { return impl_->active.profile; }
std::uint64_t ChordService::Revision() const { return impl_->revision; }
std::uint64_t ChordService::DurationMicros() const { return impl_->duration; }
std::string ChordService::SelectedSchema() const {
    const auto selected = impl_->state.value("selectedSchema", std::string{});
    return IsChordSchema(selected) && !Enabled() ? impl_->Fallback() : selected;
}
std::string ChordService::FallbackSchema() const { return impl_->Fallback(); }
void ChordService::ObserveSchema(const std::string& schema) {
    if (schema.empty() || impl_->applying || impl_->imported)
        return;
    if (IsChordSchema(schema) && (!Enabled() || schema != Profile().SchemaID()))
        return;
    if (impl_->state.value("selectedSchema", std::string{}) == schema &&
        impl_->state.value("lastOrdinarySchema", std::string{}) == schema)
        return;
    impl_->state["selectedSchema"] = schema;
    if (!IsChordSchema(schema))
        impl_->state["lastOrdinarySchema"] = schema;
    try {
        impl_->SaveState();
    } catch (...) {
    }
}
ChordService::Json ChordService::Command(const Json& request) { return impl_->Command(request); }
} // namespace fcitx
