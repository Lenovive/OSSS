#include "app_profile.h"

#include "platform/app_paths.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace osss {
namespace {

std::wstring TrimWhitespace(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool EqualsIgnoringCase(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
        return std::towlower(a) == std::towlower(b);
    });
}

// A value needs quoting if it contains whitespace or would otherwise be read
// back as two tokens. Quoting more than necessary is harmless; quoting less
// silently changes the argument on the next load.
bool NeedsQuoting(const std::wstring& value) {
    return value.empty() ||
        value.find_first_of(L" \t\"") != std::wstring::npos;
}

} // namespace

bool TokenizeProfileLine(const std::wstring& line, std::vector<std::wstring>& tokens) {
    std::wstring current;
    bool in_token = false;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const wchar_t character = line[index];
        if (quoted) {
            if (character == L'"') {
                quoted = false;
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == L'"') {
            quoted = true;
            in_token = true;
            continue;
        }
        if (character == L' ' || character == L'\t') {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(character);
        in_token = true;
    }
    if (quoted) {
        return false;
    }
    if (in_token) {
        tokens.push_back(current);
    }
    return true;
}

ProfileParseResult ParseProfiles(const std::wstring& text) {
    ProfileParseResult result;
    std::wistringstream stream(text);
    std::wstring line;
    int line_number = 0;
    ProfileEntry* current = nullptr;

    while (std::getline(stream, line)) {
        ++line_number;
        const std::wstring trimmed = TrimWhitespace(line);
        if (trimmed.empty() || trimmed.front() == L'#' || trimmed.front() == L';') {
            continue;
        }
        if (trimmed.front() == L'[') {
            if (trimmed.back() != L']' || trimmed.size() <= 2) {
                result.error = L"Section header must look like [name.exe].";
                result.error_line = line_number;
                return result;
            }
            const std::wstring name = TrimWhitespace(trimmed.substr(1, trimmed.size() - 2));
            if (name.empty()) {
                result.error = L"Section header must name an executable.";
                result.error_line = line_number;
                return result;
            }
            // A repeated section appends rather than starting a second entry, so
            // a hand-edited file with the same game twice behaves the way it
            // reads instead of silently keeping only one.
            const auto existing = std::find_if(
                result.entries.begin(),
                result.entries.end(),
                [&name](const ProfileEntry& entry) {
                    return EqualsIgnoringCase(entry.executable, name);
                });
            if (existing != result.entries.end()) {
                current = &*existing;
            } else {
                result.entries.push_back(ProfileEntry{name, {}});
                current = &result.entries.back();
            }
            continue;
        }
        if (!current) {
            result.error = L"Arguments must follow a [name.exe] section header.";
            result.error_line = line_number;
            return result;
        }
        if (!TokenizeProfileLine(trimmed, current->arguments)) {
            result.error = L"Unterminated quote.";
            result.error_line = line_number;
            return result;
        }
    }
    return result;
}

std::wstring FormatProfiles(const std::vector<ProfileEntry>& entries) {
    std::wostringstream output;
    output << L"# OSSS per-application profiles.\n"
              L"#\n"
              L"# Each section is named for an executable and holds the command-line\n"
              L"# arguments to apply when that program is the capture target. Explicit\n"
              L"# arguments on the real command line override anything here.\n";
    for (const ProfileEntry& entry : entries) {
        output << L"\n[" << entry.executable << L"]\n";
        for (const std::wstring& argument : entry.arguments) {
            if (NeedsQuoting(argument)) {
                output << L'"' << argument << L'"';
            } else {
                output << argument;
            }
            output << L'\n';
        }
    }
    return output.str();
}

std::optional<std::vector<std::wstring>> FindProfileArguments(
    const std::vector<ProfileEntry>& entries,
    const std::wstring& executable) {
    if (executable.empty()) {
        return std::nullopt;
    }
    for (const ProfileEntry& entry : entries) {
        if (EqualsIgnoringCase(entry.executable, executable)) {
            return entry.arguments;
        }
    }
    return std::nullopt;
}

void SetProfileArguments(
    std::vector<ProfileEntry>& entries,
    const std::wstring& executable,
    std::vector<std::wstring> arguments) {
    for (ProfileEntry& entry : entries) {
        if (EqualsIgnoringCase(entry.executable, executable)) {
            entry.arguments = std::move(arguments);
            return;
        }
    }
    entries.push_back(ProfileEntry{executable, std::move(arguments)});
}

std::filesystem::path ProfilePath() {
    const std::filesystem::path root = ApplicationDataDirectory();
    if (root.empty()) {
        return {};
    }
    return root / "profiles.txt";
}

ProfileParseResult LoadProfiles() {
    ProfileParseResult result;
    const auto path = ProfilePath();
    if (path.empty()) {
        return result;
    }
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        // Never having written a profile is not an error.
        return result;
    }
    std::wifstream input(path);
    if (!input) {
        result.error = L"The profile file could not be opened for reading.";
        return result;
    }
    std::wostringstream buffer;
    buffer << input.rdbuf();
    return ParseProfiles(buffer.str());
}

bool SaveProfiles(const std::vector<ProfileEntry>& entries, std::wstring& error) {
    const auto path = ProfilePath();
    if (path.empty()) {
        error = L"No per-user application-data path is available.";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);

    // Write and rename, so an interrupted save cannot leave a truncated file
    // that fails to parse on the next start and takes every profile with it.
    const auto temporary = std::filesystem::path(path).concat(L".tmp");
    {
        std::wofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = L"The profile file could not be opened for writing.";
            return false;
        }
        output << FormatProfiles(entries);
        if (!output) {
            error = L"Writing the profile file failed.";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = L"Replacing the profile file failed.";
        return false;
    }
    return true;
}

} // namespace osss
