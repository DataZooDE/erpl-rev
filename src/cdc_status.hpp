// Trigger status, derived from a catalogue probe rather than stored.
//
// The registry knows which objects SHOULD exist; a probe reports which do. The
// status is the comparison, and it has to be, because a stored enum cannot
// notice a trigger dropped out of band -- by a system copy, a transport, or a
// DBA. That is the case that matters: a cycle running against a trigger set with
// a missing trigger advances its position past changes nothing captured, and
// nothing later revisits them.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace cdc {

enum class Status { Active, Disabled, Inconsistent };

// What the registry says should exist for one target.
struct Expected {
    std::string log_table;
    std::string seq_name;
    std::vector<std::string> triggers;
};

// What the database says exists.
struct Probe {
    std::vector<std::string> tables;
    std::vector<std::string> sequences;
    std::vector<std::string> enabled_triggers;
    std::vector<std::string> disabled_triggers;
};

struct StatusResult {
    Status status = Status::Active;
    std::vector<std::string> missing;
};

StatusResult Derive(const Expected &want, const Probe &have, bool deactivated);

// Only the missing objects. Re-running the whole provision DDL would recreate
// the shadow table and reset the position, discarding captured changes.
std::vector<std::string> RepairPlan(const StatusResult &s,
                                    const std::vector<std::string> &provision_ddl);

// ZCDC_* objects no registered target claims. Restricted to that namespace on
// purpose: the probe sees the whole schema, and dropping a customer object
// because it was not in our registry would be catastrophic and our fault.
std::vector<std::string> FindOrphans(const std::vector<Expected> &registry, const Probe &have);

std::string Explain(const StatusResult &s);
std::string StatusName(Status s);

}  // namespace cdc
}  // namespace erpl_rev
