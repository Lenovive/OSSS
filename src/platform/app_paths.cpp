#include "platform/app_paths.h"

#include <cstdlib>

#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace osss {

#if defined(_WIN32)

std::filesystem::path ApplicationDataDirectory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) {
        if (raw) {
            CoTaskMemFree(raw);
        }
        return {};
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    path /= L"OSSS";
    return path;
}

#elif defined(__APPLE__)

std::filesystem::path ApplicationDataDirectory() {
    const char* home = std::getenv("HOME");
    if (!home) {
        return {};
    }
    return std::filesystem::path(home) / "Library" / "Application Support" / "OSSS";
}

#else

std::filesystem::path ApplicationDataDirectory() {
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && data_home[0] != '\0') {
        return std::filesystem::path(data_home) / "OSSS";
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        return {};
    }
    return std::filesystem::path(home) / ".local" / "share" / "OSSS";
}

#endif

} // namespace osss
