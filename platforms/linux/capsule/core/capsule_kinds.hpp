#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rimes::capsule {

inline constexpr const char* kDefaultEntryId = "72696d65-7300-4000-8000-000000000001";
inline constexpr const char* kDefaultEntryTitle = "RIMES 默认词条";
inline constexpr const char* kDefaultEntryContent = "RIMES";
inline constexpr int kPreviewLimit = 280;
inline constexpr int kMaxTitleChars = 256;
inline constexpr int kMaxContentChars = 256 * 1024;
inline constexpr int kMaxDetailRows = kMaxContentChars + 20000;
inline constexpr int kMaxRecords = 20000;
inline constexpr int kMaxDocumentBytes = 8 * 1024 * 1024;
inline constexpr int kRailWidth = 940;
inline constexpr int kRailHeight = 208;

enum class Kind {
    Note,
    Image,
    Pdf,
    Video,
    Skill,
    Resource,
    Password,
    Retired,
};

enum class Tab {
    Recent,
    Captures,
    Note,
    Image,
    Video,
    Pdf,
    Skill,
    Resource,
    Password,
};

inline constexpr Tab kTabs[] = {
    Tab::Recent, Tab::Captures, Tab::Note, Tab::Image,
    Tab::Video,  Tab::Pdf,      Tab::Skill, Tab::Password,
};

const char* KindRaw(Kind kind);
const char* KindLabel(Kind kind);
Kind KindFromRaw(std::string_view raw);

const char* TabRaw(Tab tab);
const char* TabLabel(Tab tab);
Tab TabFromRaw(std::string_view raw);
Tab CycleTab(Tab tab, int offset);
Kind KindForTab(Tab tab);
bool TabIsPorted(Tab tab);

std::string PreviewOf(std::string_view text);
std::string CountText(int count);

}  // namespace rimes::capsule
