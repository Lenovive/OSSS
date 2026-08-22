// Target-window selection: which windows a --title fragment resolves to.
//
// The desktop-dependent half of window_catalog (enumeration, DPI, refresh-rate
// paths) is not testable here. The ranking is, and it is where the bug was:
// a terminal's window title is the command line running in it, so launching
// OSSS from a shell put that shell in the candidate set under the exact name
// the user had typed, and `--title osss_test_animation` aborted as ambiguous
// between the animation and the terminal that started it.

#include "test_harness.h"
#include "window_catalog.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using osss::test::Require;

osss::WindowEntry Entry(const wchar_t* process_name, const wchar_t* title) {
    osss::WindowEntry entry;
    entry.process_name = process_name;
    entry.title = title;
    return entry;
}

// The exact desktop state that produced the ambiguity, as reported by
// --list-windows at the time: the animation window, and the Windows Terminal
// whose title was the command line that launched it.
std::vector<osss::WindowEntry> TerminalCollisionDesktop() {
    return {
        Entry(
            L"osss_test_animation.exe",
            L"OSSS Test Animation | Direct3D 11 | 60 FPS | S score | B burst"),
        Entry(
            L"WindowsTerminal.exe",
            L"C:\\Users\\user\\Desktop\\OSSS\\out\\release\\osss_test_animation.exe"),
    };
}

void TestExecutableMatchBeatsATitleMatch() {
    const auto matches =
        osss::SelectWindowsMatching(TerminalCollisionDesktop(), L"osss_test_animation");
    Require(
        matches.size() == 1,
        "A fragment naming an executable must resolve to one window, not " +
            std::to_string(matches.size()) + ".");
    Require(
        matches.front().process_name == L"osss_test_animation.exe",
        "The surviving match must be the application, not the shell that launched it.");
}

// The behaviour the executable rule must not break: --title vlc finding
// "movie.mkv - VLC media player". Here the fragment matches both fields of the
// same window, so preferring the executable still finds it.
void TestAnAppNamedFragmentStillFindsAWindowTitledAfterItsDocument() {
    const std::vector<osss::WindowEntry> desktop = {
        Entry(L"vlc.exe", L"movie.mkv - VLC media player"),
        Entry(L"explorer.exe", L"Downloads"),
    };
    const auto matches = osss::SelectWindowsMatching(desktop, L"vlc");
    Require(matches.size() == 1, "--title vlc must resolve to the VLC window.");
    Require(matches.front().process_name == L"vlc.exe", "Wrong window selected for vlc.");
}

// And the case that requires the title fallback to survive: a fragment naming
// the document, which no executable name contains.
void TestADocumentNamedFragmentFallsBackToTitles() {
    const std::vector<osss::WindowEntry> desktop = {
        Entry(L"vlc.exe", L"movie.mkv - VLC media player"),
        Entry(L"notepad.exe", L"notes.txt"),
    };
    const auto matches = osss::SelectWindowsMatching(desktop, L"movie.mkv");
    Require(matches.size() == 1, "A document fragment must still match by title.");
    Require(matches.front().process_name == L"vlc.exe", "Wrong window selected for movie.mkv.");
}

// Genuine ambiguity must stay ambiguous: two windows of the same application
// are a real choice the caller has to make with --hwnd.
void TestTwoWindowsOfOneApplicationStayAmbiguous() {
    const std::vector<osss::WindowEntry> desktop = {
        Entry(L"chrome.exe", L"Inbox"),
        Entry(L"chrome.exe", L"Docs"),
        Entry(L"WindowsTerminal.exe", L"chrome.exe --headless"),
    };
    const auto matches = osss::SelectWindowsMatching(desktop, L"chrome");
    Require(
        matches.size() == 2,
        "Two windows of one application must remain ambiguous, got " +
            std::to_string(matches.size()) + ".");
    for (const osss::WindowEntry& entry : matches) {
        Require(
            entry.process_name == L"chrome.exe",
            "The terminal must not survive alongside real executable matches.");
    }
}

void TestMatchingIsCaseInsensitive() {
    const auto matches =
        osss::SelectWindowsMatching(TerminalCollisionDesktop(), L"OSSS_Test_Animation");
    Require(matches.size() == 1, "Matching must ignore case.");
}

void TestNoMatchAndEmptyFragment() {
    Require(
        osss::SelectWindowsMatching(TerminalCollisionDesktop(), L"firefox").empty(),
        "A fragment matching nothing must return nothing.");
    // An empty fragment matches every string, which would silently resolve to
    // an arbitrary window. It must select nothing instead.
    Require(
        osss::SelectWindowsMatching(TerminalCollisionDesktop(), L"").empty(),
        "An empty fragment must not match every window.");
}

// A process whose executable could not be read has an empty process_name.
// std::wstring::find of a non-empty needle in an empty string fails, so these
// fall through to the title, which is the only thing known about them.
void TestUnidentifiedProcessesMatchByTitleOnly() {
    const std::vector<osss::WindowEntry> desktop = {
        Entry(L"", L"Protected Content Player"),
    };
    const auto matches = osss::SelectWindowsMatching(desktop, L"protected");
    Require(matches.size() == 1, "A window with no readable executable must match by title.");
}

} // namespace

int main() {
    try {
        TestExecutableMatchBeatsATitleMatch();
        TestAnAppNamedFragmentStillFindsAWindowTitledAfterItsDocument();
        TestADocumentNamedFragmentFallsBackToTitles();
        TestTwoWindowsOfOneApplicationStayAmbiguous();
        TestMatchingIsCaseInsensitive();
        TestNoMatchAndEmptyFragment();
        TestUnidentifiedProcessesMatchByTitleOnly();
        std::cout << "OSSS target-window selection tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
