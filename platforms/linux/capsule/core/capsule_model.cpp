#include "capsule_model.hpp"
#include "capsule_structure.hpp"
#include "capsule_text.hpp"

#include <algorithm>
#include <cctype>

namespace rimes::capsule {
namespace {

bool MatchesQuery(const Card& card, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    const auto haystack = LowerText(card.title + "\n" + card.payload + "\n" + card.source + "\n" + card.preview);
    const auto needle = OneLine(LowerText(query));
    std::size_t start = 0;
    while (start < needle.size()) {
        while (start < needle.size() &&
               std::isspace(static_cast<unsigned char>(needle[start])) != 0) {
            ++start;
        }
        if (start >= needle.size()) {
            break;
        }
        std::size_t end = start;
        while (end < needle.size() &&
               std::isspace(static_cast<unsigned char>(needle[end])) == 0) {
            ++end;
        }
        if (haystack.find(needle.substr(start, end - start)) == std::string::npos) {
            return false;
        }
        start = end;
    }
    return true;
}

}  // namespace

void CapsuleModel::set_visible(bool value) {
    if (visible_ == value) {
        return;
    }
    visible_ = value;
    Touch();
}

void CapsuleModel::set_armed(bool value) {
    if (armed_ == value) {
        return;
    }
    armed_ = value;
    if (!armed_) {
        token_.clear();
    }
    Touch();
}

void CapsuleModel::set_token(std::string token) {
    if (token_ == token) {
        return;
    }
    token_ = std::move(token);
    Touch();
}

bool CapsuleModel::arms(std::string_view token) const {
    return armed_ && !token_.empty() && token_ == token;
}

void CapsuleModel::set_password_field(bool value) {
    if (password_field_ == value) {
        return;
    }
    password_field_ = value;
    if (password_field_) {
        Disarm("password");
        set_visible(false);
    }
    Touch();
}

void CapsuleModel::set_module(Module module) {
    if (!ModuleEnabled(module) || module_ == module) return;
    module_ = module; filter_ = "all"; query_.clear(); last_copied_.clear(); selected_ = 0;
    switch (module) {
    case Module::Temporary: tab_ = Tab::Recent; break;
    case Module::Capture: tab_ = Tab::Captures; break;
    case Module::Notes: tab_ = Tab::Note; break;
    case Module::Resources: tab_ = Tab::Pdf; break;
    case Module::Passwords: tab_ = Tab::Password; break;
    }
    ApplyFilter(); Touch();
}
void CapsuleModel::set_filter(std::string filter) {
    if (!ValidFilter(module_, filter) || filter_ == filter) return;
    filter_ = std::move(filter); selected_ = 0; ApplyFilter(); Touch();
}
void CapsuleModel::EnableModule(Module module, bool enabled) {
    if (enabled) {
        enabled_modules_.insert(module);
        if (!ModuleEnabled(module_)) set_module(module);
    }
    else {
        enabled_modules_.erase(module);
        // Stop exposing disabled plaintext; persistent stores remain untouched.
        all_.erase(std::remove_if(all_.begin(), all_.end(), [module](const auto& card) { return card.module == module; }), all_.end());
        if (module_ == module) {
            Disarm("module-disabled"); query_.clear(); last_copied_.clear(); filter_ = "all";
            if (!enabled_modules_.empty()) set_module(*enabled_modules_.begin());
        }
    }
    ApplyFilter(); Touch();
}
void CapsuleModel::CycleModule(int offset) {
    if (enabled_modules_.empty()) return;
    std::vector<Module> modules(enabled_modules_.begin(), enabled_modules_.end());
    const auto found = std::find(modules.begin(), modules.end(), module_);
    const int current = found == modules.end() ? 0 : static_cast<int>(found - modules.begin());
    const int count = static_cast<int>(modules.size());
    set_module(modules[(current + offset % count + count) % count]);
}
void CapsuleModel::set_managing(bool value) {
    if (managing_ == value) return;
    managing_ = value;
    Disarm("view-change"); Touch();
}
void CapsuleModel::set_tab(Tab tab) {
    // Older clients keep their type tabs; current UI uses five modules.
    switch (tab) {
    case Tab::Recent: set_module(Module::Temporary); break;
    case Tab::Captures: set_module(Module::Capture); break;
    case Tab::Note: set_module(Module::Notes); break;
    case Tab::Image: set_module(Module::Capture); set_filter("image"); break;
    case Tab::Video: set_module(Module::Capture); set_filter("video"); break;
    case Tab::Pdf: set_module(Module::Resources); set_filter("reference"); break;
    case Tab::Skill: set_module(Module::Resources); set_filter("skills"); break;
    case Tab::Resource: set_module(Module::Resources); break;
    case Tab::Password: set_module(Module::Passwords); break;
    }
}

void CapsuleModel::set_query(std::string query) {
    if (query_ == query) {
        return;
    }
    query_ = std::move(query);
    selected_ = 0;
    ApplyFilter();
    Touch();
}

void CapsuleModel::append_query(char ch) {
    if (ch < 0x20 || ch > 0x7e) {
        return;
    }
    query_.push_back(ch);
    selected_ = 0;
    ApplyFilter();
    Touch();
}

void CapsuleModel::backspace_query() {
    if (query_.empty()) {
        return;
    }
    auto end = query_.size() - 1;
    while (end && (static_cast<unsigned char>(query_[end]) & 0xc0) == 0x80) --end;
    query_.erase(end);
    selected_ = 0;
    ApplyFilter();
    Touch();
}

void CapsuleModel::LoadCards(const std::vector<Record>& records) {
    const std::string selected_id = Selected() ? Selected()->id : "";
    // Clipboard/password backends supply metadata through LoadModuleCards.
    all_.erase(std::remove_if(all_.begin(), all_.end(), [](const auto& card) {
        return card.module != Module::Temporary && card.module != Module::Passwords;
    }), all_.end());
    for (const auto& record : records) {
        const auto module = RecordModule(record);
        if (!ModuleEnabled(module) || record.kind == Kind::Password) continue;
        Card card;
        card.id = record.id; card.kind = record.kind; card.title = record.title;
        card.payload = record.content + (record.kind == Kind::Pdf ? record.reference.SearchText() : ""); card.module = module; card.filter = RecordFilter(record);
        card.revision = record.revision; card.attachment = record.attachment;
        card.preview = record.summary.empty() ? NoteSummary(record.content) : record.summary;
        if (record.kind == Kind::Pdf && record.summary.empty()) {
            const auto& reference = record.reference;
            bool empty = true; for (const auto* key : kReferenceFields) if (!TrimText(reference.Get(key)).empty()) empty = false;
            if (empty) card.preview = "待补书目信息 · " + std::filesystem::path(record.content).filename().string();
            else {
                card.preview.clear();
                const auto kind = reference.kind == "book" ? "图书" : reference.kind == "journalArticle" ? "期刊论文" : reference.kind == "conferencePaper" ? "会议论文" : "报告";
                for (const auto& part : {reference.Get("authors"), reference.Get("year"), std::string(kind)}) if (!part.empty()) {if(!card.preview.empty()) card.preview += " · "; card.preview += part;}
            }
        } else if (record.kind == Kind::Resource && record.summary.empty()) card.preview = "本机资源 · " + std::filesystem::path(record.content).filename().string();
        all_.push_back(std::move(card));
    }
    ApplyFilter();
    const auto selected = std::find_if(cards_.begin(), cards_.end(), [&](const auto& card) { return card.id == selected_id; });
    if (selected != cards_.end()) selected_ = static_cast<int>(selected - cards_.begin());
    Touch();
}
void CapsuleModel::LoadModuleCards(Module module, std::vector<Card> cards) {
    const std::string selected_id = Selected() ? Selected()->id : "";
    all_.erase(std::remove_if(all_.begin(), all_.end(), [module](const auto& card) { return card.module == module; }), all_.end());
    if (ModuleEnabled(module)) for (auto& card : cards) {
        card.module = module;
        if (module == Module::Passwords) { card.payload.clear(); card.preview = "••••••••"; }
        all_.push_back(std::move(card));
    }
    ApplyFilter();
    const auto selected = std::find_if(cards_.begin(), cards_.end(), [&](const auto& card) { return card.id == selected_id; });
    if (selected != cards_.end()) selected_ = static_cast<int>(selected - cards_.begin());
    Touch();
}

const Card* CapsuleModel::Selected() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(cards_.size())) {
        return nullptr;
    }
    return &cards_[static_cast<std::size_t>(selected_)];
}

