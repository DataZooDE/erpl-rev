// `erpl-rev top` -- the replication monitor.
//
// Built for an operator on a terminal, in the shape of htop or lazydocker: the
// worst target at the top, one screen that refreshes itself, and single keys to
// act on the row under the cursor. Not a dashboard -- there is nothing here to
// admire, only things to notice and things to do.
//
// All the judgement lives in tui_model: which target is worst, how a lag reads,
// what counts as a problem. This file turns that into cells and keystrokes.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "commands.hpp"
#include "db_client.hpp"
#include "tui_graph.hpp"
#include "tui_model.hpp"
#include "tui_theme.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>

namespace erpl_rev {
namespace cmd {
namespace {

using namespace ftxui;

// The palette lives in tui_theme, free of FTXUI so its gradients can be tested
// without a terminal. This is the only place the two meet.
Color C(const tui::Rgb &c) { return Color::RGB(c.r, c.g, c.b); }

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

    // The throughput history, and whether anyone asked for it.
    //
    // Off by default and behind `g`, because the samples are not free: one row
    // count per target per refresh. DuckDB answers those from metadata, but a
    // monitor should not spend work nobody asked for -- and a scripted
    // `top --once` should render the same frame it always did.
    bool graph_on = false;

    // The terminal width, read OFF the render path.
    //
    // Asking the screen for its width from inside render() -- which runs on
    // FTXUI's thread while holding snap_mx -- stalled the refresh loop
    // completely: the display froze and the graph drew nothing, which looks
    // exactly like a broken graph rather than a monitor that has stopped
    // updating. Sampled by the ticker instead, where nothing else is held.
    std::atomic<int> term_cols{92};
    std::vector<tui::CountSample> history;
    const size_t kHistory = 240;   // ~8 minutes at the 2s refresh

    // Whether this is a live monitor or one frame. A tool an operator runs is
    // also a tool a script runs, and an interactive-only monitor cannot be put
    // in a log, a ticket or an e2e assertion.
    bool once = false;
    // --graph exists so the graph is reachable without a keypress. Without it
    // the sampling and the whole canvas path cannot be entered by any test,
    // any script or any sanitizer job -- which is precisely why the wiring had
    // no coverage while the geometry had plenty.
    bool want_graph = false;
    // --refreshes N runs N cycles at the real cadence IN ONE PROCESS.
    //
    // One --once frame performs a single refresh, and a rate needs two
    // samples, so a --once run can never draw a band: a loop of separate
    // --once processes would pass happily over a binary that freezes after a
    // handful of samples, which is the failure being guarded against. The
    // repetition has to happen inside one process to mean anything.
    int refreshes = 1;
    for (size_t i = 0; i < o.args.size(); ++i) {
        if (o.args[i] == "--once") once = true;
        else if (o.args[i] == "--graph") want_graph = true;
        else if (o.args[i] == "--refreshes" && i + 1 < o.args.size())
            refreshes = std::max(1, std::atoi(o.args[++i].c_str()));
    }
    if (refreshes > 1) once = true;   // a repeated frame is still a one-shot render

    // Re-opened per refresh rather than held: the monitor is expected to survive
    // the server restarting under it, and a held handle would not.
    // refresh() is reachable from the ticker AND from FTXUI's thread via `r`,
    // `g`, `n` and `u`. Serialised so two cannot be in flight at once: both
    // would append to the history, and two samples milliseconds apart invent a
    // rate that flattens the graph for as long as it stays in the window.
    //
    // `sample` is false for every key-driven refresh. The history should carry
    // one clock at one cadence -- the ticker's -- not extra points wherever an
    // operator happened to press a key.
    std::mutex refresh_mx;
    auto refresh = [&](bool sample) {
        std::lock_guard<std::mutex> rg(refresh_mx);
        // Loaded OUTSIDE the snapshot lock: the read talks to the server and
        // can block for as long as the network takes, and holding that lock
        // across it would freeze the display exactly when the server is slow.
        bool want_counts = false;
        {
            std::lock_guard<std::mutex> g(snap_mx);
            want_counts = graph_on && sample;
        }

        // ONE connection for both reads. Opening a second one per refresh for
        // the graph's samples was enough to make them stop arriving after the
        // first couple, which showed up as a graph that drew nothing over a
        // table growing by six thousand rows a second.
        tui::Snapshot fresh;
        std::vector<std::pair<std::string, long long>> counts;
        try {
            auto db = dbc::Db::Open(ep);
            auto q = [&](const std::string &sql) { return db.Query(sql); };
            fresh = tui::Load(q);
            if (want_counts && fresh.error.empty()) {
                std::vector<std::string> names;
                names.reserve(fresh.rows.size());
                for (const auto &r : fresh.rows) names.push_back(r.target);
                counts = tui::SampleCounts(q, names);
            }
        } catch (const std::exception &e) {
            fresh = tui::Snapshot{};
            fresh.error = e.what();
            counts.clear();   // a gap in the graph, not a failed refresh
        }

        std::lock_guard<std::mutex> g(snap_mx);
        snap = std::move(fresh);
        if (want_counts && !counts.empty()) {
            tui::CountSample cs;
            // A monotonic clock: the rate is a division by elapsed time, and a
            // wall clock that steps backwards over NTP would render a negative
            // or enormous spike out of nothing.
            cs.at_epoch = std::chrono::duration<double>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
            cs.counts = std::move(counts);
            history.push_back(std::move(cs));
            if (history.size() > kHistory) history.erase(history.begin());
        }
        if (selected >= static_cast<int>(snap.rows.size()))
            selected = snap.rows.empty() ? 0 : static_cast<int>(snap.rows.size()) - 1;
    };
    graph_on = want_graph;
    refresh(want_graph);

