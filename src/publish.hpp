// Publishing, subscriptions and change-log retention.
//
// A subscription reads one target's change log from an offset, publishes what it
// finds, and advances -- in ONE transaction, so a failed publish cannot leave the
// offset claiming rows that never landed.
//
// Server-side, not through ABAP: a subscription publish touches no SAP data, so
// routing it through an RFC round trip would mean travelling to SAP to ask DuckDB
// to copy one of its own tables -- and it could not share a transaction with the
// offset advance. zcl_erpl_rev_util=>publish keeps its own path for the ad-hoc
// and GUI cases; the SQL generation is shared, the call path is not.
#pragma once

#include <duckdb.hpp>

#include <string>

namespace erpl_rev {

enum class SinkKind { Parquet, Table };
enum class SinkMode { Full, Append };

struct Sink {
    SinkKind kind = SinkKind::Parquet;
    SinkMode mode = SinkMode::Full;
    std::string dest;
    std::string partition_by;
};

// The COPY / INSERT / CREATE TABLE AS for one sink. Shared with the ABAP path so
// the two cannot produce different SQL for the same sink.
std::string PublishSql(const Sink &sink, const std::string &source_relation);

// "PARQUET:/path:FULL" / "TABLE:lake.main.t:APPEND"
Sink ParseSink(const std::string &spec);

struct AdvanceResult {
    long long published = 0;
    long long new_offset = 0;
};

void CreateSubscription(duckdb::Connection &con, const std::string &name,
                        const std::string &target, const std::string &sink_spec);

// Publish everything past the offset and advance it, atomically.
AdvanceResult Advance(duckdb::Connection &con, const std::string &name);

// Delete log rows every subscription has passed AND that are older than the
// window. Returns how many were removed.
long long Retain(duckdb::Connection &con, const std::string &target, long long window_secs);

}  // namespace erpl_rev