void CapsuleModel::Select(int index) {
    if (cards_.empty()) {
        selected_ = 0;
        Touch();
        return;
    }
    selected_ = std::clamp(index, 0, static_cast<int>(cards_.size()) - 1);
    Touch();
}

void CapsuleModel::Move(int delta) {
    Select(selected_ + delta);
}

void CapsuleModel::set_last_copied(std::string text) {
    last_copied_ = std::move(text);
    ++copy_seq_;
    Touch();
}

void CapsuleModel::set_hint(std::string hint) {
    hint_ = std::move(hint);
    Touch();
}

void CapsuleModel::Touch() {
    ++generation_;
    RefreshHint();
}

void CapsuleModel::Disarm(std::string_view /*reason*/) {
    if (!armed_ && token_.empty()) {
        return;
    }
    armed_ = false;
    token_.clear();
    Touch();
}

void CapsuleModel::Hide() {
    visible_ = false;
    armed_ = false;
    token_.clear();
    query_.clear();
    last_copied_.clear();
    ApplyFilter();
    Touch();
}

void CapsuleModel::ApplyFilter() {
    cards_.clear();
    if (!ModuleEnabled(module_)) {
        selected_ = 0;
        return;
    }
    for (const auto& card : all_) {
        if (card.module == module_ && (filter_ == "all" || card.filter == filter_) &&
            (card.external_search || MatchesQuery(card, query_))) {
            // Filtered cards are projections. Full bodies stay in the search
            // index and are re-read by revision before explicit delivery.
            Card projected;
            projected.id=card.id; projected.kind=card.kind; projected.title=card.title; projected.preview=card.preview;
            projected.module=card.module; projected.filter=card.filter; projected.revision=card.revision; projected.attachment=card.attachment;
            projected.external_search = card.external_search;
            projected.favorite = card.favorite;
            projected.source = card.source;
            cards_.push_back(std::move(projected));
        }
    }
    if (selected_ >= static_cast<int>(cards_.size())) {
        selected_ = cards_.empty() ? 0 : static_cast<int>(cards_.size()) - 1;
    }
}

void CapsuleModel::RefreshHint() {
    if (password_field_) {
        hint_ = "Protected — Capsule will not insert here";
        return;
    }
    if (!ModuleEnabled(module_)) {
        hint_ = "请在插件管理中启用 Capsule 模块";
        return;
    }
    if (loading_) { hint_ = "正在读取 Capsule…"; return; }
    if (managing_) { hint_ = module_==Module::Capture ? "管理捕获 · Esc 返回底栏" : "Ctrl+S SAVE   ESC BACK TO RAIL"; return; }
    if (!armed_) {
        hint_ = "Disarmed — Ctrl+Shift+V on this field re-arms; press again to close";
        return;
    }
    if (cards_.empty()) {
        hint_ = query_.empty() ? "暂无" + ModuleDefinition(module_).at("title").get<std::string>() + "条目" : "没有匹配的条目";
        return;
    }
    hint_ = Selected() && Selected()->kind == Kind::Note ? "Return 插入 · Ctrl+C 复制 · Esc 关闭" : (module_ == Module::Resources ? "Return 复制文件 · 双击管理 · Esc 关闭" : "当前条目不支持文本插入 · Esc 关闭");
}

}  // namespace rimes::capsule