    auto screen = ScreenInteractive::Fullscreen();

    // The throughput box: a stacked braille area, one colour band per target.
    //
    // Deliberately NOT btop's encoding. btop spends colour on HEIGHT -- its CPU
    // graph runs purple to green as a peak rises -- so a glance at the hue
    // gives the magnitude. Here colour says WHICH TARGET, because showing
    // several concurrent replications at once is the thing this graph exists
    // for, and one area cannot encode both. The height gradients are kept for
    // the meters, where magnitude is the only thing being said.
    auto throughput_box = [&](int cols) -> Element {
        const auto &th = tui::DefaultTheme();
        const int chars_w = std::max(20, cols - 6);
        const int rows_h = 7;
        const int px = chars_w * 2;   // braille: 2 dots across a cell
        const int py = rows_h * 4;    // and 4 down

        const auto buckets = tui::Rates(history);

        // Which targets to draw, and in a stable order. Sorted by name so a
        // band does not jump between rows when the table re-sorts by severity.
        std::vector<std::string> names;
        for (const auto &b : buckets)
            for (const auto &r : b.rates)
                if (std::find(names.begin(), names.end(), r.first) == names.end())
                    names.push_back(r.first);
        std::sort(names.begin(), names.end());

        double peak = 0;
        for (const auto &b : buckets) peak = std::max(peak, b.Total());
        const double ceiling = tui::NiceCeiling(peak);
        const double now_rate = buckets.empty() ? 0.0 : buckets.back().Total();

        // Laid out by the pure half, and captured BY VALUE below.
        //
        // canvas() stores its callback and invokes it during LAYOUT -- after
        // this function has returned. A callback capturing the sample vectors
        // by reference reads them destroyed, and the draw loop then never
        // ends: the monitor's thread spins at 100% and the display freezes,
        // which looks exactly like a hung query. Values cannot dangle.
        struct Span {
            int x = 0, y_top = 0, y_bottom = 0;
            Color col;
        };
        std::vector<Span> spans;
        for (const auto &b : tui::BandSpans(buckets, names, ceiling, px, py))
            spans.push_back({b.x, b.y_top, b.y_bottom,
                             C(th.band[tui::ColorSlotFor(names[b.band], tui::Theme::kBands)])});

        auto c = canvas(px, py, [spans](Canvas &cv) {
            for (const auto &s : spans)
                for (int y = s.y_bottom; y >= s.y_top; --y) cv.DrawPoint(s.x, y, true, s.col);
        });

        Elements legend;
        for (const auto &nm : names) {
            double v = 0;
            if (!buckets.empty())
                for (const auto &r : buckets.back().rates)
                    if (r.first == nm) v = r.second;
            legend.push_back(text("  ■ ") |
                             color(C(th.band[tui::ColorSlotFor(nm, tui::Theme::kBands)])));
            legend.push_back(text(nm + " ") | color(C(th.main_fg)));
            legend.push_back(text(tui::FormatRate(v)) | color(C(th.graph_text)));
        }
        if (names.empty())
            legend.push_back(text("  sampling…") | color(C(th.inactive_fg)));

        Element title = hbox({
            text(" throughput ") | bold | color(C(th.title)),
            text("scale " + tui::FormatRate(ceiling)) | color(C(th.inactive_fg)),


            text("  ·  ") | color(C(th.div_line)),
            text("now " + tui::FormatRate(now_rate)) | color(C(th.box_throughput)) | bold,
            text("  peak " + tui::FormatRate(peak) + " ") | color(C(th.graph_text)),
        });
        return window(title, vbox({c, hbox(legend)}), ROUNDED) |
               color(C(th.box_throughput));
    };

