// Watermark bounds for the delta engine.
//
// A cycle reads a half-open window (floor, ceiling] of the change column. Both
// ends matter, and the second one is the part that was missing:
//
//   floor    the stored watermark pulled BACK by the safety window, so rows that
//            committed late are re-read. The merge is keyed, so re-delivery is
//            free -- it shows up as rows_read > rows_applied and nothing else.
//
//   ceiling  a value the cycle is confident everything below has committed by.
//            For a clock-based column that is (read start - safety window). The
//            watermark advances to the CEILING, never to the maximum of the rows
//            that happened to be delivered: a row committing during a slow read,
//            below that maximum, would otherwise fall below the next floor and
//            be lost permanently -- which a floor-only overlap does not prevent.
//
// A counter column has no clock, so its ceiling is not knowable before the read.
// Because every cycle stages, the server sees the delivered rows and can cut the
// ceiling at (staged max - safety units) at commit time, with no round trip.
//
// Pure: no DuckDB, no RFC, no clock of its own -- the caller passes read_start.
#pragma once

#include <cstdint>
#include <string>

namespace erpl_rev {
namespace wm {

enum class WmKind {
    Numts,       // SAP timestamp YYYYMMDDHHMMSS
    Timestampl,  // YYYYMMDDHHMMSS.fffffff
    Date,        // DATS, day granularity
    Datetime,    // a DATS column plus a TIMS column, compared as one value
    Int,         // a monotonic counter (document number, sequence)
    Changenr,    // NOT usable: see IsUsableAsWatermark
};

struct WatermarkSpec {
    WmKind kind = WmKind::Numts;
    std::string chg_col;
    std::string time_col;    // DATETIME only: the TIMS half
    std::string wm_value;    // stored high-water; empty on an initial load
    int64_t safety_secs = 120;
    int64_t safety_units = 0;  // counter kinds; a duration means nothing there
};

struct Bounds {
    bool has_floor = false;
    bool has_ceiling = false;
    bool ceiling_from_stage = false;  // counter kinds: cut it at commit instead
    std::string floor;
    std::string ceiling;
    std::string next_watermark;  // what to store after a clean cycle
    std::string as_of_date;      // DATE/DATETIME: the day that must not be read
};

Bounds ComputeBounds(const WatermarkSpec &spec, int64_t read_start_epoch);

// Counter kinds: the ceiling once the staged rows are visible.
std::string CeilingFromStagedMax(const WatermarkSpec &spec, const std::string &staged_max);

// DATETIME: split the composed 14-character bound back into its two columns.
std::string FloorDate(const Bounds &b);
std::string FloorTime(const Bounds &b);

bool IsUsableAsWatermark(WmKind k);
std::string WhyNotUsable(WmKind k);

WmKind ParseKind(const std::string &s);
std::string KindName(WmKind k);

std::string FormatNumts(int64_t epoch);
int64_t ParseNumts(const std::string &numts);

}  // namespace wm
}  // namespace erpl_rev
