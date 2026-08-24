#include "window_catalog.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace osss {
namespace {

// Byte-level ASCII case folding. The titles and executable names that reach
// the matcher are UTF-8, and the case that matters for `--title` matching is
// the ASCII one; towlower would only add a locale dependency the core should
// not have, and is not available without a Windows-wide code page on the
// non-Windows builds.
std::string Lowercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

} // namespace

std::vector<WindowEntry> SelectWindowsMatching(
    const std::vector<WindowEntry>& candidates,
    const std::string& fragment) {
    const std::string needle = Lowercase(fragment);
    if (needle.empty()) {
        return {};
    }
    std::vector<WindowEntry> by_executable;
    std::vector<WindowEntry> by_title;
    for (const WindowEntry& entry : candidates) {
        if (Lowercase(entry.process_name).find(needle) != std::string::npos) {
            by_executable.push_back(entry);
        } else if (Lowercase(entry.title).find(needle) != std::string::npos) {
            by_title.push_back(entry);
        }
    }
    // See the header: an executable match wins outright, so a shell whose title
    // happens to contain the command cannot make an unambiguous request
    // ambiguous. Title matches are the fallback, not a peer.
    return by_executable.empty() ? by_title : by_executable;
}

} // namespace osss
