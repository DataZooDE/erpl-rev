// `erpl-rev top` -- the replication monitor.
//
// Built for an operator on a terminal, in the shape of htop or lazydocker: the
// worst target at the top, one screen that refreshes itself, and single keys to
// act on the row under the cursor. Not a dashboard -- there is nothing here to
// admire, only things to notice and things to do.
//
// All the judgement lives in tui_model: which target is worst, how a lag reads,
// what counts as a problem. This file turns that into cells and keystrokes.

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "commands.hpp"
#include "db_client.hpp"
#include "tui_model.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace erpl_rev {
namespace cmd {
namespace {

using namespace ftxui;

// Colour carries the same three states the views keep apart, because they need
// three different actions: blocked needs re-registering, parked needs unpark, a
// lagging target needs its error read.
Color RowColour(const tui::Row &r) {
    if (r.blocked) return Color::Red;
    if (r.parked) return Color::Magenta;
    if (r.lag_seconds < 0) return Color::GrayDark;
    if (r.fail_count > 0) return Color::Yellow;
    return Color::Default;
}

std::string Pad(std::string s, size_t w) {
    if (s.size() > w) return s.substr(0, w - 1) + "…";
    s.resize(w, ' ');
    return s;
}

}  // namespace

int RunTop(Options o) {
    const auto cfg = cli::ReadConfig();
    dbc::Endpoint ep;
    try {
        ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "erpl-rev top: %s\n", e.what());
        return 1;
    }

    // The snapshot is written by the refresh thread and read by the renderer on
    // FTXUI's thread. Unguarded, the renderer iterates a vector that the ticker
    // is reallocating underneath it -- a crash in a monitor, which is the one
    // tool an operator reaches for when things are already going wrong.
    //
    // A whole-snapshot swap under one lock rather than finer locking: the
    // display must never show half of one poll and half of the next, and there
    // is nothing here worth contending over.
    std::mutex snap_mx;
    tui::Snapshot snap;
    int selected = 0;
    std::string action_note;

    // Whether this is a live monitor or one frame. A tool an operator runs is
    // also a tool a script runs, and an interactive-only monitor cannot be put
    // in a log, a ticket or an e2e assertion.
    bool once = false;
    for (const auto &a : o.args) if (a == "--once") once = true;

    // Re-opened per refresh rather than held: the monitor is expected to survive
    // the server restarting under it, and a held handle would not.
    auto refresh = [&] {
        // Loaded OUTSIDE the lock: the read talks to the server and can block
        // for as long as the network takes, and holding the lock across it
        // would freeze the display exactly when the server is slow.
        tui::Snapshot fresh;
        try {
            auto db = dbc::Db::Open(ep);
            fresh = tui::Load([&](const std::string &sql) { return db.Query(sql); });
        } catch (const std::exception &e) {
            fresh = tui::Snapshot{};
            fresh.error = e.what();
        }
        std::lock_guard<std::mutex> g(snap_mx);
        snap = std::move(fresh);
        if (selected >= static_cast<int>(snap.rows.size()))
            selected = snap.rows.empty() ? 0 : static_cast<int>(snap.rows.size()) - 1;
    };
    refresh();

    auto screen = ScreenInteractive::Fullscreen();

