#include "capsule_kinds.hpp"

#include <algorithm>
#include <cctype>

namespace rimes::capsule {
namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

}  // namespace

const char* KindRaw(Kind kind) {
    switch (kind) {
        case Kind::Note:
            return "note";
        case Kind::Image:
            return "image";
        case Kind::Pdf:
            return "pdf";
        case Kind::Video:
            return "video";
        case Kind::Skill:
            return "skill";
        case Kind::Resource: return "resource";
        case Kind::Password:
            return "password";
        case Kind::Retired:
            return "retired";
    }
    return "retired";
}

const char* KindLabel(Kind kind) {
    switch (kind) {
        case Kind::Note:
            return "笔记";
        case Kind::Image:
            return "图库";
        case Kind::Pdf:
            return "PDF";
        case Kind::Video:
            return "影集";
        case Kind::Skill:
            return "技能";
        case Kind::Resource: return "资源";
        case Kind::Password:
            return "密码";
        case Kind::Retired:
            return "";
    }
    return "";
}

Kind KindFromRaw(std::string_view raw) {
    const auto value = Lower(raw);
    if (value == "note") {
        return Kind::Note;
    }
    if (value == "image") {
        return Kind::Image;
    }
    if (value == "pdf") {
        return Kind::Pdf;
    }
    if (value == "video") {
        return Kind::Video;
    }
    if (value == "skill") {
        return Kind::Skill;
    }
    if (value == "resource") return Kind::Resource;
    if (value == "password") {
        return Kind::Password;
    }
    return Kind::Retired;
}

const char* TabRaw(Tab tab) {
    switch (tab) {
        case Tab::Recent:
            return "recent";
        case Tab::Captures:
            return "captures";
        case Tab::Note:
            return "note";
        case Tab::Image:
            return "image";
        case Tab::Video:
            return "video";
        case Tab::Pdf:
            return "pdf";
        case Tab::Skill:
            return "skill";
        case Tab::Resource: return "resource";
        case Tab::Password:
            return "password";
    }
    return "note";
}

const char* TabLabel(Tab tab) {
    switch (tab) {
        case Tab::Recent:
            return "临时";
        case Tab::Captures:
            return "捕获";
        case Tab::Note:
            return KindLabel(Kind::Note);
        case Tab::Image:
            return KindLabel(Kind::Image);
        case Tab::Video:
            return KindLabel(Kind::Video);
        case Tab::Pdf:
            return KindLabel(Kind::Pdf);
        case Tab::Skill:
            return KindLabel(Kind::Skill);
        case Tab::Resource: return KindLabel(Kind::Resource);
        case Tab::Password:
            return KindLabel(Kind::Password);
    }
    return KindLabel(Kind::Note);
}

Tab TabFromRaw(std::string_view raw) {
    const auto value = Lower(raw);
    if (value == "recent" || value == "临时") {
        return Tab::Recent;
    }
    if (value == "captures" || value == "捕获") {
        return Tab::Captures;
    }
    if (value == "note" || value == "笔记") {
        return Tab::Note;
    }
    if (value == "image" || value == "图库") {
        return Tab::Image;
    }
    if (value == "video" || value == "影集") {
        return Tab::Video;
    }
    if (value == "pdf") {
        return Tab::Pdf;
    }
    if (value == "skill" || value == "技能") {
        return Tab::Skill;
    }
    if (value == "resource") return Tab::Resource;
    if (value == "password" || value == "密码") {
        return Tab::Password;
    }
    return Tab::Note;
}

Tab CycleTab(Tab tab, int offset) {
    const int count = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));
    int index = 0;
    for (int i = 0; i < count; ++i) {
        if (kTabs[i] == tab) {
            index = i;
            break;
        }
    }
    int next = (index + offset) % count;
    if (next < 0) {
        next += count;
    }
    return kTabs[next];
}

Kind KindForTab(Tab tab) {
    switch (tab) {
        case Tab::Note:
            return Kind::Note;
        case Tab::Image:
            return Kind::Image;
        case Tab::Video:
            return Kind::Video;
        case Tab::Pdf:
            return Kind::Pdf;
        case Tab::Skill:
            return Kind::Skill;
        case Tab::Resource: return Kind::Resource;
        case Tab::Password:
            return Kind::Password;
        case Tab::Recent:
        case Tab::Captures:
            return Kind::Retired;
    }
    return Kind::Retired;
}

bool TabIsPorted(Tab tab) {
    return tab == Tab::Note;
}

std::string PreviewOf(std::string_view text) {
    std::string out;
    out.reserve(std::min(text.size(), static_cast<std::size_t>(kPreviewLimit)));
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
        if (static_cast<int>(out.size()) >= kPreviewLimit) {
            break;
        }
    }
    if (out.size() < text.size()) {
        while (!out.empty() && (static_cast<unsigned char>(text[out.size()]) & 0xc0) == 0x80) out.pop_back();
    }
    return out;
}

std::string CountText(int count) {
    if (count == 1) {
        return "1 ITEM";
    }
    return std::to_string(count) + " ITEMS";
}

}  // namespace rimes::capsule
