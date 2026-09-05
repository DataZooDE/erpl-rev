// Control-schema versioning and forward migration.
//
// Every milestone of the parity work adds columns. Without a versioned,
// idempotent migration list, the first added column strands every DuckDB file
// already in the field -- and that is unrecoverable, because the upgrade path
// can no longer be written retroactively.
//
// test/fixtures/control_schema_v1.duckdb.gz is a file the PRE-change binary
// produced (four control tables, one view, two sequences, no version table),
// seeded with rows so these tests prove data SURVIVES rather than merely that
// the DDL runs.

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>
#include <zlib.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "control_schema.hpp"

using namespace erpl_rev;

namespace {

// The fixture is stored gzipped (2.6 MB -> 10 KB). Unpack it to a throwaway
// path; each test gets its own copy, because migrating mutates it.
std::string UnpackV1Fixture(const std::string &tag) {
    const std::string src = std::string(ERPL_REV_TEST_FIXTURE_DIR) + "/control_schema_v1.duckdb.gz";
    const std::string dst = "/tmp/erpl_rev_v1_" + tag + "_" + std::to_string(::getpid()) + ".duckdb";
    std::remove(dst.c_str());
    std::remove((dst + ".wal").c_str());

    gzFile in = gzopen(src.c_str(), "rb");
    REQUIRE(in != nullptr);
    std::ofstream out(dst, std::ios::binary);
    REQUIRE(out.good());
    std::vector<char> buf(1 << 16);
    for (;;) {
        int n = gzread(in, buf.data(), static_cast<unsigned>(buf.size()));
        REQUIRE(n >= 0);
        if (n == 0) break;
        out.write(buf.data(), n);
    }
    gzclose(in);
    out.close();
    return dst;
}

std::string Scalar(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    REQUIRE_FALSE(r->HasError());
    if (r->RowCount() == 0) return "";
    return r->GetValue(0, 0).IsNull() ? "" : r->GetValue(0, 0).ToString();
}

bool HasColumn(duckdb::Connection &con, const std::string &table, const std::string &col) {
    return Scalar(con, "SELECT count(*) FROM duckdb_columns() WHERE lower(table_name)=lower('" +
                           table + "') AND lower(column_name)=lower('" + col + "')") == "1";
}

}  // namespace

TEST_CASE("control_schema: the v1 fixture is genuinely pre-versioning", "[schema]") {
    // If this ever fails, someone regenerated the fixture from newer code and
    // the whole upgrade proof below is worthless.
    const auto path = UnpackV1Fixture("pre");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);

    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() "
                      "WHERE table_name='_erpl_rev_schema_version'") == "0");
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_delta_state") == "2");
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_cdc") == "2");
    CHECK(Scalar(con, "SELECT mode FROM _erpl_rev_cdc WHERE target='SFLIGHT'") == "FULL_IUD");
}

TEST_CASE("control_schema: an unversioned file migrates without losing data", "[schema]") {
    const auto path = UnpackV1Fixture("mig");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);

    schema::Migrate(con, "test");

    // Version recorded, and recorded as history: which binary applied what, when.
    CHECK(schema::CurrentVersion(con) == schema::LatestVersion());
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_schema_version WHERE version=1") == "1");
    CHECK(Scalar(con, "SELECT binary_version FROM _erpl_rev_schema_version WHERE version=1") == "test");

    // Every seeded row still there, with its values intact.
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_delta_state") == "2");
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state WHERE target='ZDELTA_WM'") ==
          "20260901120000");
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_run_stats") == "1");
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_cli_cmd") == "1");
    CHECK(Scalar(con, "SELECT position FROM _erpl_rev_cdc WHERE target='SFLIGHT'") == "42");
}

TEST_CASE("control_schema: v2 adds the columns the delta engine needs", "[schema]") {
    const auto path = UnpackV1Fixture("v2");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    for (const char *c : {"time_col", "safety_units", "active_run_id", "load_type_default",
                          "fail_count", "parked_until", "park_reason", "max_cycle_secs",
                          "log_enabled", "log_full_loads", "xform_view", "validation_policy"})
        CHECK(HasColumn(con, "_erpl_rev_delta_state", c));
}