    auto render = [&] {
        std::lock_guard<std::mutex> g(snap_mx);
        const auto &s = snap.summary;
        // The header answers "is anything wrong" before the eye reaches the
        // table. The daemon is here because "nothing is replicating" is usually
        // the daemon, not the targets.
        auto daemon_ok = s.daemon_status == "RUNNING";
        Elements head{
            text("erpl-rev") | bold,
            text("  targets " + std::to_string(s.targets)),
            text("  healthy " + std::to_string(s.healthy)) | color(Color::Green),
        };
        if (s.blocked) head.push_back(text("  blocked " + std::to_string(s.blocked)) |
                                      color(Color::Red) | bold);
        if (s.parked) head.push_back(text("  parked " + std::to_string(s.parked)) |
                                     color(Color::Magenta));
        if (s.failing) head.push_back(text("  failing " + std::to_string(s.failing)) |
                                      color(Color::Yellow));
        if (s.never_run) head.push_back(text("  never run " + std::to_string(s.never_run)) |
                                        color(Color::GrayDark));
        head.push_back(filler());
        head.push_back(text("worst lag " + tui::FormatLag(s.worst_lag)));
        head.push_back(text("  daemon " + s.daemon_status +
                            (s.daemon_age >= 0 ? " (" + tui::FormatLag(s.daemon_age) + " ago)"
                                               : "")) |
                       color(daemon_ok ? Color::Green : Color::Red));

        Elements body;
        body.push_back(hbox({text(Pad("TARGET", 24)) | bold, text(Pad("METHOD", 12)) | bold,
                             text(Pad("CADENCE", 10)) | bold, text(Pad("STATUS", 10)) | bold,
                             text(Pad("LAG", 9)) | bold, text(Pad("ROWS", 9)) | bold,
                             text(Pad("FAILS", 6)) | bold, text("NOTE") | bold}));
        for (size_t i = 0; i < snap.rows.size(); ++i) {
            const auto &r = snap.rows[i];
            // The reason, where there is one: a status with no reason is a
            // status nobody can act on.
            std::string note = r.blocked  ? r.last_error
                             : r.parked   ? r.park_reason
                             : r.fail_count > 0 ? r.last_error
                                                : std::string();
            auto line = hbox({text(Pad(r.target, 24)), text(Pad(r.method, 12)),
                              text(Pad(r.cadence, 10)), text(Pad(r.status, 10)),
                              text(Pad(tui::FormatLag(r.lag_seconds), 9)),
                              text(Pad(std::to_string(r.rows), 9)),
                              text(Pad(std::to_string(r.fail_count), 6)), text(note)}) |
                        color(RowColour(r));
            if (static_cast<int>(i) == selected) line = line | inverted;
            body.push_back(line);
        }
        if (snap.rows.empty())
            body.push_back(text("  no registered targets") | color(Color::GrayDark));

        Elements foot{text(" q quit   r refresh   n run now   u unpark   ↑/↓ select ") |
                      color(Color::GrayDark)};
        if (!action_note.empty()) foot.push_back(text("  " + action_note) | color(Color::Cyan));
        if (!snap.error.empty())
            foot.push_back(text("  " + snap.error) | color(Color::Red) | bold);

        return vbox({hbox(head), separator(), vbox(body) | flex, separator(), hbox(foot)}) |
               border;
    };

    // Actions go through the ordinary queue, exactly as the CLI verbs do. A
    // monitor that reached into the database directly would be a second way to
    // change replication state, and the two would drift.
    auto act = [&](const char *verb) {
        std::string target;
        {
            std::lock_guard<std::mutex> g(snap_mx);
            if (snap.rows.empty()) return;
            target = snap.rows[selected].target;
        }
        Options a = o;
        a.non_interactive = true;   // a monitor must never stop to ask
        a.assume_yes = true;
        a.args = {std::string(verb), target};
        const int rc = RunSync(a);
        // The REAL outcome. Saying "queued" regardless would be a key that
        // reports success for work that never happened, which is the whole
        // failure mode this session has been chasing out of the product.
        {
            std::lock_guard<std::mutex> g(snap_mx);
            // rc 3 is "queued, outcome not yet known" -- reported as queued
            // rather than as done, because a monitor that says "accepted" for
            // something still sitting in a queue is telling the operator the
            // work happened.
            action_note = rc == 0   ? std::string(verb) + " " + target + ": done"
                        : rc == 3   ? std::string(verb) + " " + target + ": queued"
                                    : std::string(verb) + " " + target + ": FAILED (rc " +
                                          std::to_string(rc) + ")";
        }
        refresh();
    };

    if (once) {
        // No lock here: the ticker has not started, this thread is the only one
        // touching the snapshot, and render() takes the lock itself -- taking it
        // here too would deadlock on a non-recursive mutex.
        // Rendered through the same element tree, so what a script sees is what
        // an operator sees -- a second formatter would drift from the first.
        auto doc = render();
        auto scr = Screen::Create(Dimension::Fit(doc), Dimension::Fit(doc));
        Render(scr, doc);
        std::printf("%s\n", scr.ToString().c_str());
        return snap.error.empty() ? 0 : 1;
    }

    auto component = Renderer([&] { return render(); });
    component |= CatchEvent([&](Event e) {
        if (e == Event::Character('q') || e == Event::Escape) { screen.Exit(); return true; }
        if (e == Event::Character('r')) { refresh(); return true; }
        if (e == Event::ArrowDown || e == Event::Character('j')) {
            if (selected + 1 < static_cast<int>(snap.rows.size())) ++selected;
            return true;
        }
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            if (selected > 0) --selected;
            return true;
        }
        if (e == Event::Character('n')) { act("run"); return true; }
        if (e == Event::Character('u')) { act("unpark"); return true; }
        return false;
    });

    // A refresh loop on its own thread, so the display keeps moving while the
    // operator is not pressing anything -- which is most of the time.
    std::atomic<bool> alive{true};
    std::thread ticker([&] {
        while (alive.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!alive.load()) break;
            refresh();
            screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(component);
    alive.store(false);
    ticker.join();
    return 0;
}

}  // namespace cmd
}  // namespace erpl_rev
