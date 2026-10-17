#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "capsule_kinds.hpp"
#include "capsule_modules.hpp"
#include "capsule_structure.hpp"
#include "capsule_reference.hpp"
#include <map>

namespace rimes::capsule {

struct Record {
    std::string id;
    Kind kind = Kind::Note;
    std::string title;
    std::string content;
    std::string updated_at;
    std::string revision;
    std::filesystem::path path;
    std::string summary;
    std::string note_format = "other";
    std::string resource_type = "other";
    Reference reference;
    // Preserve unrecognized Markdown metadata on edits and imports.
    std::vector<std::string> extra_header;
    std::string attachment;
    std::vector<DetailRow> header_fields;
    std::string rich_asset;
    bool projection_conflict = false;
    std::string format_issue;
    // A caller may set this only after explicitly importing the edited
    // Markdown projection. It is never persisted or inferred from a read.
    bool resolve_projection = false;

};

enum class StoreStatus {
    Ok,
    NotFound,
    RevisionConflict,
    Invalid,
    Io,
};

class ContentStore {
public:
    explicit ContentStore(std::filesystem::path root);

    static std::filesystem::path DefaultRoot();

    const std::filesystem::path& root() const { return root_; }

    StoreStatus SeedIfNeeded(std::string* error);
    StoreStatus List(Kind kind, std::vector<Record>* out, std::string* error) const;
    StoreStatus ListAll(std::vector<Record>* out, std::string* error) const;
    // Immutable content-addressed assets; record deletion never deletes assets.
    StoreStatus ImportAttachment(const std::filesystem::path& source, std::string* asset, std::string* error);
    StoreStatus ReadAttachment(std::string_view asset, std::string* bytes, std::string* error) const;
    StoreStatus Get(std::string_view id, Record* out, std::string* error) const;
    StoreStatus Put(const Record& request, std::string expected_revision, Record* out,
                    std::string* error);
    StoreStatus Remove(std::string_view id, std::string_view expected_revision,
                       std::string* error);

private:
    StoreStatus WithLock(const std::function<StoreStatus()>& body, std::string* error) const;
    StoreStatus ReadAll(std::vector<Record>* out, std::string* error) const;
    StoreStatus ParseFile(const std::filesystem::path& path, Record* out,
                          std::string* error) const;
    StoreStatus WriteRecord(const Record& record, std::string* error) const;
    std::filesystem::path EntryPath(std::string_view id) const;

    std::filesystem::path root_;
    std::filesystem::path entries_;
    std::filesystem::path seed_;
    std::filesystem::path lock_;
};

Module RecordModule(const Record& record);
std::string RecordFilter(const Record& record);

std::string RenderMarkdown(const Record& record);
bool ParseMarkdown(std::string_view text, Record* out, std::string* error);
std::string JsonQuote(std::string_view text);
bool JsonUnquote(std::string_view text, std::string* out);

}  // namespace rimes::capsule
