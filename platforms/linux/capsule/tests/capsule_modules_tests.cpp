#include "capsule_protocol.hpp"
#include "capsule_hash.hpp"
#include "official_catalog.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rimes::capsule;
static void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
int main() {
    const auto root=std::filesystem::temp_directory_path()/("xsime-capsule-modules-"+std::to_string(getpid()));
    try {
        std::filesystem::remove_all(root);
        rimes::plugins::PackageStore packages(root,rimes::plugins::OfficialCatalog(),rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(),RIMES_PRODUCT_VERSION);
        CapsuleModel model;
        for(auto module:kModules) {
            const auto view=packages.Inspect(ModulePlugin(module));
            Check(view.available && view.enabled,"bundled module missing");
            Check(ValidFilter(module,"all"),"native default filter missing");
            model.EnableModule(module,true);
        }
        {
            CapsuleModel reenabled;
            for(auto module:kModules) reenabled.EnableModule(module,false);
            reenabled.EnableModule(Module::Resources,true);
            Check(reenabled.module()==Module::Resources && reenabled.ModuleEnabled(reenabled.module()),
                  "enabling first available module kept disabled selection");
            reenabled.EnableModule(Module::Resources,false);
            reenabled.EnableModule(Module::Resources,true);
            Check(reenabled.module()==Module::Resources && !reenabled.armed(),
                  "same-module re-enable revived insertion authority");
        }
        Check(ModuleDefinition(Module::Notes).at("title")=="笔记","native module title");
        Check(!ModuleFromRaw("unknown") && !ValidFilter(Module::Notes,"image"),"invalid module/filter");
        std::vector<Record> records;
        for(auto kind:{Kind::Note,Kind::Image,Kind::Video,Kind::Pdf,Kind::Skill,Kind::Resource}) {
            Record record;record.id=NewUuidV4();record.kind=kind;record.title=KindRaw(kind);record.content="内容 Mixed";
            record.revision="version1";record.note_format="bullet";record.resource_type="project";records.push_back(record);
        }
        model.LoadCards(records);model.set_module(Module::Resources);
        Check(model.cards().size()==3,"resource projection");model.set_filter("project");
        Check(model.cards().size()==1 && model.Selected()->kind==Kind::Resource,"resource filter");
        model.set_module(Module::Capture);model.set_filter("image");Check(model.cards().size()==1,"image filter");
        model.set_module(Module::Notes);model.set_query("内容 MIXED");Check(model.cards().size()==1,"token search");
        Check(model.Selected()->payload.empty(),"filtered snapshots copied complete note bodies");
        model.set_query("笔记");model.backspace_query();Check(model.query()=="笔","UTF-8 backspace");model.set_query("");
        auto added=records[0];added.id=NewUuidV4();records.insert(records.begin(),added);
        const auto selected=model.Selected()->id;model.LoadCards(records);
        Check(model.Selected()->id==selected,"reload retargeted selection");
        model.set_visible(true);model.set_token("target");model.set_armed(true);model.set_last_copied("clipboard");
        model.EnableModule(Module::Notes,false);
        Check(!model.armed() && model.last_copied().empty() && model.module()!=Module::Notes,"disable did not revoke insertion/copy");
        model.EnableModule(Module::Notes,true);model.LoadCards(records);model.set_module(Module::Notes);
        model.set_token("target");model.set_armed(true);model.set_managing(true);
        Check(!model.armed() && model.token().empty(),"manager retained client target");
        model.set_managing(false);Check(!model.armed(),"rail return silently rearmed");
        Card secret;secret.id=NewUuidV4();secret.kind=Kind::Password;secret.title="Account";secret.payload="never serialize";
        model.LoadModuleCards(Module::Passwords,{secret});model.set_module(Module::Passwords);
        Check(model.Selected()->payload.empty() && model.Selected()->preview=="••••••••","password projection leaked");
        Check(EncodeSnapshot(MakeSnapshot(model)).find("never serialize")==std::string::npos,"password wire leak");
        std::vector<Record> many;
        for(int i=0;i<300;++i) { auto record=records[0];record.id=NewUuidV4();many.push_back(record); }
        model.set_module(Module::Notes);model.LoadCards(many);model.Select(280);
        Snapshot decoded;const auto wire=EncodeSnapshot(MakeSnapshot(model));
        Check(DecodeSnapshot(wire,&decoded) && decoded.count==300 && decoded.cards.size()<=200 && decoded.selected_id==many[280].id,"bounded selected snapshot");
        Check(decoded.offset+decoded.selected==280,"transport lost absolute list position");
        auto concealed=MakeSnapshot(model);concealed.password_field=true;
        Check(DecodeSnapshot(EncodeSnapshot(concealed),&decoded) && decoded.count==0 && decoded.offset==0,"protected list retained its window offset");
        Command command;std::string error;
        Check(ParseCommand("{\"v\":1,\"op\":\"select\",\"id\":\""+many[280].id+"\"}",&command,&error),"identity selection");
        for(const auto* invalid:{R"({"v":1,"op":"module","text":"bad"})",R"({"v":1,"op":"copy","id":"../file"})",R"({"v":1,"op":"select","index":-2})",R"({"v":1,"op":"close","payload":"extra"})"})
            Check(!ParseCommand(invalid,&command,&error),"invalid command accepted");
        const auto original=packages.Inspect(ModulePlugin(Module::Notes));
        packages.Enable(original.id,false,original.grant);bool wrote=false;
        try { packages.WithEnabledGrant(original.id,original.grant,[&]{wrote=true;}); } catch(const std::exception&) {}
        Check(!wrote,"revoked job wrote");
        auto disabled=packages.Inspect(original.id);packages.Enable(disabled.id,true,disabled.grant);
        try { packages.WithEnabledGrant(original.id,original.grant,[&]{wrote=true;}); } catch(const std::exception&) {}
        Check(!wrote,"old job revived after re-enable");
        packages.WithEnabledGrant(original.id,packages.Inspect(original.id).grant,[&]{wrote=true;});
        Check(wrote,"current grant failed");
        std::filesystem::remove_all(root);
        std::cout<<"Capsule modules, filters, lifecycle, stable selection, bounded transport and revocation passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';std::filesystem::remove_all(root);return 1;
    }
}
