#include "delta_plan.hpp"

#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <stdexcept>

namespace erpl_rev {
namespace wm {

namespace {

constexpr int64_t kDay = 86400;


// Civil <-> epoch on UTC only. Every value here is compared against another
// value from the SAME column, so the arithmetic only has to be self-consistent.
int64_t CivilToEpoch(int y, int mo, int d, int h, int mi, int s) {
    // Howard Hinnant's days_from_civil.
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * kDay + h * 3600 + mi * 60 + s;
}

void EpochToCivil(int64_t t, int &y, int &mo, int &d, int &h, int &mi, int &s) {
    int64_t days = t / kDay;
    int64_t rem = t % kDay;
    if (rem < 0) { rem += kDay; --days; }
    h = static_cast<int>(rem / 3600);
    mi = static_cast<int>((rem % 3600) / 60);
    s = static_cast<int>(rem % 60);

    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mo = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y = static_cast<int>(yr + (mo <= 2));
}

int Num(const std::string &s, size_t off, size_t len) {
    int v = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[off + i];
        if (c < '0' || c > '9') throw std::runtime_error("watermark: not a number: " + s);
        v = v * 10 + (c - '0');
    }
    return v;
}

std::string Pad(int64_t v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*lld", width, static_cast<long long>(v));
    return buf;
}

// A DATS value (YYYYMMDD) as an epoch second at midnight.
int64_t ParseDats(const std::string &d) {
    if (d.size() < 8) throw std::runtime_error("watermark: short DATS: " + d);
    return CivilToEpoch(Num(d, 0, 4), Num(d, 4, 2), Num(d, 6, 2), 0, 0, 0);
}

std::string FormatDats(int64_t epoch) {
    int y, mo, d, h, mi, s;
    EpochToCivil(epoch, y, mo, d, h, mi, s);
    return Pad(y, 4) + Pad(mo, 2) + Pad(d, 2);
}

// TIMESTAMPL carries a fractional tail after '.'; arithmetic is on whole
// seconds, and the tail rides along so a boundary re-read cannot skip
// sub-second rows.
std::string FractionOf(const std::string &v) {
    const auto dot = v.find('.');
    return dot == std::string::npos ? std::string() : v.substr(dot);
}

int64_t ClampNonNegative(int64_t v) { return v < 0 ? 0 : v; }

}  // namespace

std::string FormatNumts(int64_t epoch) {
    int y, mo, d, h, mi, s;
    EpochToCivil(epoch, y, mo, d, h, mi, s);
    return Pad(y, 4) + Pad(mo, 2) + Pad(d, 2) + Pad(h, 2) + Pad(mi, 2) + Pad(s, 2);
}

int64_t ParseNumts(const std::string &v) {
    if (v.size() < 14) throw std::runtime_error("watermark: short timestamp: " + v);
    return CivilToEpoch(Num(v, 0, 4), Num(v, 4, 2), Num(v, 6, 2),
                        Num(v, 8, 2), Num(v, 10, 2), Num(v, 12, 2));
}

bool IsUsableAsWatermark(WmKind k) { return k != WmKind::Changenr; }

std::string WhyNotUsable(WmKind k) {
    if (k != WmKind::Changenr) return {};
    // Stated where an operator will read it, because the alternative matters as
    // much as the refusal.
    return "CHANGENR comes from a buffered number range and is not monotonic in "
           "commit order, so an overlap expressed in change numbers cannot bound "
           "loss. Use the CHANGEDOC method, which positions on UDATE+UTIME.";
}

WmKind ParseKind(const std::string &s) {
    if (s == "NUMTS") return WmKind::Numts;
    if (s == "TIMESTAMPL") return WmKind::Timestampl;
    if (s == "DATE") return WmKind::Date;
    if (s == "DATETIME") return WmKind::Datetime;
    if (s == "INT") return WmKind::Int;
    if (s == "CHANGENR") return WmKind::Changenr;
    throw std::runtime_error("watermark: unknown wm_kind '" + s + "'");
}

std::string KindName(WmKind k) {
    switch (k) {
        case WmKind::Numts: return "NUMTS";
        case WmKind::Timestampl: return "TIMESTAMPL";
        case WmKind::Date: return "DATE";
        case WmKind::Datetime: return "DATETIME";
        case WmKind::Int: return "INT";
        case WmKind::Changenr: return "CHANGENR";
    }
    return "NUMTS";
}

std::string CeilingFromStagedMax(const WatermarkSpec &spec, const std::string &staged_max) {
    if (staged_max.empty()) return spec.wm_value;
    const int64_t max_v = std::stoll(staged_max);
    const auto out = std::to_string(ClampNonNegative(max_v - spec.safety_units));

    // Keep the source's zero padding. A NUMC or CHAR document number is stored
    // padded and the generated predicate is a STRING comparison, so dropping the
    // padding inverts it: '0000020000' > '19950' is FALSE lexically, and the
    // target reads nothing on every cycle after the first, silently and forever.
    // Widths are preserved, never invented -- an unpadded counter stays unpadded.
    if (out.size() < staged_max.size())
        return std::string(staged_max.size() - out.size(), '0') + out;
    return out;
}

std::string FloorDate(const Bounds &b) { return b.floor.substr(0, 8); }
std::string FloorTime(const Bounds &b) { return b.floor.substr(8, 6); }

Bounds ComputeBounds(const WatermarkSpec &spec, int64_t read_start_epoch,
                     bool full_reload) {
    Bounds b;
    b.has_floor = !spec.wm_value.empty();

    switch (spec.kind) {
        case WmKind::Changenr:
            throw std::runtime_error(WhyNotUsable(WmKind::Changenr));

        case WmKind::Int: {
            // No clock: the ceiling is cut from the staged rows at commit time.
            if (b.has_floor) {
                const auto f = std::to_string(
                    ClampNonNegative(std::stoll(spec.wm_value) - spec.safety_units));
                // Same reasoning as the ceiling: the floor is compared as text.
                b.floor = f.size() < spec.wm_value.size()
                              ? std::string(spec.wm_value.size() - f.size(), '0') + f
                              : f;
            }
            b.ceiling_from_stage = true;
            b.next_watermark = spec.wm_value;
            break;
        }

        case WmKind::Date: {
            // Complete days only. as_of_date is captured ONCE per cycle by the
            // caller passing read_start, so a retry after midnight cannot make
            // the rule mean something different halfway through.
            //
            b.as_of_date = FormatDats(read_start_epoch);
            // Any non-zero window has to round UP to a whole day, or it vanishes
            // against a day-granular column.
            const int64_t days = spec.safety_secs == 0
                                     ? 0
                                     : (spec.safety_secs + kDay - 1) / kDay;
            if (b.has_floor)
                b.floor = FormatDats(ParseDats(spec.wm_value) - days * kDay);
            b.has_ceiling = true;
            // Today is not a complete day, so a DELTA must not read it. A
            // reload must -- see the header.
            b.ceiling_bounds_read = !full_reload;
            b.ceiling = b.as_of_date;                       // exclusive
            b.next_watermark = FormatDats(ParseDats(b.as_of_date) - kDay);
            break;
        }

        case WmKind::Datetime:
        case WmKind::Numts:
        case WmKind::Timestampl: {
            const int64_t ceil_epoch = read_start_epoch - spec.safety_secs;
            if (b.has_floor) {
                const std::string frac =
                    spec.kind == WmKind::Timestampl ? FractionOf(spec.wm_value) : std::string();
                b.floor = FormatNumts(ParseNumts(spec.wm_value) - spec.safety_secs) + frac;
            }
            b.has_ceiling = true;
            b.ceiling = FormatNumts(ceil_epoch);
            if (spec.kind == WmKind::Timestampl) b.ceiling += ".0000000";
            b.next_watermark = b.ceiling;
            b.as_of_date = FormatDats(read_start_epoch);
            break;
        }
    }
    return b;
}

}  // namespace wm
}  // namespace erpl_rev
