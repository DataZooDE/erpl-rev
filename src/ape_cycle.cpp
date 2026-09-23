// See ape_cycle.hpp for the contract.

#include "ape_cycle.hpp"

#include <cctype>
#include <map>

#include "ape_decode.hpp"
#include "ape_envelope.hpp"
#include "duckdb_bridge.hpp"
#include "json_util.hpp"
#include "sxml_binary.hpp"

namespace erpl_rev {
namespace ape {
namespace {

std::string BatchId(long long batch_index) {
    return "batch " + std::to_string(batch_index);
}

std::string Upper(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string ApeStagingName(const std::string &target) { return target + "__apesnap"; }

ApeApplyResult ApeApplyFullPackage(DuckDbBridge &db, const std::string &target,
                                   const std::string &package_json,
                                   long long batch_index) {
    ApeApplyResult res;
    res.batch_index = batch_index;
    const std::string staging = ApeStagingName(target);
    // The target must already exist: ABAP seeds it from the DDIC describe
    // before the scan (the same seed contract replicate() honours, HLD §4).
    try {
        db.Query("SELECT * FROM " + target + " LIMIT 0");
    } catch (const std::exception &e) {
        throw ApeDecodeError("APE FULL " + BatchId(batch_index) + ": unknown target '" +
                             target + "': " + e.what());
    }
    if (batch_index == 0) {
        db.Execute("DROP TABLE IF EXISTS " + staging);
        db.Execute("CREATE TABLE " + staging + " AS SELECT * FROM " + target + " LIMIT 0");
    } else {
        // A later batch without a stage means the scan lost its staging
        // (server restart mid-scan): refuse, do not rebuild and silently
        // merge a partial snapshot.
        try {
            db.Query("SELECT * FROM " + staging + " LIMIT 0");
        } catch (const std::exception &e) {
            throw ApeDecodeError("APE FULL " + BatchId(batch_index) + ": staging '" +
                                 staging + "' is missing, the scan must restart at batch 0: " +
                                 e.what());
        }
    }

    const ApeEnvelope env = ExtractEnvelope(package_json.c_str());
    res.last_batch = env.last_batch;
    if (!env.has_data) return res;

    const auto rows = DecodeCsv(env.body, env.fields.size(), BatchId(batch_index));
    size_t op = kNoOperation;
    try {
        op = FindField(env.fields, "/1DH/OPERATION");
    } catch (const ApeDecodeError &) {
        // A source without the indicator: every row is an after-image.
    }

    // Staging column order, so the BXML insert needs no name lookup downstream.
    std::vector<std::string> cols;
    for (const auto &c : db.Query("SELECT * FROM " + staging + " LIMIT 0").columns)
        cols.push_back(c.name);
    std::vector<size_t> pos;
    pos.reserve(cols.size());
    for (const auto &c : cols) {
        try {
            pos.push_back(FindField(env.fields, c));
        } catch (const ApeDecodeError &) {
            // Staging NULLs would silently wipe the column on merge: refuse
            // with the column named (FR-7), never shift or null-fill.
            throw ApeDecodeError("APE FULL " + BatchId(batch_index) + ": package has no " +
                                 "column '" + Upper(c) + "' for target '" + target + "'");
        }
    }

    sxml::Table tbl;
    tbl.columns = cols;
    for (size_t r = 0; r < rows.size(); r++) {
        // A delete image is keys-only: upserting it would plant a half-NULL
        // row over live data, so a FULL scan carrying one is refused.
        if (ClassifyRow(rows[r], op) == RowOp::Delete)
            throw ApeDecodeError("APE FULL " + BatchId(batch_index) + " row " +
                                 std::to_string(r) +
                                 ": /1DH/OPERATION='D' is not an after-image; a FULL " +
                                 "scan carries upserts only");
        std::vector<std::string> out;
        out.reserve(cols.size());
        for (size_t i : pos) out.push_back(rows[r][i]);
        tbl.rows.push_back(std::move(out));
    }
    res.rows_staged =
        db.IngestBxml(staging, sxml::Encode("DATA", tbl), IngestMode::Insert, {}, "", "", "");
    return res;
}

ApeFullCounts ApeFinalizeFull(DuckDbBridge &db, const std::string &target,
                              const std::vector<std::string> &keys) {
    const SnapshotResult r = db.SnapshotMerge(target, ApeStagingName(target), keys);
    return {r.ins, r.upd, r.del};
}

namespace {

// SQL string literal. Package envelopes carry SAP text, so a quote in the
// data must not break the spill INSERT.
std::string SqlQ(const std::string &s) {
    std::string out = "'";
    for (char c : s) out += (c == '\'' ? "''" : std::string(1, c));
    return out + "'";
}

void EnsureTarget(DuckDbBridge &db, const std::string &target, const std::string &what) {
    try {
        db.Query("SELECT * FROM " + target + " LIMIT 0");
    } catch (const std::exception &e) {
        throw ApeDecodeError("APE " + what + ": unknown target '" + target + "': " + e.what());
    }
}

// The merge position: the last batch merged, -1 when nothing was. Missing
// registration is refused -- position belongs to a registered target.
long long SpillPosition(DuckDbBridge &db, const std::string &target, const std::string &what) {
    QueryResult qr;
    try {
        qr = db.Query("SELECT coalesce(spill_batch,-1) AS spill_batch "
                      "FROM _erpl_rev_delta_state WHERE target=" + SqlQ(target));
    } catch (const std::exception &e) {
        throw ApeDecodeError("APE " + what + ": state read failed for '" + target +
                             "': " + e.what());
    }
    if (qr.rows.empty())
        throw ApeDecodeError("APE " + what + ": no delta registration for '" + target + "'");
    const auto rows = json::ParseRows("[" + qr.rows[0] + "]");
    if (rows.empty() || rows[0].empty())
        throw ApeDecodeError("APE " + what + ": unreadable position for '" + target + "'");
    return std::atoll(rows[0][0].value.c_str());
}

// Decode one envelope and merge its rows onto the target (U/blank = upsert
// the after-image, D = delete by key). Shared by the live path (which spills
// first) and the replay path (whose spill already exists).
ApeDeltaCounts MergeDeltaEnvelope(DuckDbBridge &db, const std::string &target,
                                  const std::vector<std::string> &keys,
                                  const std::string &package_json,
                                  const std::string &batch_id) {
    ApeDeltaCounts counts;
    const ApeEnvelope env = ExtractEnvelope(package_json.c_str());
    if (!env.has_data) return counts;
    const auto rows = DecodeCsv(env.body, env.fields.size(), batch_id);
    size_t op;
    try {
        op = FindField(env.fields, "/1DH/OPERATION");
    } catch (const ApeDecodeError &) {
        // Replication always carries the indicator (protocol §13): without
        // it no row can be classified, so refuse rather than guess.
        throw ApeDecodeError("APE DELTA " + batch_id + ": package has no " +
                             "'/1DH/OPERATION' column; replication packages always carry it");
    }

    const auto desc = db.Query("SELECT * FROM " + target + " LIMIT 0").columns;
    std::vector<std::string> cols;
    cols.reserve(desc.size());
    for (const auto &c : desc) cols.push_back(c.name);
    std::vector<size_t> pos;
    pos.reserve(cols.size());
    for (const auto &c : cols) {
        try {
            pos.push_back(FindField(env.fields, c));
        } catch (const ApeDecodeError &) {
            throw ApeDecodeError("APE DELTA " + batch_id + ": package has no " +
                                 "column '" + Upper(c) + "' for target '" + target + "'");
        }
    }

    // Split first, merge second. A D image is keys-only with blank non-keys,
    // and blanks do not cast to typed columns -- routing D rows through the
    // typed upsert projection would refuse every package that deletes. So D
    // keys delete by key (keys are always filled), and only after-images
    // (filled cells) ride the typed MERGE. The two steps are not one
    // transaction, but the spill makes that safe: a crash between them
    // replays the whole package, and both halves are idempotent.
    std::vector<std::vector<std::string>> urows, dkeys;
    for (size_t r = 0; r < rows.size(); r++) {
        if (ClassifyRow(rows[r], op) == RowOp::Delete) {
            std::vector<std::string> key;
            for (const auto &k : keys) {
                size_t ki = 0;
                for (; ki < cols.size(); ki++)
                    if (Upper(cols[ki]) == Upper(k)) break;
                if (ki >= cols.size())
                    throw ApeDecodeError("APE DELTA " + batch_id + ": key '" + Upper(k) +
                                         "' is not a target column of '" + target + "'");
                key.push_back(rows[r][pos[ki]]);
            }
            dkeys.push_back(std::move(key));
            counts.deleted++;
        } else {
            urows.push_back(rows[r]);
            counts.upserted++;
        }
    }
    if (!dkeys.empty()) {
        std::map<std::string, std::string, std::less<>> types;
        for (const auto &c : desc) types[Upper(c.name)] = c.type;
        std::string list = keys.size() > 1 ? "(" : "";
        for (size_t i = 0; i < keys.size(); i++) list += (i ? "," : "") + keys[i];
        if (keys.size() > 1) list += ")";
        std::string vals;
        for (const auto &key : dkeys) {
            vals += std::string(vals.empty() ? "" : ",") + "(";
            for (size_t i = 0; i < keys.size(); i++) {
                auto it = types.find(Upper(keys[i]));
                const std::string &ty = it != types.end() ? it->second : "VARCHAR";
                vals += std::string(i ? "," : "") + "CAST(" + SqlQ(key[i]) + " AS " + ty + ")";
            }
            vals += ")";
        }
        db.Execute("DELETE FROM " + target + " WHERE " + list + " IN (VALUES " + vals + ")");
    }
    if (!urows.empty()) {
        // The engine's op name is not a SQL identifier (slashes): carry the
        // verdict in a safe control column instead. IngestBxml strips it as
        // control data, never as a target column.
        sxml::Table tbl;
        tbl.columns = cols;
        tbl.columns.push_back("ape_op");
        for (const auto &row : urows) {
            std::vector<std::string> out;
            out.reserve(cols.size() + 1);
            for (size_t i : pos) out.push_back(row[i]);
            // Blank is an after-image (BR-3). The MERGE below only acts on
            // i/u/d, so a blank passed through would silently skip the row.
            const std::string &cell = row[op];
            out.push_back(cell.empty() ? "U" : cell);
            tbl.rows.push_back(std::move(out));
        }
        db.IngestBxml(target, sxml::Encode("DATA", tbl), IngestMode::Merge, keys, "", "", "",
                      "ape_op");
    }
    counts.rows_applied = static_cast<long long>(rows.size());
    return counts;
}

}  // namespace

ApeDeltaCounts ApeApplyDeltaPackage(DuckDbBridge &db, const std::string &target,
                                    const std::vector<std::string> &keys,
                                    const std::string &package_json,
                                    long long batch_index) {
    const std::string batch_id = BatchId(batch_index);
    EnsureTarget(db, target, "DELTA " + batch_id);
    const ApeEnvelope probe = ExtractEnvelope(package_json.c_str());
    if (!probe.has_data) return {};
    // No silent restarts: a batch at/below the merged position means the
    // cycle counter restarted at 0 while spill_batch survived from an older
    // generation. Merging would strand a kill between spill and merge
    // (RECOVER replays batch_index > position, i.e. nothing), so refuse
    // loudly. Retries are unaffected: a failed batch never advances the
    // position, so its re-send still counts past it.
    const long long pos = SpillPosition(db, target, "DELTA " + batch_id);
    if (batch_index <= pos)
        throw ApeDecodeError("APE DELTA " + batch_id + ": stale batch index " +
                             std::to_string(batch_index) + " at/below merged position " +
                             std::to_string(pos) + " for '" + target +
                             "'; the cycle must count past spill_batch, never restart at 0");
    // Structural validation before the side-effecting spill: a package that
    // can never merge (no indicator to classify by) is refused up front, so
    // it neither spills nor poisons the recover queue. Row-level failures
    // below still spill-then-fail, which is exactly what recover replays.
    try {
        FindField(probe.fields, "/1DH/OPERATION");
    } catch (const ApeDecodeError &) {
        throw ApeDecodeError("APE DELTA " + batch_id + ": package has no " +
                             "'/1DH/OPERATION' column; replication packages always carry it");
    }
    // The crash copy FIRST (ADR-2): a kill between here and the merge below
    // must find this exact envelope in the spill. Re-spilling the same batch
    // overwrites, so a retried post cannot duplicate.
    try {
        db.Execute("INSERT INTO _erpl_rev_ape_spill(target, batch_index, payload) VALUES (" +
                   SqlQ(target) + ", " + std::to_string(batch_index) + ", " +
                   SqlQ(package_json) + ") ON CONFLICT(target, batch_index) DO UPDATE SET " +
                   "payload=excluded.payload, spilled_ts=now()");
    } catch (const std::exception &e) {
        throw ApeDecodeError("APE DELTA " + batch_id + ": spill failed for '" + target +
                             "': " + e.what());
    }
    ApeDeltaCounts counts =
        MergeDeltaEnvelope(db, target, keys, package_json, "batch " + std::to_string(batch_index));
    db.Execute("UPDATE _erpl_rev_delta_state SET spill_batch=" + std::to_string(batch_index) +
               " WHERE target=" + SqlQ(target));
    return counts;
}

ApeDeltaCounts ApeRecover(DuckDbBridge &db, const std::string &target,
                          const std::vector<std::string> &keys) {
    EnsureTarget(db, target, "RECOVER");
    const long long pos = SpillPosition(db, target, "RECOVER");
    QueryResult qr = db.Query("SELECT batch_index, payload FROM _erpl_rev_ape_spill WHERE target=" +
                              SqlQ(target) + " AND batch_index > " + std::to_string(pos) +
                              " ORDER BY batch_index");
    if (qr.rows.empty()) return {};
    std::string arr = "[";
    for (size_t i = 0; i < qr.rows.size(); i++) arr += (i ? "," : "") + qr.rows[i];
    arr += "]";

    ApeDeltaCounts total;
    long long top = pos;
    for (const auto &row : json::ParseRows(arr)) {
        long long batch = pos;
        std::string payload;
        for (const auto &cell : row) {
            if (cell.key == "batch_index") batch = std::atoll(cell.value.c_str());
            if (cell.key == "payload") payload = cell.value;
        }
        const auto c = MergeDeltaEnvelope(db, target, keys, payload,
                                          "spilled batch " + std::to_string(batch));
        total.rows_applied += c.rows_applied;
        total.upserted += c.upserted;
        total.deleted += c.deleted;
        if (batch > top) top = batch;
    }
    db.Execute("UPDATE _erpl_rev_delta_state SET spill_batch=" + std::to_string(top) +
               " WHERE target=" + SqlQ(target));
    db.Execute("DELETE FROM _erpl_rev_ape_spill WHERE target=" + SqlQ(target) +
               " AND batch_index <= " + std::to_string(top));
    return total;
}

void ApePurge(DuckDbBridge &db, const std::string &target) {
    EnsureTarget(db, target, "PURGE");
    db.Execute("DELETE FROM _erpl_rev_ape_spill WHERE target=" + SqlQ(target));
    db.Execute("UPDATE _erpl_rev_delta_state SET spill_batch=-1 WHERE target=" +
               SqlQ(target));
}

}  // namespace ape
}  // namespace erpl_rev