TEST_CASE("control_schema: v3 rewrites the stored FULL_IUD mode value", "[schema]") {
    // The trigger mode is a stored string. Renaming the enum in C++ without
    // rewriting stored values leaves the dialect meeting a value it no longer
    // recognises, on exactly the systems that have been running longest.
    const auto path = UnpackV1Fixture("v3");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    CHECK(Scalar(con, "SELECT mode FROM _erpl_rev_cdc WHERE target='SFLIGHT'") == "IMAGE_IUD");
    // Untouched modes stay untouched.
    CHECK(Scalar(con, "SELECT mode FROM _erpl_rev_cdc WHERE target='ZDELTA_WM'") == "DELETE_ONLY");
    // And the position is NOT reset -- a rename must never re-seed a trigger set.
    CHECK(Scalar(con, "SELECT position FROM _erpl_rev_cdc WHERE target='SFLIGHT'") == "42");

    for (const char *c : {"trigger_table", "last_probe_ts", "shadow_rows", "shadow_oldest_ts",
                          "tuning"})
        CHECK(HasColumn(con, "_erpl_rev_cdc", c));
}

TEST_CASE("control_schema: migrating twice is a no-op", "[schema]") {
    const auto path = UnpackV1Fixture("idem");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);

    schema::Migrate(con, "test");
    const auto after_first = Scalar(con, "SELECT count(*) FROM _erpl_rev_schema_version");

    // Re-running the whole list must not throw (a bare ALTER TABLE ADD COLUMN
    // would) and must not double the history.
    REQUIRE_NOTHROW(schema::Migrate(con, "test"));
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_schema_version") == after_first);
    CHECK(Scalar(con, "SELECT count(*) FROM _erpl_rev_delta_state") == "2");
}

TEST_CASE("control_schema: a newer file is refused, not half-read", "[schema]") {
    // The worst failure mode in the set: an older binary opening a file whose
    // columns it does not know, then failing deep inside some later query.
    const auto path = UnpackV1Fixture("fwd");
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    con.Query("INSERT INTO _erpl_rev_schema_version VALUES (" +
              std::to_string(schema::LatestVersion() + 5) + ", 'from the future', now(), 'vnext')");

    REQUIRE_THROWS_AS(schema::Migrate(con, "test"), std::runtime_error);
    try {
        schema::Migrate(con, "test");
    } catch (const std::runtime_error &e) {
        const std::string msg = e.what();
        // The message has to name both versions, or the operator cannot act on it.
        CHECK(msg.find(std::to_string(schema::LatestVersion() + 5)) != std::string::npos);
        CHECK(msg.find(std::to_string(schema::LatestVersion())) != std::string::npos);
    }
}

TEST_CASE("control_schema: a fresh file gets the same shape as a migrated one", "[schema]") {
    // Two ways to reach the current schema -- create from scratch, or migrate an
    // old file. They must agree, or field systems and CI diverge silently.
    const auto migrated = UnpackV1Fixture("shape");
    duckdb::DuckDB db1(migrated);
    duckdb::Connection c1(db1);
    schema::Migrate(c1, "test");

    duckdb::DuckDB db2(nullptr);  // in-memory, fresh
    duckdb::Connection c2(db2);
    schema::Migrate(c2, "test");

    const char *sql =
        "SELECT table_name || '.' || column_name || ':' || data_type FROM duckdb_columns() "
        "WHERE table_name LIKE '\\_erpl\\_rev\\_%' ESCAPE '\\' ORDER BY 1";
    auto a = c1.Query(sql);
    auto b = c2.Query(sql);
    REQUIRE_FALSE(a->HasError());
    REQUIRE_FALSE(b->HasError());
    REQUIRE(a->RowCount() == b->RowCount());
    for (size_t i = 0; i < a->RowCount(); ++i)
        CHECK(a->GetValue(0, i).ToString() == b->GetValue(0, i).ToString());
}
