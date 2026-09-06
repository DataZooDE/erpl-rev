// Gzip-framed ingest payloads (src/payload.cpp, DataZooDE/erpl-rev#68).
//
// The wire carries no encoding flag: binary sXML starts with the ASCII magic
// "BXML" and gzip with 1F 8B, so the server decides by looking at the payload.
// These tests pin that decision, the error behaviour on a bad stream, and — the
// one that actually matters — that a compressed package ingests to exactly the
// rows the uncompressed one does.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <zlib.h>

#include "duckdb_bridge.hpp"
#include "payload.hpp"
#include "sxml_binary.hpp"

#include "json_util.hpp"

using namespace erpl_rev;

namespace {

// Frame a string the way the ABAP side does: the ASCII magic "ERPZ" followed by
// a RAW DEFLATE stream, which is what cl_abap_gzip=>compress_binary emits (no
// gzip and no zlib header -- verified on the trial).
// `window` selects the framing: -15 raw deflate, 15+16 real gzip.
std::string Deflate(const std::string &in, int window = -15,
                    int level = Z_DEFAULT_COMPRESSION) {
    z_stream zs{};
    REQUIRE(deflateInit2(&zs, level, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    std::vector<char> buf(1 << 16);
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf.data());
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = deflate(&zs, Z_FINISH);
        out.append(buf.data(), buf.size() - zs.avail_out);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return window < 0 ? std::string(kDeflateMagic) + out : out;
}

// A real gzip stream, to prove the alternate framing is still accepted.
std::string Gzip(const std::string &in) { return Deflate(in, 15 + 16); }

// A small package: two columns, three rows.
std::string SamplePackage() {
    sxml::Table t;
    t.columns = {"MANDT", "SGTXT"};
    t.rows = {{"001", "alpha"}, {"001", "beta"}, {"001", "gamma"}};
    return sxml::Encode("ZDEMO", t);
}

} // namespace

TEST_CASE("payload: a framed payload is recognised, sXML is left alone", "[payload]") {
    const std::string raw = SamplePackage();
    REQUIRE(raw.rfind("BXML", 0) == 0);
    REQUIRE_FALSE(IsCompressed(raw));
    REQUIRE(IsCompressed(Deflate(raw)));
    REQUIRE(IsCompressed(Gzip(raw)));

    // Left alone means byte-identical, not merely decodable.
    REQUIRE(MaybeInflate(raw) == raw);
    REQUIRE(MaybeInflate(Deflate(raw)) == raw);
    REQUIRE(MaybeInflate(Gzip(raw)) == raw);
}

TEST_CASE("payload: short and unframed inputs are not mistaken for compressed", "[payload]") {
    REQUIRE_FALSE(IsCompressed(""));
    REQUIRE_FALSE(IsCompressed("\x1f"));       // one byte of the gzip magic
    REQUIRE_FALSE(IsCompressed("ERP"));        // three bytes of ours
    REQUIRE_FALSE(IsCompressed("BXML"));       // an uncompressed package
    REQUIRE(MaybeInflate("") == "");
    REQUIRE(MaybeInflate("\x1f") == "\x1f");
}

TEST_CASE("payload: a corrupt or truncated stream fails as an inflate error", "[payload]") {
    const std::string gz = Deflate(SamplePackage());

    // Truncated mid-stream.
    REQUIRE_THROWS_WITH(MaybeInflate(gz.substr(0, gz.size() / 2)),
                        Catch::Matchers::ContainsSubstring("payload inflate failed"));

    // Corrupt body behind a valid magic. The point of the assertion is the
    // message: a bad payload must not surface later as "sXML: missing BXML
    // magic" from the decoder, which would send anyone debugging it to the
    // wrong layer entirely.
    std::string bad = gz;
    for (size_t i = 10; i < bad.size(); i++) bad[i] = char(0x5A);
    REQUIRE_THROWS_WITH(MaybeInflate(bad),
                        Catch::Matchers::ContainsSubstring("payload inflate failed"));
}

TEST_CASE("payload: trailing data after the stream is rejected", "[payload]") {
    // Stopping at the first Z_STREAM_END and returning would silently drop a
    // second gzip member or any trailing bytes. The sXML decoder reads to EOF
    // without checking that its element stack closed, so a short payload would
    // land as missing rows rather than as an error -- silent data loss.
    const std::string one = Deflate(SamplePackage());
    REQUIRE_THROWS_WITH(MaybeInflate(one + std::string("junk")),
                        Catch::Matchers::ContainsSubstring("trailing data"));

    // Two concatenated gzip members: valid per RFC 1952, but we decode one
    // package per call, so reject rather than quietly return the first.
    const std::string gz = Gzip(SamplePackage());
    REQUIRE_THROWS_WITH(MaybeInflate(gz + gz),
                        Catch::Matchers::ContainsSubstring("payload inflate failed"));
}

TEST_CASE("payload: the inflated-size cap is enforced", "[payload]") {
    // A megabyte of zeros compresses to about a kilobyte -- the shape of a
    // decompression bomb, and the reason MaybeInflate takes a limit at all.
    const std::string bomb = Deflate(std::string(1024 * 1024, '\0'));
    REQUIRE(bomb.size() < 8192);
    REQUIRE_THROWS_WITH(MaybeInflate(bomb, 4096),
                        Catch::Matchers::ContainsSubstring("exceeds the limit"));
    REQUIRE(MaybeInflate(bomb, 2 * 1024 * 1024).size() == 1024 * 1024);
}

TEST_CASE("payload: a compressed package ingests to the same rows as the raw one", "[payload]") {
    const std::string raw = SamplePackage();

    DuckDbBridge db("");
    db.Query("CREATE TABLE t_raw (mandt VARCHAR, sgtxt VARCHAR)");
    db.Query("CREATE TABLE t_gz  (mandt VARCHAR, sgtxt VARCHAR)");

    REQUIRE(db.IngestBxml("t_raw", raw, IngestMode::Insert, {}, "", "", "", "") == 3);
    REQUIRE(db.IngestBxml("t_gz", Deflate(raw), IngestMode::Insert, {}, "", "", "", "") == 3);

    auto r = db.Query("SELECT count(*) FROM t_raw a JOIN t_gz b USING (mandt, sgtxt)");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows.front().find('3') != std::string::npos);

