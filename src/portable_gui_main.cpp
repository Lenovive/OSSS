#include "window_catalog.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

int RunPipeline(const osss::WindowHandle target, const char* launcher_path) {
#if defined(__unix__) || defined(__APPLE__)
    std::string executable = "osss";
    if (launcher_path && (std::string(launcher_path).find('/') != std::string::npos)) {
        executable = (std::filesystem::path(launcher_path).parent_path() / "osss").string();
    }
    std::vector<std::string> values{
        executable,
        "--hwnd",
        std::to_string(static_cast<std::uint64_t>(target.Native()))};
    std::vector<char*> arguments;
    arguments.reserve(values.size() + 1U);
    for (std::string& value : values) {
        arguments.push_back(value.data());
    }
    arguments.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        return 1;
    }
    if (child == 0) {
        execvp(arguments.front(), arguments.data());
        std::perror("execvp osss");
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::perror("waitpid");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#else
    (void)target;
    (void)launcher_path;
    std::cerr << "portable launcher is unavailable on this platform\n";
    return 1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "OSSS portable launcher\n\n";
    const auto windows = osss::ListCapturableWindows();
    if (windows.empty()) {
        std::cerr << "No capturable windows were found. Use osss --list-windows for diagnostics.\n";
        return 1;
    }
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto& window = windows[index];
        std::cout << (index + 1U) << ". " << window.process_name;
        if (!window.title.empty()) {
            std::cout << " — " << window.title;
        }
        std::cout << "\n";
    }
    std::cout << "Select a window number (blank to quit): ";
    std::string input;
    if (!std::getline(std::cin, input) || input.empty()) {
        return 0;
    }
    char* end = nullptr;
    errno = 0;
    const auto selected = std::strtoul(input.c_str(), &end, 10);
    if (errno != 0 || end == input.c_str() || *end != '\0' ||
        selected == 0 || selected > windows.size()) {
        std::cerr << "Invalid window selection.\n";
        return 2;
    }
    return RunPipeline(windows[selected - 1U].handle, argc > 0 ? argv[0] : nullptr);
}
