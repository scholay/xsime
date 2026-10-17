#include "capsule_hash.hpp"
#include "capsule_history.hpp"
#include "capsule_manager.hpp"
#include "capsule_rich_text.hpp"
#include "desktop.hpp"
#include "official_catalog.hpp"
#include "private_files.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace rimes::capsule;
using Json=nlohmann::json;
static void Check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
static GtkWidget* Find(GtkWidget* root,const char* name) {
    if(std::string_view(gtk_widget_get_name(root))==name) return root;
    if(!GTK_IS_CONTAINER(root)) return nullptr;
    auto* children=gtk_container_get_children(GTK_CONTAINER(root));GtkWidget* found=nullptr;
    for(auto* node=children;node && !found;node=node->next) found=Find(GTK_WIDGET(node->data),name);
    g_list_free(children);return found;
}
static void Wait(const std::function<bool()>& condition) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    do {
        while(gtk_events_pending()) gtk_main_iteration();
        if(condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while(std::chrono::steady_clock::now()<end);
    throw std::runtime_error("GTK condition timed out");
}
static std::string Text(GtkWidget* view) {
    auto* buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));GtkTextIter a{},b{};
    gtk_text_buffer_get_bounds(buffer,&a,&b);auto* raw=gtk_text_buffer_get_text(buffer,&a,&b,FALSE);
    std::string value=raw;g_free(raw);return value;
}
int main(int argc,char** argv) {
    const auto root=std::filesystem::temp_directory_path()/("xsime-capsule-manager-"+std::to_string(getpid()));
    try {
        std::filesystem::remove_all(root);setenv("RIMES_USER_DIR",root.c_str(),1);
        gtk_init(&argc,&argv);
        ContentStore store(ContentStore::DefaultRoot());std::string error;
        Check(store.SeedIfNeeded(&error)==StoreStatus::Ok,"seed failed");
        rimes::plugins::PackageStore packages(root/"plugins",rimes::plugins::OfficialCatalog(),rimes::plugins::BundledPackages(),
            rimes::plugins::CompiledAdapters(),RIMES_PRODUCT_VERSION);
        CapsuleModel model;model.set_visible(true);model.set_managing(true);
        std::vector<Record> records;store.ListAll(&records,&error);model.LoadCards(records);
        ManagerWindow* manager=nullptr;int refreshed=0;
        ManagerWindow window([&](const std::string& raw) {
            const auto command=Json::parse(raw);
            if(command.at("op")=="refresh") {
                ++refreshed;store.ListAll(&records,&error);model.LoadCards(records);manager->Apply(MakeSnapshot(model));
            }
        });manager=&window;window.Apply(MakeSnapshot(model));
        auto get=[&](const char* name){auto* widget=Find(window.Root(),name);Check(widget,"missing UI control");return widget;};
        auto* title=get("capsule-title");auto* body=get("capsule-body");auto* save=get("capsule-save");
        auto layout=[&](int width,int height) {
            if(gtk_widget_get_allocated_width(window.Root())!=width || gtk_widget_get_allocated_height(window.Root())!=height) return false;
            const int list_width=std::clamp(static_cast<int>(width*0.24),170,286);
            const int editor_x=142+list_width+27;
            auto bounds=[&](const char* name,int x,int y,int w,int h) {
                GtkAllocation rect{};gtk_widget_get_allocation(get(name),&rect);
                return rect.x==x && rect.y==y && rect.width==w && rect.height==h;
            };
            return bounds("capsule-modules",14,56,118,height-86) &&
                bounds("capsule-list",142,90,list_width,height-120) &&
                bounds("capsule-editor",editor_x,56,width-editor_x-14,height-86);
        };
        auto contents=[&](const char* value){gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(body)),value,-1);};
        Wait([&]{return std::string(gtk_entry_get_text(GTK_ENTRY(title)))==kDefaultEntryTitle;});
        Wait([&]{return layout(940,660);});
        gtk_entry_set_text(GTK_ENTRY(title),"保存标题");contents("saved body");gtk_button_clicked(GTK_BUTTON(save));
        Wait([&]{return refreshed==1 && Text(body)=="saved body";});
        gtk_window_resize(GTK_WINDOW(window.Window()),620,430);
        Wait([&]{return layout(620,430);});
        gtk_window_resize(GTK_WINDOW(window.Window()),940,660);
        Wait([&]{return layout(940,660);});
        Record saved;Check(store.Get(kDefaultEntryId,&saved,&error)==StoreStatus::Ok && saved.title=="保存标题" && saved.content=="saved body","GUI save did not persist");
        contents("unsaved draft");
        auto locked=MakeSnapshot(model);locked.password_field=true;locked.visible=false;window.Apply(locked);
        Check(!gtk_widget_get_visible(window.Window()) && Text(body).empty(),"protected window retained visible plaintext");
        window.Apply(MakeSnapshot(model));
        Check(Text(body)=="unsaved draft","protection lost unsaved draft");
        auto note=packages.Inspect("builtin.capsule.notes");packages.Enable(note.id,false,note.grant);
        gtk_button_clicked(GTK_BUTTON(save));
        Check(store.Get(saved.id,&saved,&error)==StoreStatus::Ok && saved.content=="saved body","disabled module wrote through stale UI");
        Check(Text(body)=="unsaved draft","revocation lost draft");
        note=packages.Inspect(note.id);packages.Enable(note.id,true,note.grant);
        gtk_button_clicked(GTK_BUTTON(save));Wait([&]{return refreshed==2 && Text(body)=="unsaved draft";});
        Check(store.Get(saved.id,&saved,&error)==StoreStatus::Ok && saved.content=="unsaved draft","current grant could not save");
        auto external=saved;external.content="external revision";Check(store.Put(external,saved.revision,&saved,&error)==StoreStatus::Ok,"external update");
        contents("conflicting local draft");gtk_button_clicked(GTK_BUTTON(save));
        Wait([&]{return std::string(gtk_label_get_text(GTK_LABEL(get("capsule-status")))).find("条目已被修改")!=std::string::npos;});
        Check(Text(body)=="conflicting local draft","conflict lost draft");
        Check(store.Get(saved.id,&saved,&error)==StoreStatus::Ok && saved.content=="external revision","conflict overwrote external edit");
        // A clean editor can start a new entry without a subsequent harmless
        // host refresh restoring the previously selected saved entry.
        {
            store.ListAll(&records,&error);model.LoadCards(records);
            ManagerWindow fresh([](const std::string&){});fresh.Apply(MakeSnapshot(model));
            auto* fresh_title=Find(fresh.Root(),"capsule-title");
            Wait([&]{return std::string(gtk_entry_get_text(GTK_ENTRY(fresh_title)))==saved.title;});
            gtk_button_clicked(GTK_BUTTON(Find(fresh.Root(),"capsule-new")));
            fresh.Apply(MakeSnapshot(model));
            Check(std::string(gtk_entry_get_text(GTK_ENTRY(fresh_title))).empty(),"host refresh discarded the new-note editor");
        }
        {
            std::vector<Record> many(400,saved);
            for(auto& record:many) record.id=NewUuidV4();
            CapsuleModel paged; paged.LoadCards(many); paged.set_visible(true); paged.set_managing(true);
            ManagerWindow* receiver=nullptr;
            ManagerWindow library([&](const std::string& raw) {
                auto command=Json::parse(raw);
                if(command.at("op")=="select") { paged.Select(command.at("index")); receiver->Apply(MakeSnapshot(paged)); }
            },true);receiver=&library;library.Apply(MakeSnapshot(paged));
            auto* scroll=Find(library.Root(),"capsule-list");
            auto* viewport=gtk_bin_get_child(GTK_BIN(scroll));
            auto* rows=gtk_bin_get_child(GTK_BIN(viewport));
            auto* children=gtk_container_get_children(GTK_CONTAINER(rows));
            gtk_widget_grab_focus(GTK_WIDGET(children->data));g_list_free(children);
            GdkEventKey key{};key.type=GDK_KEY_PRESS;key.keyval=GDK_KEY_End;gboolean handled=FALSE;
            g_signal_emit_by_name(library.Window(),"key-press-event",&key,&handled);
            Check(handled && paged.selected()==399,"manager could not navigate beyond the first transport window");
            key.keyval=GDK_KEY_Page_Up;handled=FALSE;
            g_signal_emit_by_name(library.Window(),"key-press-event",&key,&handled);
            Check(handled && paged.selected()==391,"manager lost list focus while replacing transport window");
        }
        {
            std::ifstream input(NATIVE_NOTES_FIXTURES);
            auto fixture = Json::parse(input).at("rich").at(1);
            Record rich;
            rich.title = "Native image";
            rich.note_format = "rich-text";
            rich.rich_asset = *Base64Decode(fixture.at("asset").get<std::string>());
            rich.content = fixture.at("markdown");
            Check(store.Put(rich, "", &rich, &error) == StoreStatus::Ok, "rich manager fixture");
            CapsuleModel rich_model;
            rich_model.set_visible(true);
            rich_model.set_managing(true);
            rich_model.LoadCards({rich});
            ManagerWindow* receiver = nullptr;
            int saved_count = 0;
            ManagerWindow editor([&](const std::string& raw) {
                if (Json::parse(raw).at("op") == "refresh") {
                    ++saved_count;
                    Check(store.Get(rich.id, &rich, &error) == StoreStatus::Ok, "rich reload");
                    rich_model.LoadCards({rich});
                    receiver->Apply(MakeSnapshot(rich_model));
                }
            });
            receiver = &editor;
            editor.Apply(MakeSnapshot(rich_model));
            auto* rich_title = Find(editor.Root(), "capsule-title");
            auto* rich_body = Find(editor.Root(), "capsule-body");
            auto* rich_save = Find(editor.Root(), "capsule-save");
            Wait([&] {
                return std::string(gtk_entry_get_text(GTK_ENTRY(rich_title))) == rich.title;
            });
            Check(gtk_widget_get_visible(Find(editor.Root(), "capsule-rich-tools")) &&
                      !gtk_widget_get_visible(Find(editor.Root(), "capsule-bullet-tools")),
                  "native format toolbar visibility");
            if (argc == 2) {
                gtk_window_set_focus(GTK_WINDOW(editor.Window()), nullptr);
                while (gtk_events_pending())
                    gtk_main_iteration();
                Check(rimes::desktop::Render(editor.Root(), argv[1]), "rich manager render");
            }
            gtk_entry_set_text(GTK_ENTRY(rich_title), "Rich draft");
            auto protected_state = MakeSnapshot(rich_model);
            protected_state.password_field = true;
            protected_state.visible = false;
            editor.Apply(protected_state);
            Check(Text(rich_body).empty(), "rich body concealed");
            editor.Apply(MakeSnapshot(rich_model));
            Check(std::string(gtk_entry_get_text(GTK_ENTRY(rich_title))) == "Rich draft",
                  "rich title draft restored");
            gtk_button_clicked(GTK_BUTTON(rich_save));
            Wait([&] { return saved_count == 1; });
            const auto original =
                ReadRichText(*Base64Decode(fixture.at("asset").get<std::string>()));
            auto captured = ReadRichText(rich.rich_asset);
            Check(captured && captured->PlainText() == original->PlainText() &&
                      captured->assets == original->assets &&
                      captured->Markdown() == original->Markdown(),
                  "rich manager save/protected restore lost native image");
            Wait([&] {
                return std::string(gtk_entry_get_text(GTK_ENTRY(rich_title))) == rich.title;
            });
            auto external = RenderMarkdown(rich);
            external.replace(external.rfind(rich.content), rich.content.size(),
                             "external Markdown projection");
            {
                std::ofstream output(rich.path);
                output << external;
            }
            Check(store.Get(rich.id, &rich, &error) == StoreStatus::Ok, "projection reload");
            rich_model.LoadCards({rich});
            editor.Apply(MakeSnapshot(rich_model));
            auto* importer = Find(editor.Root(), "capsule-import-projection");
            Wait([&] { return gtk_widget_is_visible(importer); });
            gtk_button_clicked(GTK_BUTTON(rich_save));
            Check(store.Get(rich.id, &rich, &error) == StoreStatus::Ok && rich.projection_conflict,
                  "GUI silently resolved projection conflict");
            g_idle_add(
                +[](gpointer) -> gboolean {
                    auto* windows = gtk_window_list_toplevels();
                    for (auto* node = windows; node; node = node->next)
                        if (GTK_IS_MESSAGE_DIALOG(node->data))
                            gtk_dialog_response(GTK_DIALOG(node->data), GTK_RESPONSE_ACCEPT);
                    g_list_free(windows);
                    return G_SOURCE_REMOVE;
                },
                nullptr);
            gtk_button_clicked(GTK_BUTTON(importer));
            gtk_button_clicked(GTK_BUTTON(rich_save));
            Wait([&] { return saved_count == 2; });
            Check(!rich.projection_conflict &&
                      ReadRichText(rich.rich_asset)->PlainText() == "external Markdown projection",
                  "explicit GUI import did not persist replacement");
        }
        {
            auto temporary = packages.Inspect("builtin.capsule.temporary");
            if (!temporary.enabled)
                packages.Enable(temporary.id, true, temporary.grant);
            ClipboardHistoryStore history(ClipboardHistoryStore::DefaultRoot());
            auto item = history.Capture({{{"text/plain;charset=utf-8", "历史完整内容\nsecond line"},
                                          {"text/html", "<b>历史完整内容</b>"}}},
                                        {"x11", "Fixture", "123"});
            Snapshot state;
            state.visible = true;
            state.managing = true;
            state.module = "temporary";
            state.modules = {Module::Temporary, Module::Notes};
            state.selected_id = item.id;
            state.count = 1;
            Card card;
            card.id = item.id;
            card.revision = item.revision;
            card.module = Module::Temporary;
            card.title = item.title;
            card.preview = "short preview";
            card.source = "Fixture";
            state.cards = {card};
            std::vector<Json> commands;
            ManagerWindow history_ui(
                [&](const std::string& raw) { commands.push_back(Json::parse(raw)); });
            history_ui.Apply(state);
            auto* detail = Find(history_ui.Root(), "capsule-clipboard-detail");
            auto* text = Find(history_ui.Root(), "capsule-clipboard-text");
            auto* copy = Find(history_ui.Root(), "capsule-clipboard-copy");
            Check(detail && text && copy, "missing history manager controls");
            Wait([&] { return Text(text) == "历史完整内容\nsecond line"; });
            Check(gtk_widget_get_visible(detail) &&
                      !gtk_text_view_get_editable(GTK_TEXT_VIEW(text)),
                  "history manager must be read only");
            gtk_button_clicked(GTK_BUTTON(copy));
            Check(commands.size() == 1 && commands.back().at("op") == "copy" &&
                      commands.back().at("revision") == item.revision,
                  "history copy lost identity/revision");
            Check(history.Favorite(item.id, item.revision, true), "favorite fixture");
            item = *history.Get(item.id);
            state.cards[0].revision = item.revision;
            history_ui.Apply(state);
            gtk_button_clicked(GTK_BUTTON(copy));
            Check(commands.back().at("revision") == item.revision,
                  "history manager copied stale revision");
            state.password_field = true;
            history_ui.Apply(state);
            Check(Text(text).empty() && !gtk_widget_get_visible(history_ui.Window()),
                  "protected manager exposed history text");
            gtk_button_clicked(GTK_BUTTON(copy));
            Check(commands.size() == 2, "concealed history accepted copy");
            state.password_field = false;
            history_ui.Apply(state);
            Wait([&] { return Text(text) == "历史完整内容\nsecond line"; });
            state.module = "notes";
            state.cards.clear();
            state.selected_id.clear();
            history_ui.Apply(state);
            Check(Text(text).empty() && !gtk_widget_get_visible(detail),
                  "switching modules retained clipboard plaintext");
        }
        {
            auto grant = packages.Inspect("builtin.capsule.resources");
            if (!grant.enabled) packages.Enable(grant.id, true, grant.grant);
            const auto file = root / "reference.pdf"; std::ofstream(file) << "%PDF-1.4\nfixture invalid for preview";
            Record reference; reference.kind=Kind::Pdf; reference.title="文献标题"; reference.content=file.string();
            reference.reference.fields={{"authors","张三; Alice"},{"year","2024"},{"publisher","示例出版社"}};
            Check(store.Put(reference,"",&reference,&error)==StoreStatus::Ok,"resource manager fixture");
            CapsuleModel resource_model; resource_model.EnableModule(Module::Resources,true); resource_model.set_module(Module::Resources);
            resource_model.set_visible(true); resource_model.set_managing(true); resource_model.LoadCards({reference});
            ManagerWindow* current=nullptr; int saved_count=0, copied_files=0; std::string copied_citation;
            ManagerWindow resource_ui([&](const std::string& raw) {
                const auto command=Json::parse(raw);
                if (command.at("op")=="refresh") {
                    ++saved_count; Check(store.Get(reference.id,&reference,&error)==StoreStatus::Ok,"read saved resource");
                    resource_model.LoadCards({reference}); current->Apply(MakeSnapshot(resource_model));
                }
            },false,[&](const Record& item) {Check(item.content==file.string(),"file copy lost chosen path"); ++copied_files;},
            [&](const std::string& text) {copied_citation=text;});
            current=&resource_ui; resource_ui.Apply(MakeSnapshot(resource_model));
            auto find=[&](const char* name) {auto* item=Find(resource_ui.Root(),name); Check(item,"missing resource control"); return item;};
            auto* authors=find("capsule-resource-authors"); auto* citation=find("capsule-resource-citation"); auto* path=find("capsule-resource-path");
            auto* import=find("capsule-resource-import"); auto* resource_save=find("capsule-save");
            Wait([&] {return std::string(gtk_entry_get_text(GTK_ENTRY(authors)))=="张三; Alice";});
            Check(gtk_widget_is_visible(find("capsule-resource-form")) && !gtk_widget_is_visible(find("capsule-body")),"resource uses note editor");
            Check(std::string(gtk_label_get_text(GTK_LABEL(citation)))==*FormatReference(reference.title,reference.reference,"gbt7714"),"resource citation preview drift");
            gtk_button_clicked(GTK_BUTTON(find("capsule-resource-citation-copy")));
            Check(copied_citation==*FormatReference(reference.title,reference.reference,"gbt7714"),"citation button copied wrong text");
            gtk_button_clicked(GTK_BUTTON(find("capsule-resource-copy"))); Check(copied_files==1,"resource file button disconnected");
            gtk_entry_set_text(GTK_ENTRY(authors),"Changed Author"); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(import),TRUE);
            auto protected_state=MakeSnapshot(resource_model); protected_state.password_field=true; resource_ui.Apply(protected_state);
            Check(std::string(gtk_entry_get_text(GTK_ENTRY(path))).empty() && !gtk_widget_get_visible(resource_ui.Window()),"resource path survived conceal");
            gtk_button_clicked(GTK_BUTTON(find("capsule-resource-copy"))); Check(copied_files==1,"protected resource copied file");
            resource_ui.Apply(MakeSnapshot(resource_model));
            Check(std::string(gtk_entry_get_text(GTK_ENTRY(authors)))=="Changed Author" && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(import)),"protection lost resource draft or attachment choice");
            gtk_button_clicked(GTK_BUTTON(resource_save));
            Wait([&] {return saved_count==1 && std::string(gtk_entry_get_text(GTK_ENTRY(authors)))=="Changed Author";});
            Check(reference.reference.Get("authors")=="Changed Author" && !reference.attachment.empty(),"resource save failed metadata/import");
            auto external=reference; external.title="External"; Check(store.Put(external,external.revision,&external,&error)==StoreStatus::Ok,"external reference edit");
            gtk_entry_set_text(GTK_ENTRY(authors),"Local Draft"); gtk_button_clicked(GTK_BUTTON(resource_save));
            Wait([&] {return std::string(gtk_label_get_text(GTK_LABEL(find("capsule-status")))).find("条目已被修改")!=std::string::npos;});
            Check(std::string(gtk_entry_get_text(GTK_ENTRY(authors)))=="Local Draft","stale resource save lost draft");
            // Unicode search includes reference metadata in the actual host model.
            resource_model.set_query("Changed Author"); Check(resource_model.cards().size()==1,"resource metadata search failed");
        }
        std::cout << "GTK manager save, protected draft recovery, clipboard detail, revocation and "
                     "concurrent-edit conflict passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';std::filesystem::remove_all(root);return 1;}
    std::filesystem::remove_all(root);return 0;
}
