#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace osss {

// Per-application settings, stored as argument lists.
//
// A profile is deliberately *not* a struct mirroring `Options`. It is the
// command line you would have typed, keyed by executable name. That choice is
// the whole design:
//
//   * one parsing path. Every flag is validated by the same branch that
//     validates it on the real command line, so a profile cannot express a
//     setting the CLI rejects, and cannot silently accept one it spells wrong.
//   * nothing to keep in sync. A flag added to main.cpp works in profiles the
//     day it lands, with no serialisation code to update and no chance of a
//     field being read but never written.
//   * a readable file. Users edit this by hand, and `--flow-scale quality` needs
//     no explanation that a key/value pair would.
//
// The file lives beside the shader cache in %LOCALAPPDATA%\OSSS and looks like:
//
//     # Lines starting with # are comments.
//     [witcher3.exe]
//     --target-fps 240 --max-multiplier 4
//     --flow-scale quality
//
//     [vlc.exe]
//     --target-fps 120 --ui-mask "0,0.9,1,1"
//
// Section names match `WindowEntry::process_name` case-insensitively. Arguments
// accumulate across every line of a section, so they can be split however reads
// best. Values may be double-quoted, which is what `--ui-mask` needs.
struct ProfileEntry {
    std::wstring executable;
    std::vector<std::wstring> arguments;
};

struct ProfileParseResult {
    std::vector<ProfileEntry> entries;
    // Empty when parsing succeeded. A malformed file is reported rather than
    // partially applied: silently dropping half a profile would look like the
    // settings simply not working.
    std::wstring error;
    int error_line = 0;

    [[nodiscard]] bool Ok() const noexcept {
        return error.empty();
    }
};

// Splits one line into arguments, honouring double quotes so a value containing
// spaces or semicolons survives. An unterminated quote is an error.
[[nodiscard]] bool TokenizeProfileLine(
    const std::wstring& line,
    std::vector<std::wstring>& tokens);

[[nodiscard]] ProfileParseResult ParseProfiles(const std::wstring& text);

// Round-trips through ParseProfiles. Sections are emitted in the order given.
[[nodiscard]] std::wstring FormatProfiles(const std::vector<ProfileEntry>& entries);

// Case-insensitive match on the executable file name. Returns nullopt when no
// section names it.
[[nodiscard]] std::optional<std::vector<std::wstring>> FindProfileArguments(
    const std::vector<ProfileEntry>& entries,
    const std::wstring& executable);

// Inserts or replaces one section, leaving every other section untouched.
void SetProfileArguments(
    std::vector<ProfileEntry>& entries,
    const std::wstring& executable,
    std::vector<std::wstring> arguments);

// %LOCALAPPDATA%\OSSS\profiles.txt. Empty when no per-user application-data
// path is available, which disables profiles without disabling anything else.
[[nodiscard]] std::filesystem::path ProfilePath();

// Reads and parses ProfilePath(). A missing file is success with no entries --
// having never written a profile is not an error.
[[nodiscard]] ProfileParseResult LoadProfiles();

// Writes atomically through a temporary file and a rename, so an interrupted
// save cannot leave a half-written profile that fails to parse next start.
[[nodiscard]] bool SaveProfiles(const std::vector<ProfileEntry>& entries, std::wstring& error);

} // namespace osss
