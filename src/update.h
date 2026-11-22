#pragma once
#include "worker.h"
#include <array>
#include <optional>
#include <string_view>

// The update notice: asks GitHub for the latest release and compares it with this build. It never downloads or
// installs anything.
namespace mech::update {
using Version = std::array<int, 3>; // Major, minor, patch.
inline constexpr wchar_t releasePage[] = L"https://github.com/Liddo-kun/MechControl-RE/releases/latest";
// "v3.1.0", "3.1.0" or "3.1"; anything else, such as a pre-release suffix, is not a version.
std::optional<Version> parse(std::string_view tag);
std::wstring text(const Version& v); // "3.1.0"
Version current();                   // This build's version, from CMake.
// The tag_name of GitHub's latest-release reply.
std::string latestTag(std::string_view reply);
// Asks on a thread of its own, which posts ResultKind::update with the tag or ResultKind::updateFailed with what
// failed, for the log. Nothing waits for it: the delivery drops a reply that comes after the window closed.
void check(std::shared_ptr<Delivery> delivery);
}
