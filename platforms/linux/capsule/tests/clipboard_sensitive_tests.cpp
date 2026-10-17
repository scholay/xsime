#include "clipboard_desktop.hpp"
#include "process.hpp"
#include <gtk/gtk.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace rimes::capsule;
using Clock=std::chrono::steady_clock;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){
 try {
    const bool wayland=std::getenv("WAYLAND_DISPLAY");if(!wayland)gtk_init(&argc,&argv);
    std::atomic<int> elapsed{0},captures{0};
    auto noop=[](auto,auto,auto){};auto status=[](auto){};
    DesktopClipboard owner(noop,status,[&]{return Clock::now()+std::chrono::seconds(elapsed.load());});
    DesktopClipboard other(noop,status);
    DesktopClipboard history([&](auto,auto,auto){++captures;},status);
    owner.Configure(false,false);other.Configure(false,false);history.Configure(true,false);
    auto spin=[&](int ms,const std::function<bool()>& done){const auto until=Clock::now()+std::chrono::milliseconds(ms);do{if(!wayland)while(gtk_events_pending())gtk_main_iteration();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(Clock::now()<until);return done();};
    Check(spin(5000,[&]{return owner.Available()&&other.Available()&&history.Available();}),"clipboard unavailable");
    auto copy=[&](DesktopClipboard& target,std::string text,bool sensitive){std::atomic<int> completed{0};Check(target.Restore(ClipboardArchive{{{"text/plain;charset=utf-8",text},{"UTF8_STRING",text}}},sensitive,[&](bool ok){completed=ok?1:-1;}),"copy queue rejected");Check(spin(4000,[&]{return completed.load()!=0;}),"copy completion timeout");Check(completed==1,"copy ownership failed");};
    auto read=[&](const char* type)->std::string {
        if(wayland){std::atomic<bool> cancel{false};return rimes::runtime::RunProcess({"/usr/bin/wl-paste","--no-newline","--type",type},"",cancel,std::chrono::seconds(3),65536).output;}
        auto* data=gtk_clipboard_wait_for_contents(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),gdk_atom_intern(type,FALSE));
        std::string value;if(data){const auto length=gtk_selection_data_get_length(data);if(length>0)value.assign(reinterpret_cast<const char*>(gtk_selection_data_get_data(data)),length);gtk_selection_data_free(data);}return value;
    };
    auto types=[&]()->std::string {
        if(wayland){std::atomic<bool> cancel{false};return rimes::runtime::RunProcess({"/usr/bin/wl-paste","--list-types"},"",cancel,std::chrono::seconds(3),65536).output;}
        GdkAtom* values=nullptr;gint count=0;std::string result;
        if(gtk_clipboard_wait_for_targets(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),&values,&count)){for(int i=0;i<count;++i){auto* name=gdk_atom_name(values[i]);result+=std::string(name)+"\n";g_free(name);}g_free(values);}return result;
    };
    copy(owner,"public-vault-fixture",true);
    Check(read("text/plain;charset=utf-8")=="public-vault-fixture","sensitive copy unavailable");
    const auto offered=types();for(const auto* marker:{"application/x-rimes-sensitive","org.nspasteboard.ConcealedType","org.nspasteboard.TransientType","x-kde-passwordManagerHint"})Check(offered.find(marker)!=offered.npos,"sensitive marker missing");
    Check(captures.load()==0,"secret entered history");
    elapsed=59;spin(250,[]{return false;});Check(read("text/plain;charset=utf-8")=="public-vault-fixture","secret expired early");
    elapsed=61;Check(spin(2500,[&]{return read("text/plain;charset=utf-8").empty();}),"secret was not cleared at expiry");
    for(int i=0;i<4;++i){copy(owner,"old-secret",true);copy(other,"newer-copy",false);elapsed.fetch_add(61);spin(1100,[]{return false;});Check(read("text/plain;charset=utf-8")=="newer-copy","expiry overwrote newer owner");}
    copy(owner,"shielded-secret",true);owner.Configure(false,true);
    Check(spin(2500,[&]{return read("text/plain;charset=utf-8").empty();}),"session protection retained secret clipboard");
    owner.Configure(false,false);copy(owner,"ordinary-copy",false);elapsed.fetch_add(90);spin(1100,[]{return false;});Check(read("text/plain;charset=utf-8")=="ordinary-copy","ordinary clipboard was expired");
    Check(captures.load()==0,"owned secret or ordinary restoration entered history");
    std::cout<<(wayland?"Wayland":"X11")<<": sensitive markers, actual paste, exact 60-second policy, newer-copy preservation, protection and ordinary-copy retention passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
