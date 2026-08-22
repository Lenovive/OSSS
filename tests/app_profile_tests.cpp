// Per-application profile parsing, formatting, and lookup. Touches no disk and
// no GPU: ProfilePath/LoadProfiles/SaveProfiles are the only parts that need
// either, and they are thin wrappers over the logic exercised here.

#include "app_profile.h"
#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using osss::test::Require;

void TestEmptyAndComments() {
    const auto parsed = osss::ParseProfiles(
        L"# a comment\n"
        L"; also a comment\n"
        L"\n"
        L"   \n");
    Require(parsed.Ok(), "comments and blank lines must parse");
    Require(parsed.entries.empty(), "no sections means no entries");
}

void TestSectionsAndArguments() {
    const auto parsed = osss::ParseProfiles(
        L"[game.exe]\n"
        L"--target-fps 240 --max-multiplier 4\n"
        L"--flow-scale quality\n"
        L"\n"
        L"[vlc.exe]\n"
        L"--target-fps 120\n");
    Require(parsed.Ok(), "a well-formed file must parse");
    Require(parsed.entries.size() == 2, "two sections must give two entries");
    Require(parsed.entries[0].executable == L"game.exe", "first section name");
    Require(
        parsed.entries[0].arguments ==
            std::vector<std::wstring>{
                L"--target-fps", L"240", L"--max-multiplier", L"4", L"--flow-scale", L"quality"},
        "arguments must accumulate across lines in source order");
    Require(parsed.entries[1].arguments.size() == 2, "second section arguments");
}

// --ui-mask carries semicolons and spaces, so quoting has to survive a round
// trip or a masked HUD silently stops being masked.
void TestQuotedValues() {
    const auto parsed = osss::ParseProfiles(
        L"[game.exe]\n"
        L"--ui-mask \"0,0,0.22,0.18; 1560,940,1920,1080px\"\n");
    Require(parsed.Ok(), "a quoted value must parse");
    Require(parsed.entries[0].arguments.size() == 2, "a quoted value is one argument");
    Require(
        parsed.entries[0].arguments[1] == L"0,0,0.22,0.18; 1560,940,1920,1080px",
        "the quoted value must keep its spaces and semicolons");
}

void TestRejections() {
    const auto no_section = osss::ParseProfiles(L"--target-fps 240\n");
    Require(!no_section.Ok(), "arguments before any section must be rejected");
    Require(no_section.error_line == 1, "the failing line must be reported");

    const auto bad_header = osss::ParseProfiles(L"[game.exe\n");
    Require(!bad_header.Ok(), "an unterminated section header must be rejected");

    const auto empty_header = osss::ParseProfiles(L"[]\n");
    Require(!empty_header.Ok(), "an empty section name must be rejected");

    const auto unterminated = osss::ParseProfiles(L"[game.exe]\n--ui-mask \"0,0,1,1\n");
    Require(!unterminated.Ok(), "an unterminated quote must be rejected");
    Require(unterminated.error_line == 2, "the failing line must be reported");
}

// A malformed file is reported rather than partially applied. Half a profile
// looks exactly like the settings not working, which is the worst outcome.
void TestPartialFileIsNotApplied() {
    const auto parsed = osss::ParseProfiles(
        L"[good.exe]\n"
        L"--target-fps 240\n"
        L"[bad\n");
    Require(!parsed.Ok(), "a later error must fail the whole parse");
}

void TestRoundTrip() {
    std::vector<osss::ProfileEntry> entries;
    osss::SetProfileArguments(
        entries,
        L"game.exe",
        {L"--target-fps", L"240", L"--ui-mask", L"0,0,0.2,0.2; 100,100,200,200px"});
    osss::SetProfileArguments(entries, L"vlc.exe", {L"--flow-scale", L"performance"});

    const auto reparsed = osss::ParseProfiles(osss::FormatProfiles(entries));
    Require(reparsed.Ok(), "formatted output must reparse");
    Require(reparsed.entries.size() == 2, "round trip must keep both sections");
    Require(
        reparsed.entries[0].arguments == entries[0].arguments,
        "round trip must keep arguments byte-identical, quoting included");
    Require(reparsed.entries[1].arguments == entries[1].arguments, "second section round trip");
}

void TestLookupIsCaseInsensitive() {
    const auto parsed = osss::ParseProfiles(L"[Game.EXE]\n--target-fps 240\n");
    Require(parsed.Ok(), "parse");
    Require(
        osss::FindProfileArguments(parsed.entries, L"game.exe").has_value(),
        "lookup must ignore case, because process_name casing is not guaranteed");
    Require(
        !osss::FindProfileArguments(parsed.entries, L"other.exe").has_value(),
        "an unrelated executable must not match");
    Require(
        !osss::FindProfileArguments(parsed.entries, L"").has_value(),
        "an empty executable name must never match a section");
}

void TestSetReplacesRatherThanDuplicating() {
    std::vector<osss::ProfileEntry> entries;
    osss::SetProfileArguments(entries, L"game.exe", {L"--target-fps", L"120"});
    osss::SetProfileArguments(entries, L"GAME.EXE", {L"--target-fps", L"240"});
    Require(entries.size() == 1, "the same executable must not gain a second section");
    Require(entries[0].arguments[1] == L"240", "the newer arguments must win");
}

void TestRepeatedSectionAppends() {
    const auto parsed = osss::ParseProfiles(
        L"[game.exe]\n"
        L"--target-fps 240\n"
        L"[game.exe]\n"
        L"--flow-scale quality\n");
    Require(parsed.Ok(), "a repeated section must parse");
    Require(parsed.entries.size() == 1, "a repeated section must not create a second entry");
    Require(parsed.entries[0].arguments.size() == 4, "a repeated section must append");
}

} // namespace

int main() {
    try {
        TestEmptyAndComments();
        TestSectionsAndArguments();
        TestQuotedValues();
        TestRejections();
        TestPartialFileIsNotApplied();
        TestRoundTrip();
        TestLookupIsCaseInsensitive();
        TestSetReplacesRatherThanDuplicating();
        TestRepeatedSectionAppends();
        std::cout << "OSSS per-application profile tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
