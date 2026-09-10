#include "tui_theme.hpp"

#include <cstdint>

namespace erpl_rev {
namespace tui {

namespace {
// Interpolate in RGB. Good enough for a terminal, and it keeps the theme a
// plain data table anyone can edit without knowing a colour space.
Rgb Lerp(const Rgb &a, const Rgb &b, double t) {
    const auto c = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(x + (y - x) * t + 0.5);
    };
    return Rgb{c(a.r, b.r), c(a.g, b.g), c(a.b, b.b)};
}
}  // namespace

Rgb Gradient::At(double t) const {
    if (t <= 0) return start;
    if (t >= 1) return end;
    return t < 0.5 ? Lerp(start, mid, t * 2.0) : Lerp(mid, end, (t - 0.5) * 2.0);
}

const Theme &DefaultTheme() {
    static const Theme t = [] {
        Theme x;
        x.main_fg     = Rgb{0xcd, 0xd6, 0xf4};
        x.title       = Rgb{0xf5, 0xe0, 0xdc};
        x.hi_fg       = Rgb{0x89, 0xb4, 0xfa};
        x.inactive_fg = Rgb{0x6c, 0x70, 0x86};
        x.graph_text  = Rgb{0xa6, 0xad, 0xc8};
        x.div_line    = Rgb{0x45, 0x47, 0x5a};
        x.meter_bg    = Rgb{0x31, 0x32, 0x44};

        x.box_targets    = Rgb{0x89, 0xb4, 0xfa};
        x.box_throughput = Rgb{0xa6, 0xe3, 0xa1};
        x.box_daemon     = Rgb{0xcb, 0xa6, 0xf7};

        x.ok       = Rgb{0xa6, 0xe3, 0xa1};
        x.warn     = Rgb{0xf9, 0xe2, 0xaf};
        x.bad      = Rgb{0xf3, 0x8b, 0xa8};
        x.parked_c = Rgb{0xcb, 0xa6, 0xf7};

        // calm -> warm, as lag grows.
        x.lag    = {Rgb{0x94, 0xe2, 0xd5}, Rgb{0xf9, 0xe2, 0xaf},
                    Rgb{0xf3, 0x8b, 0xa8}};
        x.health = {Rgb{0xf3, 0x8b, 0xa8}, Rgb{0xf9, 0xe2, 0xaf},
                    Rgb{0xa6, 0xe3, 0xa1}};

        // Categorical. Adjacent entries differ in lightness as well as hue, so
        // two bands stacked on each other stay separable without relying on
        // colour discrimination alone.
        x.band[0] = Rgb{0x89, 0xb4, 0xfa};   // blue
        x.band[1] = Rgb{0xa6, 0xe3, 0xa1};   // green
        x.band[2] = Rgb{0xf9, 0xe2, 0xaf};   // yellow
        x.band[3] = Rgb{0xcb, 0xa6, 0xf7};   // mauve
        x.band[4] = Rgb{0x94, 0xe2, 0xd5};   // teal
        x.band[5] = Rgb{0xfa, 0xb3, 0x87};   // peach
        return x;
    }();
    return t;
}

}  // namespace tui
}  // namespace erpl_rev
