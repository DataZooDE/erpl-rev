// `top --once` must show the daemon state in full.
//
// The summary header used to live in the window title, which FTXUI truncates
// to the table width: with seven targets on screen `daemon STOPPED` rendered
// as `daemon STOPP`, and the e2e assertion grepping for the full state failed.
// This renders through the same Screen::Create(Dimension::Fit) path the real
// `--once` uses, so it fails exactly when the product truncates.

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "commands.hpp"
#include "tui_model.hpp"

using namespace erpl_rev;
using namespace ftxui;

namespace {

// The e2e state that failed: seven targets, one failing, one never run, and a
// stopped daemon -- a header longer than the eighty-column table.
tui::Snapshot SevenTargetsDaemonStopped() {
    tui::Snapshot snap;
    snap.summary.targets = 7;
    snap.summary.healthy = 5;
    snap.summary.failing = 1;
    snap.summary.never_run = 1;
    snap.summary.worst_lag = 154;
    snap.summary.daemon_status = "STOPPED";
    snap.summary.daemon_age = 3;
    const char *names[7] = {"ape_t_rel3", "ape_t_full", "t000_cli", "ape_t_pos",
                            "ape_t_fempty", "ape_t_rel2", "t000_flags"};
    for (int i = 0; i < 7; i++) {
        tui::Row r;
        r.target = names[i];
        r.method = "APE_FULL";
        r.cadence = "hourly";
        r.status = "IDLE";
        r.lag_seconds = 30 + i;
        snap.rows.push_back(r);
    }
    snap.rows[0].status = "ERROR";
    snap.rows[0].fail_count = 1;
    snap.rows[0].last_error = "engine: fatal error found";
    snap.rows[6].lag_seconds = -1;   // registered, never run
    return snap;
}

// Exactly what `--once` prints: the same document, the same Fit screen.
std::string RenderOnce(const tui::Snapshot &snap) {
    auto doc = cmd::RenderTopDocument(snap, 0, "", false,
                                      [](int) -> Element { return text(""); }, true, 80);
    auto screen = Screen::Create(Dimension::Fit(doc), Dimension::Fit(doc));
    Render(screen, doc);
    return screen.ToString();
}

}  // namespace

TEST_CASE("top: the --once frame shows the daemon state untruncated", "[top]") {
    // The full header, every count and the whole daemon state: squeezed
    // middle numbers and a clipped tail are the same defect as a missing one,
    // because a script greps this output and an operator reads it. Matched on
    // the text with escape sequences stripped, so colour boundaries between
    // adjacent elements cannot fake a gap or hide one.
    const std::string out = RenderOnce(SevenTargetsDaemonStopped());
    std::string plain;
    bool esc = false;
    for (char c : out) {
        if (c == '\033') esc = true;
        else if (esc && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            esc = false;
        else if (!esc) plain += c;
    }
    CHECK(plain.find("targets 7") != std::string::npos);
    CHECK(plain.find("healthy 5") != std::string::npos);
    CHECK(plain.find("failing 1") != std::string::npos);
    CHECK(plain.find("never run 1") != std::string::npos);
    CHECK(plain.find("worst lag 2m34") != std::string::npos);
    CHECK(plain.find("daemon STOPPED (3s ago)") != std::string::npos);
    CHECK(plain.find("TARGET") != std::string::npos);
}
