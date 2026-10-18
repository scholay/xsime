#pragma once
#include "mailbox_store.hpp"
#include "capsule_store.hpp"
namespace rimes::mailbox {
bool SaveToCapsule(Store& store, const std::string& thread, const std::string& revision,
                   const std::string& message, capsule::ContentStore& capsule);
void ExportImage(Store& store, const std::string& thread, const std::string& revision,
                 const std::string& message, const std::filesystem::path& destination,
                 bool overwrite);
}
