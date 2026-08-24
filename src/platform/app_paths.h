#pragma once

#include <filesystem>

namespace osss {

// The per-user data directory for this application: %LOCALAPPDATA%\OSSS on
// Windows, ~/Library/Application Support/OSSS on macOS, $XDG_DATA_HOME/OSSS
// (or ~/.local/share/OSSS) on Linux. An empty path means the platform could
// not determine one, which callers treat as "no persistent storage". The
// profile file and the shader bytecode cache live under here.
[[nodiscard]] std::filesystem::path ApplicationDataDirectory();

} // namespace osss
