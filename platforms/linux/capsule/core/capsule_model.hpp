#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <map>

#include "capsule_kinds.hpp"
#include "capsule_store.hpp"

namespace rimes::capsule {

struct Card {
    std::string id;
    Kind kind = Kind::Note;
    std::string title;
    std::string preview;
    std::string payload;
    Module module = Module::Notes;
    std::string filter = "other";
    std::string revision;
    std::string attachment;
    bool external_search = false, favorite = false;
    std::string source;
};

class CapsuleModel {
public:
    void set_visible(bool value);
    bool visible() const { return visible_; }

    void set_armed(bool value);
    bool armed() const { return armed_; }

    void set_token(std::string token);
    const std::string& token() const { return token_; }
    bool arms(std::string_view token) const;

    void set_password_field(bool value);
    bool password_field() const { return password_field_; }

    void set_module(Module module);
    Module module() const { return module_; }
    void set_filter(std::string filter);
    const std::string& filter() const { return filter_; }
    void EnableModule(Module module, bool enabled);
    const std::set<Module>& enabled_modules() const { return enabled_modules_; }
    bool ModuleEnabled(Module module) const { return enabled_modules_.count(module); }
    void CycleModule(int offset);
    void set_managing(bool value);
    bool managing() const { return managing_; }
    void LoadModuleCards(Module module, std::vector<Card> cards);
    void set_loading(bool value) { loading_ = value; Touch(); }
    bool loading() const { return loading_; }
    void set_tab(Tab tab);
    Tab tab() const { return tab_; }

    void set_query(std::string query);
    void append_query(char ch);
    void backspace_query();
    const std::string& query() const { return query_; }

    void LoadCards(const std::vector<Record>& records);
    const std::vector<Card>& cards() const { return cards_; }
    const Card* Selected() const;
    void Select(int index);
    void Move(int delta);
    int selected() const { return selected_; }

    void set_last_copied(std::string text);
    const std::string& last_copied() const { return last_copied_; }
    std::uint64_t copy_seq() const { return copy_seq_; }

    void set_hint(std::string hint);
    const std::string& hint() const { return hint_; }

    std::uint64_t generation() const { return generation_; }
    void Touch();

    void Disarm(std::string_view reason);
    void Hide();

private:
    void ApplyFilter();
    void RefreshHint();

    Module module_ = Module::Notes;
    std::string filter_ = "all";
    std::set<Module> enabled_modules_{Module::Notes};
    bool managing_ = false;
    bool loading_ = false;
    bool visible_ = false;
    bool armed_ = false;
    bool password_field_ = false;
    std::string token_;
    Tab tab_ = Tab::Note;
    std::string query_;
    std::vector<Card> all_;
    std::vector<Card> cards_;
    int selected_ = 0;
    std::string last_copied_;
    std::uint64_t copy_seq_ = 0;
    std::string hint_;
    std::uint64_t generation_ = 0;
};

}  // namespace rimes::capsule
