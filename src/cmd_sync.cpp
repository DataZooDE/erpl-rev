// `erpl-rev sync` and `erpl-rev replicate` — the CLI counterparts of the
// Z_ERPL_REV_DELTA and Z_ERPL_REV_REPLICATE reports.
//
// Reading job state is a local question and goes straight to DuckDB. Anything
// that makes SAP *read source data* has to run in SAP, and the only ABAP entry
// point erpl-adt offers is a parameterless class -- so those go through a
// generated, deployed, deleted classrun.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <thread>

#include "abap_codegen.hpp"
#include "abap_skeletons.hpp"
#include "commands.hpp"
#include "load_type.hpp"
#include "db_client.hpp"
#include "json_util.hpp"

namespace erpl_rev::cmd {

std::string BuildParams(const std::vector<std::pair<std::string, std::string>> &kv) {
    std::string j = "{";
    for (const auto &[k, v] : kv) {
        if (j.size() > 1) j += ",";
        j += json::QuoteString(k) + ":" + json::QuoteString(v);
    }
    return j + "}";
}

std::string UnknownFlag(const std::vector<std::string> &args, const std::string &sub) {
    // name, takes_value. A value-taking flag's value is the next word, and a
    // value may itself look like a flag (`--where=--x`), so it is skipped
    // rather than examined.
    struct Spec { const char *name; bool takes_value; };
    static const Spec kReplicate[] = {
        {"--table", true},        {"--target", true},     {"--columns", true},
        {"--where", true},        {"--cds-params", true}, {"--init", true},
        {"--mode", true},         {"--part-col", true},   {"--dest", true},
        {"--partition-by", true}, {"--target-kind", true}, {"--batch", true},
        {"--maxrows", true},      {"--jobs", true},       {"--wait", true},
        {"--parallel", false},    {"--no-verify", false}, {"--no-truncate", false},
        {"--detach", false},
    };
    static const Spec kSyncCreate[] = {
        {"--method", true},   {"--source", true},  {"--keys", true},
        {"--chg-col", true},  {"--wm-kind", true}, {"--wm-value", true},
        {"--cadence", true},  {"--extra", true},   {"--safety-secs", true},
        {"--time-col", true}, {"--safety-units", true},
        {"--log", false},     {"--no-log", false},
        {"--load-type-default", true},
        {"--allow-empty-reload", false}, {"--no-allow-empty-reload", false},
    };
    static const Spec kSyncSchedule[] = {
        {"--every", true}, {"--remove", false},
    };
    // `run` selects which of the four load types the cycle is. Note the value is
    // NOT validated here -- this table only knows flag names -- so the caller
    // checks it against load_type.hpp before anything contacts SAP.
    static const Spec kSyncRun[] = {
        {"--load-type", true},
    };
    static const Spec kSyncSetWm[]  = { {"--wm-value", true} };
    static const Spec kSyncPreview[] = { {"--rows", true} };
    static const Spec kSyncValidate[] = { {"--full", false}, {"--sample-rows", true} };
    static const Spec kDaemon[]     = { {"--tick", true}, {"--workers", true} };
    static const Spec kSub[]        = { {"--target", true}, {"--sink", true},
                                        {"--dedup-keys", true} };
    static const Spec kMass[]       = { {"--split", true}, {"--limit-rows", true},
                                        {"--target", true}, {"--source", true},
                                        {"--limit-mb", true}, {"--time-unit", true},
                                        {"--part-col", true}, {"--restart", true},
                                        {"--jobs", true}, {"--server-group", true} };
    static const Spec kCdc[]        = { {"--target", true}, {"--all", false},
                                        {"--mode", true}, {"--reconcile", false} };
    static const Spec kRetain[]     = { {"--target", true}, {"--window-days", true} };

    const Spec *known = nullptr;
    size_t count = 0;
    if (sub == "replicate")           { known = kReplicate;    count = std::size(kReplicate); }
    else if (sub == "sync create")    { known = kSyncCreate;   count = std::size(kSyncCreate); }
    else if (sub == "sync schedule")  { known = kSyncSchedule; count = std::size(kSyncSchedule); }
    else if (sub == "sync run" || sub == "sync run-due")
                                      { known = kSyncRun;      count = std::size(kSyncRun); }
    else if (sub == "sync set-wm")    { known = kSyncSetWm;    count = std::size(kSyncSetWm); }
    else if (sub == "sync preview")   { known = kSyncPreview;  count = std::size(kSyncPreview); }
    else if (sub == "sync validate") { known = kSyncValidate; count = std::size(kSyncValidate); }
    // unpark takes a target and nothing else, so any flag is unknown.
    else if (sub == "sync unpark")   { known = nullptr;      count = 0; }
    else if (sub.rfind("daemon", 0) == 0) { known = kDaemon; count = std::size(kDaemon); }
    else if (sub.rfind("sub", 0) == 0)    { known = kSub;    count = std::size(kSub); }
    else if (sub == "retain")             { known = kRetain; count = std::size(kRetain); }
    else if (sub.rfind("cdc", 0) == 0)    { known = kCdc;    count = std::size(kCdc); }
    else if (sub.rfind("mass", 0) == 0)   { known = kMass;   count = std::size(kMass); }
    else if (sub.rfind("daemon", 0) == 0)  { known = kDaemon;  count = std::size(kDaemon); }
    else if (sub.rfind("sub", 0) == 0)     { known = kSub;     count = std::size(kSub); }
    else if (sub.rfind("mass", 0) == 0)    { known = kMass;    count = std::size(kMass); }
    else if (sub.rfind("cdc", 0) == 0)     { known = kCdc;     count = std::size(kCdc); }
    else if (sub.rfind("retain", 0) == 0)  { known = kRetain;  count = std::size(kRetain); }
    // ls / show read no flags of their own; count stays 0, so any `--word` at
    // all is unknown -- which is the right answer for them.

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind("--", 0) != 0) continue;   // a positional, e.g. the target
        const Spec *hit = nullptr;
        for (size_t k = 0; k < count; k++)
            if (args[i] == known[k].name) { hit = &known[k]; break; }
        if (!hit) return args[i];
        if (hit->takes_value) i++;
    }
    return {};
}

