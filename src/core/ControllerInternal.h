#pragma once

#include <string>

// Helpers shared between the controller's translation units; not part of the public controller API.
namespace krkrspeed::controller::detail {

std::string toUtf8(const std::wstring &wstr);
std::wstring fromUtf8(const std::string &str);
std::string trimCopy(const std::string &s);
std::wstring toLowerCopy(const std::wstring &input);

// Reloads process_blacklist.txt if it changed on disk. Call once per process enumeration.
void refreshProcessBlacklist();
// Lookup against the last loaded blacklist; never touches the disk.
bool isProcessBlacklisted(const std::wstring &exeName);

} // namespace krkrspeed::controller::detail
