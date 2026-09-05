// Streaming latency, as a distribution.
//
// NFR-2 promises p95 <= 5s and p99 <= 10s change-to-target. That is a claim
// about a TAIL, so it needs percentiles: a distribution where 99% of changes
// land instantly and 1% take a minute has an excellent mean and violates every
// guarantee in the BRD.
//
// The change log is the instrument. Every logged row carries when the source
// says it changed (_commit_ts) and when erpl-rev applied it (_applied_at), so
// the latency of every replicated change is recoverable afterwards -- no
// separate harness, no sampling, and it works on a customer's system as
// readily as on a test one.
//
// One assumption, stated because it is invisible until it bites: _commit_ts is
// the source's change column parsed as LOCAL time, and _applied_at comes from
// the server's clock. The subtraction is therefore only meaningful while SAP and
// the server agree on a timezone. They usually do -- the server is normally
// beside the system it replicates -- and a disagreement is loud rather than
// subtle: every percentile shifts by exactly the offset, so a p50 of -7200s
// means two timezones, not a fast pipeline.
#pragma once

#include <duckdb.hpp>

#include <string>

namespace erpl_rev {

struct LatencyStats {
    bool has_data = false;
    long long samples = 0;
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, min = 0, max = 0;
    // Composition of the sample, because "1000 changes" means something
    // different when they are all inserts.
    long long inserts = 0, updates = 0, deletes = 0;
};

// Latency over a target's change log, in seconds. window_secs = 0 means all of
// it; otherwise only changes committed within that window, so one bad episode
// an hour ago does not poison the current reading.
LatencyStats GetLatencyStats(duckdb::Connection &con, const std::string &target,
                             long long window_secs = 0);

bool MeetsTarget(const LatencyStats &s, double p95_target, double p99_target);
std::string Describe(const LatencyStats &s);

}  // namespace erpl_rev
