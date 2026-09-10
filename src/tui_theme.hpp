// The monitor's palette, in one place.
//
// Modelled on btop's theme files, and using its vocabulary, because the shape
// has proven itself: a handful of named roles, plus GRADIENT TRIPLETS
// (start -> mid -> end) for anything drawn by magnitude. Keeping it as one
// struct rather than colour literals scattered through the render is what
// makes a second theme -- a light terminal, a projector, a colour-blind
// palette -- a value rather than a rewrite.
//
// One deliberate divergence from btop, and it is worth understanding before
// changing anything here. btop spends colour on HEIGHT: its CPU graph runs
// purple -> cyan -> green as a peak rises, so a glance at the hue tells you the
// magnitude. The throughput graph here spends colour on IDENTITY instead --
// which target a band belongs to -- because showing several concurrent
// replications at once is the thing it exists to do, and one area cannot encode
// both. So `band[]` below is a categorical palette, not a gradient, and the
// gradients are kept for the meters, where magnitude is the only thing being
// said.
#pragma once

#include <cstdint>

namespace erpl_rev {
namespace tui {

// Plain RGB, deliberately. Keeping FTXUI out of the palette means the
// gradient arithmetic is testable without a terminal, and the theme stays a
// data table anyone can edit without knowing the toolkit.
struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};

// start -> mid -> end, as btop writes them.
struct Gradient {
    Rgb start, mid, end;

    // The colour for a value in [0,1]. Below the midpoint interpolate
    // start->mid, above it mid->end -- the same two-segment ramp btop uses, so
    // the middle of the range is a distinct hue rather than a muddy blend of
    // the two extremes.
    Rgb At(double t) const;
};

struct Theme {
    // Roles, named as btop names them.
    Rgb main_fg, title, hi_fg, inactive_fg, graph_text, div_line, meter_bg;

    // One outline per box, so the eye learns which panel is which by colour
    // before reading the title.
    Rgb box_targets, box_throughput, box_daemon;

    // Status, which is the one thing that must never be theme-dependent enough
    // to misread: broken has to look broken in any palette.
    Rgb ok, warn, bad, parked_c;

    // Magnitude gradients.
    Gradient lag;      // how far behind: calm -> warm
    Gradient health;   // the summary meter

    // Categorical, NOT a gradient: which target a band belongs to. Chosen to
    // stay distinguishable next to each other and to survive the common forms
    // of colour blindness reasonably well -- adjacent bands differ in
    // lightness as well as hue.
    static constexpr int kBands = 6;
    Rgb band[kBands];
};

// The default palette. Dark-terminal first, which is what a monitor runs on.
const Theme &DefaultTheme();

}  // namespace tui
}  // namespace erpl_rev