namespace {

// Ask before changing anything, exactly as setup does. --non-interactive means
// "do not prompt me", never "yes".
int ConsentGate(const Options &o, const std::string &what) {
    if (o.assume_yes) return 0;
    if (cli::IsTty() && !o.non_interactive) {
        if (cli::Confirm(what + "?", false)) return 0;
        std::printf("Aborted; nothing was changed.\n");
        return 1;
    }
    std::printf("Refusing to %s without confirmation: there is no terminal to ask\n"
                "on. Re-run with --yes if that is what you meant, or with --dry-run\n"
                "to see what would happen.\n", what.c_str());
    return 2;
}

// Queue a command as data and let ZCL_ERPL_REV_CLIDRV run it.
//
// This is the path that needs no developer authorisation: the parameters go
// into a DuckDB table as JSON and are read by a class that is already deployed,
// so nothing is generated, created or deleted in SAP. Falls back to the
// codegen path when the driver is not there (issue #85).
//
// Returns the command id, or 0 if the queue could not be written.
long long QueueCommand(Options &o, const std::string &verb, const std::string &params_json,
                       std::string &why) {
    try {
        const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
        auto db = dbc::Db::Open(ep);
        const QueryResult r = db.Query(
            "INSERT INTO _erpl_rev_cli_cmd (cmd_id, verb, params) VALUES "
            "(nextval('_erpl_rev_cli_seq'), " + dbc::SqlLiteral(verb) + ", " +
            dbc::SqlLiteral(params_json) + ") RETURNING cmd_id");
        if (r.rows.empty()) { why = "the command queue accepted no row"; return 0; }
        const auto colon = r.rows[0].find(':');
        return colon == std::string::npos ? 0 : std::atoll(r.rows[0].substr(colon + 1).c_str());
    } catch (const std::exception &e) {
        why = e.what();
        return 0;
    }
}

// Read a finished command back out of the queue.
bool CommandResult(Options &o, long long id, std::string &status,
                   std::string &result, std::string &error) {
    try {
        const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
        auto db = dbc::Db::Open(ep);
        const QueryResult r = db.Query(
            "SELECT status, coalesce(result,'') AS result, coalesce(error,'') AS error "
            "FROM _erpl_rev_cli_cmd WHERE cmd_id = " + std::to_string(id));
        if (r.rows.empty()) return false;
        // ParseRows, not a hand-rolled scan. The previous one took the first '"'
        // after the key and the next '"' after that, so a result value
        // containing an escaped quote was cut at the first one. Harmless while
        // every result was a plain sentence; it truncated every operator
        // command whose result is JSON to three characters.
        const auto parsed = json::ParseRows("[" + r.rows[0] + "]");
        if (parsed.empty()) return false;
        for (const auto &c : parsed[0]) {
            if (c.key == "status") status = c.value;
            else if (c.key == "result") result = c.value;
            else if (c.key == "error") error = c.value;
        }
        return !status.empty();
    } catch (...) {
        return false;
    }
}

// Deploy, run, and read back a nonce-tagged result. `out` gets the raw console
// output so a caller can show it when the parse finds nothing.
int RunGenerated(const Options &o, const std::string &kind, const std::string &source,
                 const std::string &nonce, std::string &out) {
    if (o.print_abap) {
        std::fputs(source.c_str(), stdout);
        return 0;
    }
    // Same nonce as the render: the class name is baked into the source.
    abapgen::TempClassrun cls(cli::ToAdtConn(o), kind, nonce, o.keep_generated);
    const std::string err = cls.Deploy(source);
    if (!err.empty()) {
        std::fprintf(stderr, "erpl-rev: %s\n", err.c_str());
        return 1;
    }
    const std::string rerr = cls.Run(out);
    if (!rerr.empty()) {
        std::fprintf(stderr, "erpl-rev: %s\n", rerr.c_str());
        return 1;
    }
    if (abapgen::ResultLines(out, nonce).empty()) {
        // Exit code 0 from erpl-adt proves nothing: a classrun answers HTTP 500
        // with a body on a short dump, and that body can contain anything.
        std::fprintf(stderr,
                     "erpl-rev: the ABAP produced no result line for this run.\n%s\n",
                     out.substr(0, 800).c_str());
        return 1;
    }
    return 0;
}

bool HasFlag(const Options &o, const std::string &name) {
    return std::find(o.args.begin(), o.args.end(), name) != o.args.end();
}

std::string Field(const Options &o, const std::string &name, const std::string &def = "") {
    for (size_t i = 0; i + 1 < o.args.size(); i++)
        if (o.args[i] == name) return o.args[i + 1];
    return def;
}

// Reject a `--flag` this subcommand does not read, rather than ignore it.
// Ignoring is worse than it sounds: a flag that silently does nothing looks
// like it worked. `replicate --queue-only` on a build predating that flag
// printed no complaint and quietly took the generated-ABAP path instead --
// the command appeared to run, against a different code path than asked for.
int RefuseUnknownFlags(const Options &o, const std::string &sub) {
    const std::string bad = UnknownFlag(o.args, sub);
    if (bad.empty()) return 0;
    std::fprintf(stderr, "erpl-rev %s: unknown option '%s'. Try --help.\n",
                 sub.c_str(), bad.c_str());
    return 2;
}

// Is the pre-deployed driver available? Cheap and cached: one ADT search.
bool DriverAvailable(const Options &o) {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    auto r = adt::Run(cli::ToAdtConn(o), {"search", "ZCL_ERPL_REV_CLIDRV"});
    cached = (r.ok() && r.output.find("ZCL_ERPL_REV_CLIDRV") != std::string::npos) ? 1 : 0;
    return cached == 1;
}

// Queue a command, then get it executed: ask the driver to run now if we may,
// otherwise leave it for the periodic heartbeat. Reports which happened,
// because "queued, the job will pick it up within a minute" is a different
// answer from "done" and should not be dressed up as one.
int RunViaDriver(Options &o, const std::string &verb, const std::string &params) {
    std::string why;
    const long long id = QueueCommand(o, verb, params, why);
    if (id == 0) {
        std::fprintf(stderr, "erpl-rev: could not queue the command: %s\n", why.c_str());
        return 1;
    }

    if (o.queue_only) {
        // Deliberately no SAP call: this is the path for a caller with no SAP
        // authorisation at all.
        std::printf("Queued as command %lld. The periodic ERPL_REV_DELTA job will run it.\n"
                    "  erpl-rev sql \"SELECT status, result, error FROM _erpl_rev_cli_cmd "
                    "WHERE cmd_id = %lld\"\n", id, id);
        return 3;   // queued; the outcome is not known yet
    }

    auto r = adt::RunClass(cli::ToAdtConn(o), "ZCL_ERPL_REV_CLIDRV");
    if (!r.ok()) {
        std::printf("Queued as command %lld. Could not run the driver directly\n"
                    "  (%s),\n"
                    "  so the periodic ERPL_REV_DELTA job will pick it up. Watch it with:\n"
                    "    erpl-rev sql \"SELECT * FROM _erpl_rev_cli_cmd WHERE cmd_id = %lld\"\n",
                    id, r.output.substr(0, 120).c_str(), id);
        return 3;   // queued, outcome not yet known
    }

    std::string status, result, error;
    if (!CommandResult(o, id, status, result, error)) {
        std::fprintf(stderr, "erpl-rev: command %lld left no result row.\n", id);
        return 1;
    }
    if (status != "DONE") {
        std::fprintf(stderr, "erpl-rev: %s\n", error.empty() ? status.c_str() : error.c_str());
        return 1;
    }
    std::printf("%s\n", result.c_str());
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// sync ls / show — read-only, local
// ---------------------------------------------------------------------------

static int SyncList(Options &o) {
    const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
    if (!o.quiet) std::fprintf(stderr, "erpl-rev: %s\n", ep.why.c_str());
    auto db = dbc::Db::Open(ep);
    // `due` mirrors zcl_erpl_rev_delta=>due: micro:<s> -> s seconds, hourly ->
    // 3600, nightly -> 86400, manual -> never.
    const QueryResult r = db.Query(
        "SELECT target, method, source_from, cadence, status, "
        "       last_run_ts, rows_applied, "
        "       CASE WHEN cadence = 'manual' THEN false "
        "            WHEN last_run_ts IS NULL THEN true "
        "            ELSE epoch(now() - last_run_ts) >= "
        "                 CASE WHEN cadence LIKE 'micro:%' "
        "                        THEN TRY_CAST(substr(cadence, 7) AS BIGINT) "
        "                      WHEN cadence = 'hourly'  THEN 3600 "
        "                      WHEN cadence = 'nightly' THEN 86400 "
        "                      ELSE NULL END END AS due, "
        "       last_error "
        "FROM _erpl_rev_delta_state ORDER BY target",
        o.limit < 0 ? 0 : o.limit);
    std::fputs(render::Render(r, o.format).c_str(), stdout);
    return 0;
}

static int SyncShow(Options &o, const std::string &target) {
    const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
    if (!o.quiet) std::fprintf(stderr, "erpl-rev: %s\n", ep.why.c_str());
    auto db = dbc::Db::Open(ep);
    const QueryResult s = db.Query(
        "SELECT * FROM _erpl_rev_delta_state WHERE target = " + dbc::SqlLiteral(target));
    if (s.rows.empty()) {
        std::fprintf(stderr, "erpl-rev: no sync job named '%s'.\n", target.c_str());
        return 1;
    }
    std::fputs(render::Render(s, o.format).c_str(), stdout);

    const QueryResult h = db.Query(
        "SELECT started_at, run_type, method, status, rows_applied, duration_ms, error_text "
        "FROM erpl_rev_run_stats WHERE target = " + dbc::SqlLiteral(target) +
        " ORDER BY started_at DESC LIMIT 10");
    if (!h.rows.empty()) {
        std::printf("\nrecent runs\n");
        std::fputs(render::Render(h, o.format).c_str(), stdout);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sync create / run / schedule — SAP
// ---------------------------------------------------------------------------

static int SyncCreate(Options &o, const std::string &target) {
    abapgen::SyncState st;
    st.target      = target;
    st.method      = Field(o, "--method");
    st.source_from = Field(o, "--source");
    st.keys        = Field(o, "--keys");
    st.chg_col     = Field(o, "--chg-col");
    st.wm_kind     = Field(o, "--wm-kind");
    st.wm_value    = Field(o, "--wm-value");
    st.cadence     = Field(o, "--cadence", "nightly");
    st.extra       = Field(o, "--extra");
    st.time_col    = Field(o, "--time-col");
    const std::string safety = Field(o, "--safety-secs");
    if (!safety.empty()) st.safety_secs = std::atoll(safety.c_str());
    const std::string units = Field(o, "--safety-units");
    if (!units.empty()) st.safety_units = std::atoll(units.c_str());
    // Tri-state: --log, --no-log, or neither. "Neither" has to survive all the
    // way to the server as NULL, or every registration asserts the flag and a
    // re-registration for an unrelated reason turns a target's change log off.
    auto tri = [&](const char *on, const char *off) -> std::string {
        if (HasFlag(o, on)) return "true";
        if (HasFlag(o, off)) return "false";
        return "";
    };
    st.log_enabled        = tri("--log", "--no-log");
    st.allow_empty_reload = tri("--allow-empty-reload", "--no-allow-empty-reload");
    st.load_type_default  = Field(o, "--load-type-default");
    if (!st.load_type_default.empty() && st.load_type_default != "D" &&
        st.load_type_default != "F" && st.load_type_default != "I" &&
        st.load_type_default != "L") {
        std::fprintf(stderr,
                     "erpl-rev sync create: --load-type-default must be D, F, I or L.\n");
        return 2;
    }

    if (st.method.empty() || st.source_from.empty() || st.keys.empty()) {
        std::fprintf(stderr,
                     "erpl-rev sync create: --method, --source and --keys are required.\n");
        return 2;
    }

    if (!o.print_abap && !o.dry_run && (o.queue_only || DriverAvailable(o))) {
        if (const int rc = ConsentGate(o, "Register sync job '" + st.target + "' on " + o.host))
            return rc;
        // Rendered from the SAME list as the generated-ABAP path, so the two
        // writers of this surface cannot drift again. Backticks are the ABAP
        // literal form; the queue carries plain values.
        // The RAW form, from the same list the generated-ABAP path renders. This
        // used to take the rendered ABAP and strip the backticks back off to
        // recover the value it had started with -- which works only while every
        // rendering is trivially reversible, and a tri-state flag is not.
        std::vector<std::pair<std::string, std::string>> kv;
        for (const auto &f : abapgen::RegisterFields(st)) kv.push_back({f.name, f.raw});
        const std::string j = BuildParams(kv);
        return RunViaDriver(o, "sync_register", j);
    }
    const std::string nonce = abapgen::MakeNonce();
    const std::string src = abapgen::RenderSyncRegister(st, nonce);
    if (o.print_abap) { std::fputs(src.c_str(), stdout); return 0; }
    if (o.dry_run) {
        std::printf("Would register sync job '%s' (%s over %s).\n",
                    st.target.c_str(), st.method.c_str(), st.source_from.c_str());
        return 0;
    }
    if (const int rc = ConsentGate(o, "Register sync job '" + st.target + "' on " + o.host))
        return rc;

    std::string out;
    if (const int rc = RunGenerated(o, "C", src, nonce, out)) return rc;

    const std::string status = abapgen::ResultField(out, nonce, "status");
    const std::string err = abapgen::ResultField(out, nonce, "error");
    if (status != "ok") {
        std::fprintf(stderr, "erpl-rev sync create: rejected by SAP: %s\n", err.c_str());
        return 1;
    }
    std::printf("Registered '%s'.\n", st.target.c_str());
    return 0;
}

static int SyncRun(Options &o, const std::string &target) {
    // The flag whitelist above passes any value through, so this is the only
    // thing between `--load-type Z` and a run that quietly does the wrong thing.
    // Checked before the consent gate: a typo should cost an error, not a
    // password prompt and a wrong job.
    std::string load_type = Field(o, "--load-type");
    if (!load_type.empty()) {
        if (!IsValidLoadTypeCode(load_type)) {
            std::fprintf(stderr, "erpl-rev sync run: %s\n",
                         [&] { try { ParseLoadType(load_type); } catch (const std::exception &e) {
                                   return std::string(e.what()); } return std::string(); }().c_str());
            return 2;
        }
        load_type = LoadTypeCode(ParseLoadType(load_type));
    }
    if (!o.print_abap && !o.dry_run && (o.queue_only || DriverAvailable(o))) {
        if (const int rc = ConsentGate(o, target.empty()
                                              ? "Run every due sync job on " + o.host
                                              : "Run sync job '" + target + "' on " + o.host))
            return rc;
        return RunViaDriver(o, "sync_run",
                            BuildParams({{"target", target}, {"load_type", load_type}}));
    }
    const std::string nonce = abapgen::MakeNonce();
    const std::string src = abapgen::RenderSyncRun(target, nonce);
    if (o.print_abap) { std::fputs(src.c_str(), stdout); return 0; }
    if (o.dry_run) {
        std::printf("Would run %s.\n",
                    target.empty() ? "every due sync job" : ("sync job '" + target + "'").c_str());
        return 0;
    }
    if (const int rc = ConsentGate(o, target.empty()
                                          ? "Run every due sync job on " + o.host
                                          : "Run sync job '" + target + "' on " + o.host))
        return rc;

    std::string out;
    if (const int rc = RunGenerated(o, "S", src, nonce, out)) return rc;

    int failures = 0, ran = 0;
    for (const auto &[k, v] : abapgen::ResultLines(out, nonce)) {
        if (k != "run") continue;
        ran++;
        std::printf("  %s\n", v.c_str());
        if (v.find(";error=") != std::string::npos &&
            v.substr(v.rfind(";error=") + 7).size() > 0)
            failures++;
    }
    if (ran == 0) std::printf("Nothing was due.\n");
    return failures ? 1 : 0;
}

static int SyncSchedule(Options &o) {
    const bool remove = HasFlag(o, "--remove");
    const std::string every = Field(o, "--every");
    if (!remove && every.empty()) {
        std::fprintf(stderr, "erpl-rev sync schedule: --every <minutes> or --remove.\n");
        return 2;
    }
    const long long minutes = every.empty() ? 1 : std::atoll(every.c_str());

    if (!o.print_abap && !o.dry_run && (o.queue_only || DriverAvailable(o))) {
        if (const int rc = ConsentGate(o, std::string(remove ? "Remove" : "Install") +
                                              " the periodic job on " + o.host))
            return rc;
        const std::string j = BuildParams({{"minutes", std::to_string(minutes)},
                                           {"remove", remove ? "true" : "false"}});
        return RunViaDriver(o, "schedule", j);
    }
    const std::string nonce = abapgen::MakeNonce();
    const std::string src = abapgen::RenderSchedule(minutes, remove, nonce);
    if (o.print_abap) { std::fputs(src.c_str(), stdout); return 0; }
    if (o.dry_run) {
        std::printf("Would %s the periodic job ERPL_REV_DELTA%s.\n",
                    remove ? "remove" : "install",
                    remove ? "" : (" every " + std::to_string(minutes) + " min").c_str());
        return 0;
    }
    if (const int rc = ConsentGate(o, std::string(remove ? "Remove" : "Install") +
                                          " the periodic job on " + o.host))
        return rc;

    std::string out;
    if (const int rc = RunGenerated(o, "J", src, nonce, out)) return rc;

    const std::string msg = abapgen::ResultField(out, nonce, "msg");
    if (msg.rfind("ERROR:", 0) == 0) {
        std::fprintf(stderr, "erpl-rev sync schedule: %s\n", msg.c_str());
        return 1;
    }

    // "Submitted" is not "scheduled": the work happens in a background job that
    // can abort after this command has already returned. Ask SAP what actually
    // exists before claiming anything.
    std::printf("%s — verifying…\n", msg.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(12));

    const std::string qn = abapgen::MakeNonce();
    std::string qout;
    if (const int rc = RunGenerated(o, "Q", abapgen::RenderJobCheck(qn), qn, qout))
        return rc;
    const long long jobs = std::atoll(abapgen::ResultField(qout, qn, "jobs").c_str());

    if (remove && jobs == 0) { std::printf("Periodic job removed.\n"); return 0; }
    if (!remove && jobs >= 1) {
        std::printf("Periodic job ERPL_REV_DELTA is scheduled (every %lld min).\n", minutes);
        return 0;
    }
    std::fprintf(stderr,
        "erpl-rev sync schedule: the job was submitted but SAP still reports %lld\n"
        "  periodic ERPL_REV_DELTA job(s) — the scheduling step did not take effect.\n"
        "  Check SM37 for an aborted ERPLCLI_* job, or do it from the GUI:\n"
        "  SE38 -> Z_ERPL_REV_DELTA -> %s.\n",
        jobs, remove ? "p_unsch" : "p_sched + p_min");
    return 1;
}

int SyncSetWm(Options &o, const std::string &target);
int SyncPreview(Options &o, const std::string &target);
int SyncValidate(Options &o, const std::string &target);
int SyncUnpark(Options &o, const std::string &target);

int RunSync(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args.front();
    const std::string arg = o.args.size() > 1 && o.args[1].rfind("--", 0) != 0
                                ? o.args[1] : std::string();
    // Before anything that prompts or contacts SAP: a typo'd flag should cost
    // the user an error message, not a password prompt and a wrong job.
    if (const int rc = RefuseUnknownFlags(o, "sync " + sub)) return rc;

    const auto cfg = cli::ReadConfig();
    // --queue-only never contacts SAP, so it must not prompt for a password to
    // write a row into a local database.
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    try {
        if (sub == "ls")            return SyncList(o);
        if (sub == "show")          return SyncShow(o, arg);
        if (sub == "create")        return SyncCreate(o, arg);
        if (sub == "run")           return SyncRun(o, arg);
        if (sub == "run-due")       return SyncRun(o, "");
        if (sub == "schedule")      return SyncSchedule(o);
        if (sub == "set-wm")        return SyncSetWm(o, arg);
        if (sub == "preview")       return SyncPreview(o, arg);
        if (sub == "validate")      return SyncValidate(o, arg);
        if (sub == "unpark")        return SyncUnpark(o, arg);
    } catch (const abapgen::UnsafeValue &e) {
        std::fprintf(stderr, "erpl-rev: %s\n", e.what());
        return 2;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "erpl-rev: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr,
                 "erpl-rev sync: expected ls, show, create, run, run-due, schedule, "
                 "set-wm, preview, validate or unpark.\n");
    return 2;
}

int SyncSetWm(Options &o, const std::string &target) {
    const std::string wm = Field(o, "--wm-value");
    if (target.empty() || wm.empty()) {
        std::fprintf(stderr, "erpl-rev sync set-wm <target> --wm-value V\n"
                             "  Moves the target's position. The next cycle re-reads from "
                             "there.\n");
        return 2;
    }
    if (const int rc = ConsentGate(o, "Set the watermark of '" + target + "' to " + wm)) return rc;
    return RunViaDriver(o, "set_wm", BuildParams({{"target", target}, {"wm_value", wm}}));
}

int SyncPreview(Options &o, const std::string &target) {
    if (target.empty()) {
        std::fprintf(stderr, "erpl-rev sync preview <target> [--rows N]\n");
        return 2;
    }
    return RunViaDriver(o, "preview",
                        BuildParams({{"target", target}, {"rows", Field(o, "--rows", "20")}}));
}

int SyncValidate(Options &o, const std::string &target) {
    if (target.empty()) {
        std::fprintf(stderr, "erpl-rev sync validate <target> [--full] [--sample-rows N]\n"
                             "  Compares the replica against SAP cell by cell. A replica that "
                             "is the\n  right size and the wrong content passes every count "
                             "check there is.\n");
        return 2;
    }
    return RunViaDriver(o, "validate", BuildParams({
        {"target", target},
        {"mode", HasFlag(o, "--full") ? "full" : "sample"},
        {"sample_rows", Field(o, "--sample-rows", "1000")}}));
}

int SyncUnpark(Options &o, const std::string &target) {
    if (target.empty()) {
        std::fprintf(stderr, "erpl-rev sync unpark <target>\n"
                             "  Releases a parked target and clears its backoff.\n");
        return 2;
    }
    return RunViaDriver(o, "unpark", BuildParams({{"target", target}}));
}

// ---------------------------------------------------------------------------
// daemon | sub | retain -- the operator verbs
//
// The engine behind these has been unit-tested since it was written and could
// not be invoked: no verb reached publish::Advance, publish::Retain or the
// daemon's control row, so subscriptions, retention and "is the daemon alive"
// were reachable only by hand-written SQL. The flag tables for them existed and
// were consulted by nothing, and the tests asserting on those tables passed
// while proving nothing about a reachable path.
// ---------------------------------------------------------------------------

int RunDaemon(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args[0];
    if (const int rc = RefuseUnknownFlags(o, "daemon " + sub)) return rc;
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    if (sub == "start") {
        const std::string tick = Field(o, "--tick"), workers = Field(o, "--workers");
        return RunViaDriver(o, "daemon_start",
                            BuildParams({{"tick_secs", tick}, {"max_workers", workers}}));
    }
    if (sub == "stop")   return RunViaDriver(o, "daemon_stop", BuildParams({}));
    if (sub == "status") return RunViaDriver(o, "daemon_status", BuildParams({}));

    std::fprintf(stderr, "erpl-rev daemon: expected start, stop or status.\n");
    return 2;
}

int RunSub(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args[0];
    const std::string name = o.args.size() > 1 ? o.args[1] : "";
    if (const int rc = RefuseUnknownFlags(o, "sub " + sub)) return rc;
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    if (sub == "create") {
        const std::string target = Field(o, "--target"), sink = Field(o, "--sink");
        if (name.empty() || target.empty() || sink.empty()) {
            std::fprintf(stderr, "erpl-rev sub create <name> --target T --sink SPEC\n"
                                 "  SPEC is PARQUET:/path:FULL or TABLE:name:APPEND.\n");
            return 2;
        }
        return RunViaDriver(o, "subs", BuildParams({{"op", "create"}, {"name", name},
                                                    {"target", target}, {"sink", sink}}));
    }
    if (sub == "advance") {
        if (name.empty()) { std::fprintf(stderr, "erpl-rev sub advance <name>\n"); return 2; }
        return RunViaDriver(o, "subs", BuildParams({{"op", "advance"}, {"name", name}}));
    }
    if (sub == "ls") return RunViaDriver(o, "subs", BuildParams({{"op", "ls"}}));

    std::fprintf(stderr, "erpl-rev sub: expected create, advance or ls.\n");
    return 2;
}

int RunMass(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args[0];
    if (const int rc = RefuseUnknownFlags(o, "mass " + sub)) return rc;
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    if (sub != "run") {
        std::fprintf(stderr, "erpl-rev mass run --target T --source S --part-col C "
                             "[--split STRATEGY]\n"
                             "  STRATEGY is records (default), size, time, fiscal, list or key.\n");
        return 2;
    }
    const std::string target = Field(o, "--target"), source = Field(o, "--source");
    if (target.empty() || source.empty()) {
        std::fprintf(stderr, "erpl-rev mass run: --target and --source are required.\n");
        return 2;
    }
    if (const int rc = ConsentGate(o, "Mass load '" + source + "' into '" + target + "'"))
        return rc;
    return RunViaDriver(o, "mass_run", BuildParams({
        {"target", target}, {"source", source},
        {"strategy", Field(o, "--split", "records")},
        {"part_col", Field(o, "--part-col")},
        {"limit_rows", Field(o, "--limit-rows", "100000")},
        {"limit_mb", Field(o, "--limit-mb")},
        {"time_unit", Field(o, "--time-unit")}}));
}

int RunCdc(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args[0];
    if (const int rc = RefuseUnknownFlags(o, "cdc " + sub)) return rc;
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    const std::string target = Field(o, "--target");
    if ((sub == "status" || sub == "repair") && target.empty()) {
        std::fprintf(stderr, "erpl-rev cdc %s --target T\n", sub.c_str());
        return 2;
    }
    if (sub == "status")
        return RunViaDriver(o, "cdc_status", BuildParams({{"target", target}}));
    if (sub == "repair") {
        if (const int rc = ConsentGate(o, "Recreate the missing trigger objects of '" +
                                              target + "'"))
            return rc;
        return RunViaDriver(o, "cdc_repair", BuildParams({{"target", target}}));
    }
    std::fprintf(stderr, "erpl-rev cdc: expected status or repair.\n");
    return 2;
}

int RunRetain(Options o) {
    if (const int rc = RefuseUnknownFlags(o, "retain")) return rc;
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    const std::string target = Field(o, "--target");
    if (target.empty()) {
        std::fprintf(stderr, "erpl-rev retain --target T [--window-days N]\n");
        return 2;
    }
    // Days on the command line, seconds on the wire: an operator thinks in days
    // and the engine's window is a duration.
    const std::string days = Field(o, "--window-days", "0");
    const long long secs = std::atoll(days.c_str()) * 86400;
    return RunViaDriver(o, "retain",
                        BuildParams({{"target", target}, {"window_secs", std::to_string(secs)}}));
}

// ---------------------------------------------------------------------------
// replicate
// ---------------------------------------------------------------------------

int RunReplicate(Options o) {
    // Before anything that prompts or contacts SAP: a typo'd flag should cost
    // the user an error message, not a password prompt and a wrong load.
    if (const int rc = RefuseUnknownFlags(o, "replicate")) return rc;

    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, !o.queue_only && !o.non_interactive && cli::IsTty());

    abapgen::ReplicateParams p;
    p.table       = Field(o, "--table");
    p.target      = Field(o, "--target");
    p.columns     = Field(o, "--columns");
    p.where       = Field(o, "--where");
    p.cds_params  = Field(o, "--cds-params");
    p.init        = Field(o, "--init");
    p.mode        = Field(o, "--mode", "UPSERT");
    p.part_col    = Field(o, "--part-col");
    p.dest        = Field(o, "--dest");
    p.partition_by = Field(o, "--partition-by");
    p.target_kind = Field(o, "--target-kind", "duckdb");
    const std::string batch = Field(o, "--batch"), maxrows = Field(o, "--maxrows"),
                      jobs = Field(o, "--jobs");
    if (!batch.empty())   p.batch = std::atoll(batch.c_str());
    if (!maxrows.empty()) p.maxrows = std::atoll(maxrows.c_str());
    if (!jobs.empty())    { p.jobs = std::atoll(jobs.c_str()); p.parallel = p.jobs > 1; }
    for (const auto &a : o.args) {
        if (a == "--parallel")  p.parallel = true;
        if (a == "--no-verify") p.verify = false;
        if (a == "--no-truncate") p.truncate = false;
    }
    if (p.table.empty()) {
        std::fprintf(stderr, "erpl-rev replicate: --table is required.\n");
        return 2;
    }
    if (p.target.empty()) {
        p.target = p.table;
        for (char &c : p.target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    try {
        if (!o.print_abap && !o.dry_run && (o.queue_only || DriverAvailable(o))) {
            if (const int rc = ConsentGate(o, "Load " + p.table + " into " + p.target +
                                                  " from " + o.host))
                return rc;
            const std::string j = BuildParams({
                {"table", p.table}, {"target", p.target}, {"columns", p.columns},
                {"where", p.where}, {"cds_params", p.cds_params}, {"init", p.init},
                {"mode", p.mode}, {"batch", std::to_string(p.batch)},
                {"maxrows", std::to_string(p.maxrows)},
                {"truncate", p.truncate ? "true" : "false"}});
            // The driver runs replicate synchronously inside the classrun, which
            // is fine for the common case; --detach and the background-job path
            // remain available through the codegen route for very long loads.
            return RunViaDriver(o, "replicate", j);
        }
        const std::string nonce = abapgen::MakeNonce();
        const std::string src = abapgen::RenderReplicate(p, nonce);
        if (o.print_abap) { std::fputs(src.c_str(), stdout); return 0; }
        if (o.dry_run) {
            std::printf("Would load %s -> %s%s.\n", p.table.c_str(), p.target.c_str(),
                        p.parallel ? " (parallel)" : "");
            std::fputs(src.c_str(), stdout);
            return 0;
        }
        if (const int rc = ConsentGate(o, "Load " + p.table + " into " + p.target +
                                              " from " + o.host))
            return rc;

        std::string out;
        if (const int rc = RunGenerated(o, "R", src, nonce, out)) return rc;

        if (abapgen::ResultField(out, nonce, "status") != "submitted") {
            std::fprintf(stderr, "erpl-rev replicate: %s\n",
                         abapgen::ResultField(out, nonce, "error").c_str());
            return 1;
        }
        const std::string job = abapgen::ResultField(out, nonce, "job");
        std::printf("Submitted as SAP background job %s.\n", job.c_str());

        if (HasFlag(o, "--detach")) {
            std::printf("Watch it with: erpl-rev sql \"SELECT * FROM erpl_rev_run_stats "
                        "WHERE target = '%s' ORDER BY started_at DESC LIMIT 5\"\n",
                        p.target.c_str());
            return 0;
        }

        // Poll DuckDB rather than holding an HTTP connection: the load can run
        // for hours, and a dropped connection must not look like a failed load.
        const std::string wait_s = Field(o, "--wait");
        const long long wait = wait_s.empty() ? 3600 : std::atoll(wait_s.c_str());
        const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
        auto db = dbc::Db::Open(ep);
        const auto t0 = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - t0).count();
            long long rows = 0;
            try {
                const QueryResult c = db.Query(
                    "SELECT count(*) AS n FROM " + dbc::SqlIdentifier(p.target));
                if (!c.rows.empty()) {
                    const auto pos = c.rows[0].find(':');
                    if (pos != std::string::npos)
                        rows = std::atoll(c.rows[0].substr(pos + 1).c_str());
                }
            } catch (...) {
                // The target is created inside the run; "does not exist" is 0 rows.
            }
            const QueryResult st = db.Query(
                "SELECT status, rows_applied, duration_ms, error_text "
                "FROM erpl_rev_run_stats WHERE target = " + dbc::SqlLiteral(p.target) +
                " ORDER BY started_at DESC LIMIT 1");
            if (!st.rows.empty() && st.rows[0].find("\"status\"") != std::string::npos &&
                elapsed > 2) {
                std::fputs(render::Render(st, o.format).c_str(), stdout);
                return st.rows[0].find("SUCCESS") != std::string::npos ? 0 : 1;
            }
            if (!o.quiet && cli::IsTty())
                std::fprintf(stderr, "\r  %s: %lld rows (%llds) ", p.target.c_str(),
                             rows, static_cast<long long>(elapsed));
            if (elapsed >= wait) {
                std::fprintf(stderr,
                    "\nerpl-rev: job %s has not reported completion after %llds.\n"
                    "  It may still be running -- SAP keeps working after this command\n"
                    "  gives up. Watch it with:\n"
                    "    erpl-rev sql \"SELECT * FROM erpl_rev_run_stats ORDER BY "
                    "started_at DESC LIMIT 5\"\n",
                    job.c_str(), wait);
                return 3;   // unknown, deliberately not 1
            }
        }
    } catch (const abapgen::UnsafeValue &e) {
        std::fprintf(stderr, "erpl-rev: %s\n", e.what());
        return 2;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "erpl-rev: %s\n", e.what());
        return 1;
    }
}

} // namespace erpl_rev::cmd
