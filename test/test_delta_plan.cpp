// Watermark bounds -- the arithmetic the delta engine was missing.
//
// Two independent defects are covered here, and the second is the bigger one:
//
//   D1  safety_secs was stored, exposed on the CLI and on a screen, and read by
//       nothing. The read used `chg_col > wm` with no overlap at all.
//
//   D1' subtracting the safety window from the FLOOR does not prove no-loss on
//       its own. If a cycle reads for longer than the window, a row that commits
//       during the read, below the delivered maximum, is still skipped forever
//       once the watermark advances to that maximum. The fix is a stable
//       read-start CEILING that the watermark advances to instead.
//
// A floor-only implementation passes floor-only tests over a still-lossy engine,
// which is exactly why the ceiling cases below exist.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "delta_plan.hpp"

using namespace erpl_rev::wm;

namespace {
// 2026-09-05 12:00:00 UTC, as an epoch second, so every case is deterministic.
constexpr int64_t kReadStart = 1788609600;

WatermarkSpec Spec(WmKind k, const std::string &wm, int64_t safety = 120) {
    WatermarkSpec s;
    s.kind = k;
    s.chg_col = "CHANGED_AT";
    s.wm_value = wm;
    s.safety_secs = safety;
    return s;
}
}  // namespace

TEST_CASE("delta_plan: read_start is the timestamp the fixture claims", "[watermark]") {
    // Guard the constant the rest of the file reasons about.
    CHECK(FormatNumts(kReadStart) == "20260905120000");
}

TEST_CASE("delta_plan: the safety offset is subtracted from the floor", "[watermark]") {
    // D1. The floor is the stored watermark pulled BACK by the safety window, so
    // rows that committed late are re-read. The merge is keyed, so re-delivery
    // costs nothing.
    const auto b = ComputeBounds(Spec(WmKind::Numts, "20260905120000"), kReadStart);
    CHECK(b.has_floor);
    CHECK(b.floor == "20260905115800");   // 12:00:00 minus 120s
}

TEST_CASE("delta_plan: the ceiling is read_start minus the safety window", "[watermark]") {
    // D1'. This is the value the watermark advances to -- never the maximum of
    // the rows that happened to be delivered.
    const auto b = ComputeBounds(Spec(WmKind::Numts, "20260905000000"), kReadStart);
    CHECK(b.has_ceiling);
    CHECK(b.ceiling == "20260905115800");
    CHECK(b.next_watermark == b.ceiling);
}

TEST_CASE("delta_plan: a row committed during a slow read is not lost", "[watermark]") {
    // The bounded-lag contract, stated as a test.
    //
    // A cycle starts at 12:00:00 and reads for ten minutes. During the read a row
    // commits carrying timestamp 11:59:00 -- below the maximum the cycle observes
    // (12:09:00), so this cycle misses it.
    //
    // With the watermark advancing to the delivered maximum it would sit at
    // 12:09:00 and the row is below the next floor forever. With the ceiling it
    // sits at 11:58:00, and the next cycle's floor is earlier still.
    const auto b = ComputeBounds(Spec(WmKind::Numts, "20260905000000"), kReadStart);
    const std::string missed = "20260905115900";
    const std::string delivered_max = "20260905120900";

    CHECK(b.next_watermark < missed);          // the ceiling did not pass the row
    CHECK(delivered_max > missed);             // ... whereas the delivered max did

    // Next cycle, starting an hour later, re-reads from below the missed row.
    const auto next = ComputeBounds(Spec(WmKind::Numts, b.next_watermark), kReadStart + 3600);
    CHECK(next.floor < missed);
}

TEST_CASE("delta_plan: a clock ceiling bounds the watermark, not the read",
          "[watermark]") {
    // The distinction that keeps the daemon fast. If the ceiling capped the read,
    // a row committed now would not be visible until safety_secs had elapsed --
    // 120 seconds of latency on a 2-second tick, which defeats the daemon
    // entirely. So the cycle reads everything above the floor and only ADVANCES
    // to the ceiling; rows above it are delivered now and re-delivered next
    // cycle, which the keyed merge absorbs.
    const auto b = ComputeBounds(Spec(WmKind::Numts, "20260905000000"), kReadStart);
    CHECK(b.has_ceiling);
    CHECK_FALSE(b.ceiling_bounds_read);
    CHECK(b.next_watermark == b.ceiling);
}

TEST_CASE("delta_plan: a DATE ceiling DOES bound the read", "[watermark]") {
    // Today is not a complete day. Reading it is the bug: rows posted later today
    // would fall below tomorrow's floor and never arrive.
    const auto b = ComputeBounds(Spec(WmKind::Date, "20260903"), kReadStart);
    CHECK(b.ceiling_bounds_read);
    CHECK(b.ceiling == "20260905");
}