    // And the two tables differ nowhere at all.
    auto d = db.Query("SELECT count(*) FROM ("
                      "  SELECT * FROM t_raw EXCEPT SELECT * FROM t_gz"
                      "  UNION ALL"
                      "  SELECT * FROM t_gz EXCEPT SELECT * FROM t_raw)");
    REQUIRE(d.rows.front().find('0') != std::string::npos);
}

TEST_CASE("payload: binary (BLOB) cells survive the compressed path byte-for-byte",
          "[payload]") {
    // RAW columns were made byte-faithful in #65, trailing zeros included.
    // Compression must not quietly undo that, so pin it here rather than trust it.
    sxml::Table t;
    t.columns = {"K", "B"};
    t.rows = {{"1", std::string("\x01\x02\x00\x00", 4)},
              {"2", std::string("\x00\x00\x00\x00", 4)}};
    const std::string raw = sxml::Encode("ZBIN", t);

    DuckDbBridge db("");
    db.Query("CREATE TABLE b_raw (k VARCHAR, b BLOB)");
    db.Query("CREATE TABLE b_gz  (k VARCHAR, b BLOB)");
    REQUIRE(db.IngestBxml("b_raw", raw, IngestMode::Insert, {}, "", "", "", "") == 2);
    REQUIRE(db.IngestBxml("b_gz", Deflate(raw), IngestMode::Insert, {}, "", "", "", "") == 2);

    auto r = db.Query("SELECT count(*) FROM b_raw a JOIN b_gz b USING (k) "
                      "WHERE a.b IS NOT DISTINCT FROM b.b");
    REQUIRE(r.rows.front().find('2') != std::string::npos);
}

// --- the queue's result column -----------------------------------------------

TEST_CASE("json: a row value containing quotes survives parsing", "[json]") {
    // The CLI read finished commands out of the queue with a hand-rolled
    // extractor that found the first '"' after the key and the next '"' after
    // that. Any result value containing an escaped quote was therefore cut at
    // the first one -- which never showed while results were plain sentences
    // like "registered t000_cli", and truncated every operator command whose
    // result is JSON to the three characters `[{\`.
    //
    // ParseRows was here the whole time and does it properly. Pinning that,
    // because the fix is "use this" and the next hand-rolled parser will look
    // just as reasonable.
    const std::string row =
        R"([{"status":"DONE","result":"[{\"instance_id\":\"a/b/c\",\"ticks\":7}]","error":""}])";
    const auto rows = json::ParseRows(row);
    REQUIRE(rows.size() == 1);

    std::string result;
    for (const auto &c : rows[0]) if (c.key == "result") result = c.value;

    CHECK(result == R"([{"instance_id":"a/b/c","ticks":7}])");
}