    auto render = [&] {
        std::lock_guard<std::mutex> g(snap_mx);
        const auto &th = tui::DefaultTheme();
        const auto &s = snap.summary;
        // The header answers "is anything wrong" before the eye reaches the
        // table. The daemon is here because "nothing is replicating" is usually
        // the daemon, not the targets.
        auto daemon_ok = s.daemon_status == "RUNNING";
        // The counts live in the box's own border, as btop puts a value in the
        // title: the answer to "is anything wrong" arrives before the eye
        // reaches the first row.
        Elements head{
            text(" targets ") | bold | color(C(th.title)),
            text(std::to_string(s.targets)) | color(C(th.main_fg)),
            text("  healthy " + std::to_string(s.healthy)) | color(C(th.ok)),
        };
        if (s.blocked) head.push_back(text("  blocked " + std::to_string(s.blocked)) |
                                      color(C(th.bad)) | bold);
        if (s.parked) head.push_back(text("  parked " + std::to_string(s.parked)) |
                                     color(C(th.parked_c)));
        if (s.failing) head.push_back(text("  failing " + std::to_string(s.failing)) |
                                      color(C(th.warn)));
        if (s.never_run) head.push_back(text("  never run " + std::to_string(s.never_run)) |
                                        color(C(th.inactive_fg)));
        // No filler() here. A window's title is sized to its content, so a
        // filler has nothing to expand into and the right-hand items simply
        // butt against the left-hand ones -- "healthy 2worst lag 0s". Separated
        // explicitly instead, which also survives a narrow terminal.
        head.push_back(text("  ·  ") | color(C(th.div_line)));
        // The lag gradient: calm to warm as it grows. An hour is the ceiling
        // for the ramp -- past that it is simply red, and the number says how
        // much worse.
        const double lag_t = s.worst_lag < 0 ? 0.0 : std::min(1.0, s.worst_lag / 3600.0);
        head.push_back(text("worst lag " + tui::FormatLag(s.worst_lag)) |
                       color(C(th.lag.At(lag_t))));
        head.push_back(text("  ·  ") | color(C(th.div_line)));
        head.push_back(text("daemon " + s.daemon_status +
                            (s.daemon_age >= 0 ? " (" + tui::FormatLag(s.daemon_age) + " ago)"
                                               : "") + " ") |
                       color(daemon_ok ? C(th.ok) : C(th.bad)) | bold);

        Elements body;
        body.push_back(hbox({text(Pad("TARGET", 24)), text(Pad("METHOD", 12)),
                             text(Pad("CADENCE", 10)), text(Pad("STATUS", 10)),
                             text(Pad("LAG", 9)), text(Pad("ROWS", 9)),
                             text(Pad("FAILS", 6)), text("NOTE")}) |
                       bold | color(C(th.hi_fg)));
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
            body.push_back(text("  no registered targets") | color(C(th.inactive_fg)));

        Elements foot{text(" q quit   r refresh   g graph   n run now   u unpark   ↑/↓ select ") |
                      color(C(th.inactive_fg))};
        if (!action_note.empty()) foot.push_back(text("  " + action_note) | color(C(th.hi_fg)));
        if (!snap.error.empty())
            foot.push_back(text("  " + snap.error) | color(C(th.bad)) | bold);

        Elements panes;
        // The terminal's real width when there is one; a fixed, readable width
        // for `--once`, whose output is sized to fit the document rather than a
        // screen. `top`'s columns already need 88, so this is the same budget.
        if (graph_on) panes.push_back(throughput_box(once ? 80 : term_cols.load()));
        panes.push_back(window(hbox(head), vbox(body) | flex, ROUNDED) |
                        color(C(th.box_targets)) | flex);
        panes.push_back(hbox(foot));
        return vbox(panes);
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
        refresh(false);
    };

    if (once) {
        // The extra cycles run at the real cadence, so the samples are spaced
        // the way they are in a live monitor -- including past the dt floor
        // that drops a pair taken too close together.
        for (int i = 1; i < refreshes; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            refresh(want_graph);
        }
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
        if (e == Event::Character('r')) { refresh(false); return true; }
        // Under the lock too. These read snap.rows.size() while the refresh
        // thread may be replacing the vector -- the same race the render path
        // was fixed for, missed on the branches that move the cursor.
        if (e == Event::ArrowDown || e == Event::Character('j')) {
            std::lock_guard<std::mutex> g(snap_mx);
            if (selected + 1 < static_cast<int>(snap.rows.size())) ++selected;
            return true;
        }
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            std::lock_guard<std::mutex> g(snap_mx);
            if (selected > 0) --selected;
            return true;
        }
        if (e == Event::Character('g')) {
            {
                std::lock_guard<std::mutex> g(snap_mx);
                graph_on = !graph_on;
                // Opening the graph starts a fresh history. Keeping the old
                // samples across a close and re-open would put a gap of
                // arbitrary length between two counts and render whatever
                // arrived in between as one enormous spike.
                if (graph_on) history.clear();
            }
            refresh(false);
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
            term_cols.store(std::max(40, screen.dimx()));
            refresh(true);   // only the ticker samples: one clock, one cadence
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
