// Mass-execution split strategies.
//
// A portion list is generated here, before any worker starts, and persisted --
// that is what makes a failed mass run restartable, because the coordinator can
// see which portions finished.
//
// ABAP is a data provider, not a decision maker. A fiscal-year variant lives in
// SAP and a key histogram needs a GROUP BY against the source; both arrive here
// as data, and every strategy is then cut by the same code. That is what stops a
// second, subtly different implementation of "where do the boundaries go".
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace split {

enum class Strategy { Key, Records, Size, Time, Fiscal, List };

struct Bucket {
    std::string value;   // a distinct value (or prefix) of the partition column
    long long rows = 0;
};

struct Range {
    std::string from, to;
};

struct SplitRequest {
    Strategy strategy = Strategy::Key;
    std::string part_col;
    std::string user_where;

    // Records / Size
    long long limit_rows = 0;
    long long limit_mb = 0;
    long long bytes_per_row = 0;
    std::vector<Bucket> histogram;

    // The fallback when no histogram is available: the partition column's
    // bounds and the total row count, which is what a scalar MIN/MAX read can
    // supply. A per-value histogram is better -- it cuts on where the rows
    // actually are rather than assuming they are evenly spread -- so it wins
    // when present. These exist because the ABAP side has no proven pattern for
    // a multi-row dynamic SELECT, and inventing one dumped the work process.
    std::string range_min, range_max;
    long long total_rows = 0;

    // Time
    std::string time_unit;   // day | month | year
    std::string time_from, time_to;

    // Fiscal: ranges ABAP resolved from the fiscal-year variant.
    std::vector<Range> periods;

    // List
    std::vector<Range> explicit_ranges;
};

struct Portion {
    int portion_no = 0;
    std::string predicate;
    long long est_rows = 0;
};

std::vector<Portion> PlanSplit(const SplitRequest &req);

}  // namespace split
}  // namespace erpl_rev