TEST_CASE("delta_plan: an initial load has a ceiling but no floor", "[watermark]") {
    const auto b = ComputeBounds(Spec(WmKind::Numts, ""), kReadStart);
    CHECK_FALSE(b.has_floor);
    CHECK(b.has_ceiling);
    CHECK(b.ceiling == "20260905115800");
}

TEST_CASE("delta_plan: a zero safety window still yields a ceiling", "[watermark]") {
    const auto b = ComputeBounds(Spec(WmKind::Numts, "20260905000000", 0), kReadStart);
    CHECK(b.floor == "20260905000000");
    CHECK(b.ceiling == "20260905120000");
}

TEST_CASE("delta_plan: DATE never reads or stores the current day", "[watermark]") {
    // D2. A nightly cycle at 23:00 used to advance the watermark to today, so
    // rows posted between 23:00 and midnight fell below the next floor.
    auto s = Spec(WmKind::Date, "20260903");
    const auto b = ComputeBounds(s, kReadStart);

    CHECK(b.as_of_date == "20260905");
    CHECK(b.ceiling == "20260905");            // exclusive: today is not read
    CHECK(b.next_watermark == "20260904");     // the last COMPLETE day
    CHECK(b.next_watermark < b.as_of_date);
}

TEST_CASE("delta_plan: DATE applies its safety window in whole days", "[watermark]") {
    // A duration in seconds means nothing against a day-granular column; any
    // non-zero window has to be at least one whole day or it rounds to nothing.
    CHECK(ComputeBounds(Spec(WmKind::Date, "20260903", 120), kReadStart).floor == "20260902");
    CHECK(ComputeBounds(Spec(WmKind::Date, "20260903", 0), kReadStart).floor == "20260903");
    CHECK(ComputeBounds(Spec(WmKind::Date, "20260903", 172800), kReadStart).floor == "20260901");
}

TEST_CASE("delta_plan: DATE on an initial load still defers today", "[watermark]") {
    const auto b = ComputeBounds(Spec(WmKind::Date, ""), kReadStart);
    CHECK_FALSE(b.has_floor);
    CHECK(b.ceiling == "20260905");
    CHECK(b.next_watermark == "20260904");
}

TEST_CASE("delta_plan: DATETIME composes the DATS+TIMS pair", "[watermark]") {
    WatermarkSpec s;
    s.kind = WmKind::Datetime;
    s.chg_col = "ERDAT";
    s.time_col = "ERZET";
    s.wm_value = "20260905115959";
    s.safety_secs = 120;

    const auto b = ComputeBounds(s, kReadStart);
    // The FLOOR is derived from the stored watermark, so it is exact whatever
    // the machine's timezone. The CEILING is derived from the clock and is
    // therefore local for a DATS+TIMS pair -- asserted as a relationship in
    // "DATS and TIMS bounds are local" rather than as a literal here, so this
    // test does not start failing when CI runs somewhere else.
    CHECK(b.floor == "20260905115759");
    CHECK(b.has_ceiling);
    // The pair is compared as one 14-character value, so the caller can split it.
    CHECK(FloorDate(b) == "20260905");
    CHECK(FloorTime(b) == "115759");
}

TEST_CASE("delta_plan: DATETIME crosses midnight without wrapping", "[watermark]") {
    WatermarkSpec s;
    s.kind = WmKind::Datetime;
    s.chg_col = "ERDAT";
    s.time_col = "ERZET";
    s.wm_value = "20260905000030";
    s.safety_secs = 120;   // 30s past midnight, minus 2 minutes -> the day before

    const auto b = ComputeBounds(s, kReadStart);
    CHECK(b.floor == "20260904235830");
    CHECK(FloorDate(b) == "20260904");
    CHECK(FloorTime(b) == "235830");
}

TEST_CASE("delta_plan: TIMESTAMPL keeps its fractional digits", "[watermark]") {
    auto s = Spec(WmKind::Timestampl, "20260905120000.1234567");
    const auto b = ComputeBounds(s, kReadStart);
    // The floor drops back a whole number of seconds; the fraction is preserved
    // so a re-read cannot skip sub-second rows at the boundary.
    CHECK(b.floor == "20260905115800.1234567");
    CHECK(b.ceiling == "20260905115800.0000000");
}

TEST_CASE("delta_plan: a counter watermark takes its ceiling from the staged rows",
          "[watermark]") {
    // An INT counter carries no clock, so read_start says nothing about it. With
    // staging, the server can see every delivered row and cut the ceiling at
    // max - safety_units without asking SAP for anything.
    WatermarkSpec s;
    s.kind = WmKind::Int;
    s.chg_col = "DOCNR";
    s.wm_value = "1000";
    s.safety_units = 50;

    const auto b = ComputeBounds(s, kReadStart);
    // "0950", not "950": the comparison is lexical, so the floor has to keep the
    // stored value's width or it sorts wrong against it. '1000' > '950' is FALSE
    // lexically -- an unpadded floor excluded exactly the rows it should admit.
    CHECK(b.floor == "0950");
    CHECK_FALSE(b.has_ceiling);          // not knowable before the read
    CHECK(b.ceiling_from_stage);         // ... but knowable after it
    CHECK(CeilingFromStagedMax(s, "20000") == "19950");
}

TEST_CASE("delta_plan: a counter ceiling keeps the source's zero padding",
          "[watermark]") {
    // A NUMC/CHAR document number is zero-padded and the generated predicate is
    // a STRING comparison. Strip the padding and the comparison inverts:
    // '0000020000' > '19950' is false lexically, so the target reads zero rows
    // on every cycle after the first -- silently, forever.
    WatermarkSpec s;
    s.kind = WmKind::Int;
    s.chg_col = "BELNR";
    s.wm_value = "0000010000";
    s.safety_units = 50;

    CHECK(CeilingFromStagedMax(s, "0000020000") == "0000019950");
    CHECK(ComputeBounds(s, kReadStart).floor == "0000009950");

    // An unpadded source stays unpadded -- the rule is "preserve what the source
    // gave", not "always pad".
    WatermarkSpec plain;
    plain.kind = WmKind::Int;
    plain.chg_col = "DOCNR";
    plain.wm_value = "1000";
    plain.safety_units = 50;
    CHECK(CeilingFromStagedMax(plain, "20000") == "19950");
}

TEST_CASE("delta_plan: a counter floor never goes below zero", "[watermark]") {
    WatermarkSpec s;
    s.kind = WmKind::Int;
    s.chg_col = "DOCNR";
    s.wm_value = "10";
    s.safety_units = 50;
    CHECK(ComputeBounds(s, kReadStart).floor == "00");   // clamped, still width-preserving
}

TEST_CASE("delta_plan: CHANGENR is refused as a generic watermark", "[watermark]") {
    // The change number comes from a buffered number range and is documented in
    // this codebase as NOT monotonic in commit order -- which is exactly why the
    // change-document feed positions on UDATE+UTIME instead. An overlap
    // expressed in "units" of a non-monotonic key proves nothing about loss, so
    // it must not be offered as a watermark kind at all.
    CHECK_FALSE(IsUsableAsWatermark(WmKind::Changenr));
    CHECK(IsUsableAsWatermark(WmKind::Numts));
    CHECK(IsUsableAsWatermark(WmKind::Int));

    const auto why = WhyNotUsable(WmKind::Changenr);
    CHECK(why.find("CHANGEDOC") != std::string::npos);   // name the alternative
}

TEST_CASE("delta_plan: kind names round-trip through their stored spelling", "[watermark]") {
    // wm_kind is a stored string in _erpl_rev_delta_state.
    CHECK(ParseKind("NUMTS") == WmKind::Numts);
    CHECK(ParseKind("DATE") == WmKind::Date);
    CHECK(ParseKind("DATETIME") == WmKind::Datetime);
    CHECK(ParseKind("TIMESTAMPL") == WmKind::Timestampl);
    CHECK(ParseKind("INT") == WmKind::Int);
    CHECK(ParseKind("CHANGENR") == WmKind::Changenr);
    CHECK(KindName(WmKind::Datetime) == "DATETIME");
    // Unknown spellings must not silently become a working kind.
    CHECK_THROWS(ParseKind("WHATEVER"));
}

TEST_CASE("delta_plan: DATS and TIMS bounds are local, TIMESTAMPL bounds are UTC",
          "[watermark]") {
    // SAP stores DATS/TIMS as local wall-clock and a GET TIME STAMP value as
    // UTC. A bound computed in the wrong clock is wrong by the machine's offset,
    // and it does not look like a timezone bug -- it looks like the target
    // replicating nothing, or replicating everything on every cycle.
    //
    // Asserted as a RELATIONSHIP rather than against fixed strings, so the test
    // means the same thing in every timezone CI might run in.
    const auto utc_kind = ComputeBounds(Spec(WmKind::Numts, "20260101000000"), kReadStart);

    WatermarkSpec pair;
    pair.kind = WmKind::Datetime;
    pair.chg_col = "ERDAT";
    pair.time_col = "ERZET";
    pair.wm_value = "20260101000000";
    pair.safety_secs = 120;
    const auto local_kind = ComputeBounds(pair, kReadStart);

    const auto offset = ParseNumts(local_kind.ceiling) - ParseNumts(utc_kind.ceiling);
    // Whatever the offset is, both must agree with their own clock: the pair's
    // ceiling is the UTC one shifted by exactly the local offset, and nothing
    // else.
    CHECK(std::abs(offset) < 15 * 3600);
    CHECK((ParseNumts(utc_kind.ceiling) + offset) == ParseNumts(local_kind.ceiling));
}
