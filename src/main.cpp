// erpl-rev: a registered RFC server that bridges ABAP RFC calls into DuckDB.
//
// Registers PROGRAM_ID at the SAP gateway and hosts the bridge FMs
// (STFC_CONNECTION ping + Z_DUCKDB_QUERY/INGEST/OPEN/FETCH/CLOSE); ABAP calls them
// via a type-T destination to replicate/query data through DuckDB. Optionally
// starts the DuckDB quack network server (--quack). See README + docs/.
//
// Connection parameters come from the environment (defaults target local A4H):
//   ERPL_REV_PROGRAM_ID  (default ERPL_REV)
//   ERPL_REV_GWHOST      (default localhost)
//   ERPL_REV_GWSERV      (default 3300)   -- sapgw00 on A4H
#include "rfc_handlers.hpp"
#include "duckdb_bridge.hpp"
#include "logging.hpp"
#include "sap_uc.hpp"
#include "erpl_rev_telemetry.hpp"

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sapnwrfc.h"
#include "datazoo_banner.hpp"

// Product version baked in at build time (CI passes -DERPL_REV_VERSION=X.Y.Z).
#ifndef ERPL_REV_VERSION
#define ERPL_REV_VERSION "dev"
#endif

using namespace erpl_rev;

namespace {

volatile sig_atomic_t g_running = 1;
void OnSigInt(int) { g_running = 0; }

std::string Env(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Truthy env values: 1/true/yes/on (case-insensitive); everything else false.
bool EnvTruthy(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s(v);
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

// Replace every occurrence of `secret` in `s` with a redaction marker. Used to
// keep an operator-supplied quack token out of the logs (a generated one is left
// visible, since the log is the only place to discover it).
std::string Redact(std::string s, const std::string &secret) {
    if (secret.empty()) return s;
    const std::string mark = "<redacted>";
    for (size_t pos = 0; (pos = s.find(secret, pos)) != std::string::npos; pos += mark.size())
        s.replace(pos, secret.size(), mark);
    return s;
}

// quack only needs allow_other_hostname=true when binding beyond loopback.
bool ListenIsLoopback(const std::string &listen) {
    return listen.find("localhost") != std::string::npos ||
           listen.find("127.0.0.1") != std::string::npos ||
           listen.find("[::1]")     != std::string::npos;
}

// Parsed command-line options. *_set marks "seen on the command line" so the
// resolver can apply CLI-over-env precedence without conflating an explicit
// empty value with "unset".
struct Cli {
    bool quack = false;
    std::string quack_listen;
    bool quack_listen_set = false;
    std::string quack_token;
    bool quack_token_set = false;
    std::string db_path;
    bool db_set = false;
    std::string init_sql;
    bool init_sql_set = false;
    std::string init_file;
    bool init_file_set = false;
    bool help = false;
    bool smoke = false;
    bool no_telemetry = false;
};

// Identity for the feedback banner, the startup log line and the issue hint.
static constexpr datazoo::BannerInfo kBanner {"erpl-rev", ERPL_REV_VERSION,
                                              "https://github.com/DataZooDE/erpl-rev"};

void PrintHelp() {
    std::fputs(
        "erpl-rev — ABAP RFC -> DuckDB bridge server\n\n"
        "Usage: erpl_rev_server [options]\n\n"
        "Options:\n"
        "  --quack[=<listen>]       Also start the DuckDB quack network server\n"
        "                           (default quack:localhost, port 9494).\n"
        "  --quack-listen <listen>  Set the quack bind URI (implies --quack); use\n"
        "                           quack:0.0.0.0:9494 to expose beyond loopback.\n"
        "  --quack-token <token>    Pin the quack client auth token (implies --quack);\n"
        "                           default: quack generates a random one.\n"
        "  --db <path>              DuckDB file (default erpl-rev.duckdb);\n"
        "                           pass :memory: for a throwaway in-memory DB.\n"
        "  --init-sql <sql>         DuckDB SQL run once at boot on a GLOBAL connection\n"
        "                           (INSTALL/LOAD, CREATE SECRET, ATTACH external\n"
        "                           catalogs) so replication can publish to parquet/\n"
        "                           postgres/ducklake/bigquery/iceberg targets.\n"
        "  --init-file <path>       Read boot SQL from a file (for multi-line ATTACH/\n"
        "                           secret scripts). Overrides --init-sql.\n"
        "  --smoke                  Self-check: load + call the bundled SAP NW RFC\n"
        "                           SDK and DuckDB, print their versions, exit 0.\n"
        "                           Needs no SAP gateway (used by the CI smoke test).\n"
        "  --no-telemetry           Disable anonymous usage telemetry for this run.\n"
        "  -h, --help               Show this help and exit.\n\n"
        "Config is read from the environment; the flags above override it:\n"
        "  ERPL_REV_PROGRAM_ID    gateway PROGRAM_ID          (default ERPL_REV)\n"
        "  ERPL_REV_GWHOST        SAP gateway host            (default localhost)\n"
        "  ERPL_REV_GWSERV        SAP gateway service         (default 3300)\n"
        "  ERPL_REV_REG_COUNT     parallel registrations      (default 5)\n"
        "  ERPL_REV_QUACK         enable quack (truthy)       (default off)\n"
        "  ERPL_REV_QUACK_LISTEN  quack bind URI              (default quack:localhost)\n"
        "  ERPL_REV_QUACK_TOKEN   pin quack auth token        (default random)\n"
        "  ERPL_REV_DB_PATH       DuckDB file (:memory: for in-mem) (default erpl-rev.duckdb)\n"
        "  ERPL_REV_LOG_LEVEL     error|warn|info|debug|trace (default info)\n"
        "  ERPL_REV_LOG_FORMAT    console|json                (default console)\n"
        "  ERPL_REV_LOG_COLOR     auto|always|never           (default auto)\n"
        "  ERPL_REV_NO_TELEMETRY  disable usage telemetry (truthy)\n"
        "  DATAZOO_DISABLE_TELEMETRY  disable telemetry across all DataZoo tools\n"
        "  ERPL_REV_TELEMETRY_SAMPLE_RATE  sample rfc_call events, 0<r<=1 (default 1)\n"
        "  DATAZOO_NO_BANNER      suppress the startup feedback banner (truthy)\n\n"
        "Bugs, feedback and stars: https://github.com/DataZooDE/erpl-rev\n",
        stderr);
}

Cli ParseArgs(int argc, char **argv) {
    Cli c;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        std::string key = a, inval;
        bool has_inval = false;
        auto eq = a.find('=');
        if (eq != std::string::npos) {
            key = a.substr(0, eq);
            inval = a.substr(eq + 1);
            has_inval = true;
        }
        // Value for flags that take one: prefer "--flag=value", else next token.
        auto take_value = [&]() -> std::string {
            if (has_inval) return inval;
            if (i + 1 < argc) return argv[++i];
            return "";
        };

        if (key == "--quack") {
            c.quack = true;
            if (has_inval) { c.quack_listen = inval; c.quack_listen_set = true; }
        } else if (key == "--quack-listen") {
            c.quack = true;
            c.quack_listen = take_value();
            c.quack_listen_set = true;
        } else if (key == "--quack-token") {
            c.quack = true;
            c.quack_token = take_value();
            c.quack_token_set = true;
        } else if (key == "--db" || key == "--db-path") {
            c.db_path = take_value();
            c.db_set = true;
        } else if (key == "--init-sql") {
            c.init_sql = take_value();
            c.init_sql_set = true;
        } else if (key == "--init-file") {
            c.init_file = take_value();
            c.init_file_set = true;
        } else if (key == "-h" || key == "--help") {
            c.help = true;
        } else if (key == "--smoke") {
            c.smoke = true;
        } else if (key == "--no-telemetry") {
            c.no_telemetry = true;
        } else {
            log::get().Warn("server", "ignoring unknown argument", {{"arg", a}});
        }
    }
    return c;
}

// Self-check behind --smoke: prove the bundled SAP NW RFC SDK and DuckDB
// libraries actually load and are CALLABLE on this machine, with no SAP gateway.
// Both libraries are linked (DT_NEEDED / DYLIB / DLL), so a missing or
// arch-incompatible bundle already fails the process before main(); calling into
// each one here additionally proves the symbols resolve. Prints a single grep-able
// "smoke ok" line and exits 0 on success, 1 on failure. This is what the platform
// smoke-test CI runs against the shipped single-file bundle on a clean runner.
int RunSmoke() {
    unsigned int maj = 0, min = 0, patch = 0;
    const SAP_UC *ver = RfcGetVersion(&maj, &min, &patch);
    std::string sapver = ver ? uc2std(ver) : std::string();
    if (sapver.empty()) {
        std::fputs("erpl-rev smoke FAILED: RfcGetVersion returned no version\n", stderr);
        return 1;
    }

    std::string duckver;
    try {
        DuckDbBridge db("");   // in-memory; also exercises the boot DDL
        QueryResult r = db.Query("SELECT version() AS v");
        if (!r.rows.empty()) duckver = r.rows.front();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "erpl-rev smoke FAILED: DuckDB self-check threw: %s\n", e.what());
        return 1;
    }

    std::fprintf(stdout,
                 "erpl-rev smoke ok: SAP NW RFC SDK %s (%u.%u.%u); DuckDB %s\n",
                 sapver.c_str(), maj, min, patch, duckver.c_str());
    std::fflush(stdout);
    return 0;
}

void SAP_API OnStateChange(RFC_SERVER_HANDLE, RFC_STATE_CHANGE *c) {
    if (c->oldState == c->newState) return;   // ignore reconnect heartbeats
    log::get().Debug("server", "state change",
                     {{"old", (long long)c->oldState}, {"new", (long long)c->newState}});
}

} // namespace

// A plain `main`, not the SDK's `mainU`.
//
// `mainU` is a macro in sapucrfc.h that generates a real `main` calling
// `nlsui_initialize()` and `nlsui_alloc_wcsar()` to hand the program a UTF-16
// argv. Both live in **libsapucum**, so using it would keep a SAP shared object
// on the link line after libsapnwrfc is gone. Nothing here wants UTF-16 argv
// anyway — every flag was converted straight back to UTF-8 — so taking
// `char **` removes a dependency and a round trip at once.
int main(int argc, char **argv) {
    log::get().Configure();

    Cli cli = ParseArgs(argc, argv);
    if (cli.help) { PrintHelp(); return 0; }
    if (cli.smoke) return RunSmoke();

    // Two surfaces, because this process almost never runs on a terminal. The
    // banner is for the operator who starts it by hand; the log line is for the
    // far more common case -- systemd, a container, an SAP gateway service --
    // where stderr is a file nobody is watching live but everybody reads later.
    datazoo::ShowBannerStandalone(kBanner);
    log::get().Info("startup", datazoo::FeedbackLine(kBanner));

    std::signal(SIGINT, OnSigInt);
    std::signal(SIGTERM, OnSigInt);

    const std::string program_id = Env("ERPL_REV_PROGRAM_ID", "ERPL_REV");
    const std::string gwhost     = Env("ERPL_REV_GWHOST", "localhost");
    const std::string gwserv     = Env("ERPL_REV_GWSERV", "3300");

    // Config precedence: CLI flag > environment > default. Env stays the
    // 12factor baseline; flags are an override convenience.
    const bool quack_enabled = cli.quack || EnvTruthy("ERPL_REV_QUACK");
    const std::string quack_listen =
        cli.quack_listen_set ? cli.quack_listen
                             : Env("ERPL_REV_QUACK_LISTEN", "quack:localhost");
    const std::string quack_token =
        cli.quack_token_set ? cli.quack_token : Env("ERPL_REV_QUACK_TOKEN", "");
    // File-backed by default so replicated data survives restarts. Override with
    // --db / ERPL_REV_DB_PATH; pass ":memory:" (e.g. `make run-mem`) for a
    // throwaway in-memory DB.
    std::string db_path =
        cli.db_set ? cli.db_path : Env("ERPL_REV_DB_PATH", "erpl-rev.duckdb");
    if (db_path == ":memory:") db_path.clear();   // empty path => in-memory engine

    // Boot init SQL precedence: --init-file > --init-sql > env ERPL_REV_DUCKDB_INIT
    // (empty here lets the bridge ctor apply the env fallback itself).
    std::string init_sql;
    if (cli.init_file_set) {
        std::ifstream f(cli.init_file, std::ios::binary);
        if (!f) {
            log::get().Error("server", "cannot read --init-file", {{"path", cli.init_file}});
            return 1;
        }
        std::ostringstream ss; ss << f.rdbuf();
        init_sql = ss.str();
    } else if (cli.init_sql_set) {
        init_sql = cli.init_sql;
    }

    // Anonymous usage telemetry (DataZooDE/posthog-telemetry, shared schema v2):
    // this is a long-running server, so install_kind="server" and one
    // $session_id per uptime. Emits server_started at boot, one rfc_call per
    // bridge FM invocation, and $exception on failure — NEVER SAP data, SQL
    // text, table/field names, or error messages. Default-on; a single opt-out
    // (--no-telemetry, ERPL_REV_NO_TELEMETRY, or DATAZOO_DISABLE_TELEMETRY)
    // short-circuits everything. --help/--smoke return earlier, so never emit.
    // app_version drops any leading "v" so PostHog shows e.g. 2026.06.13.
    std::string app_version = ERPL_REV_VERSION;
    if (!app_version.empty() && app_version.front() == 'v') app_version.erase(0, 1);
    auto &telemetry = GlobalTelemetry();
    telemetry.setEnabled(!cli.no_telemetry);
    {
        const char *rate_env = std::getenv("ERPL_REV_TELEMETRY_SAMPLE_RATE");
        if (rate_env && *rate_env) telemetry.setSampling(std::atof(rate_env));
        const char *edition_env = std::getenv("ERPL_REV_EDITION");
        const std::string edition = (edition_env && *edition_env) ? edition_env : "oss";
        telemetry.configureProduct(app_version, edition);
        telemetry.associateDeployment();
        if (const char *lic = std::getenv("ERPL_REV_LICENSE_ID"); lic && *lic)
            telemetry.associateAccount(lic);
    }

    bool quack_running = false;

    try {
        InstallHandlers(db_path, init_sql);

        auto uprog = std2uc(program_id);
        auto ugwh  = std2uc(gwhost);
        auto ugws  = std2uc(gwserv);
        // Register SEVERAL parallel connections at the gateway. With a single
        // registration (REG_COUNT=1) a second or concurrent CALL FUNCTION finds
        // the lone connection busy and fails with CM_ALLOCATE_FAILURE_RETRY
        // ("RFC connection stuck"). REG_COUNT>1 lets the automatic server accept
        // and dispatch calls in parallel and recover registrations on its own.
        const std::string reg_count = Env("ERPL_REV_REG_COUNT", "5");
        auto uregc = std2uc(reg_count);
        std::vector<RFC_CONNECTION_PARAMETER> params = {
            {cU("program_id"), uprog.data()},
            {cU("gwhost"),     ugwh.data()},
            {cU("gwserv"),     ugws.data()},
            {cU("reg_count"),  uregc.data()},
        };

        RFC_ERROR_INFO info;
        RFC_SERVER_HANDLE server =
            RfcCreateServer(params.data(), (unsigned)params.size(), &info);
        if (!server) throw_rfc("RfcCreateServer", info);

        RfcAddServerStateChangedListener(server, OnStateChange, &info);

        if (RfcLaunchServer(server, &info) != RFC_OK) {
            RfcDestroyServer(server, &info);
            throw_rfc("RfcLaunchServer", info);
        }

        log::get().Info("server", "listening (Ctrl-C to stop)",
                        {{"program_id", program_id}, {"gwhost", gwhost}, {"gwserv", gwserv}});

        // Bounded counts/kinds only — never gateway host/service or db path.
        int reg_count_n = 5;
        try { reg_count_n = (int)std::stoll(reg_count); } catch (...) {}
        telemetry.serverStarted(quack_enabled, reg_count_n);

        // Optionally expose this same in-process DuckDB to remote DuckDB clients
        // over the network via the quack extension. Treated as best-effort: if it
        // can't start (e.g. offline, engine < 1.5.3), keep serving RFC.
        if (quack_enabled) {
            const bool allow_other = !ListenIsLoopback(quack_listen);
            try {
                std::string details = StartQuackServer(quack_listen, allow_other, quack_token);
                quack_running = true;
                // `details` carries the auth token. If the operator supplied it
                // (env/flag) they already know it — redact it from the log;
                // a quack-generated token is left visible so it can be found.
                log::get().Info("quack", "network server started",
                                {{"listen", quack_listen},
                                 {"allow_other_host", allow_other ? "true" : "false", false},
                                 {"token_source", quack_token.empty() ? "generated"
                                                                       : "supplied (redacted)"},
                                 {"connect", Redact(details, quack_token)}});
            } catch (const std::exception &e) {
                log::get().Error("quack", "failed to start (continuing without it)",
                                 {{"listen", quack_listen}, {"error", e.what()}});
            }
        }

        while (g_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

        log::get().Info("server", "shutting down");
        if (quack_running) {
            try { StopQuackServer(quack_listen); }
            catch (const std::exception &e) {
                log::get().Warn("quack", "stop failed", {{"error", e.what()}});
            }
        }
        // Drain buffered telemetry before exit: the library's at-exit handler
        // discards in-flight events by design, so a server must flush explicitly.
        telemetry.flush();
        RfcShutdownServer(server, 5, &info);
        RfcDestroyServer(server, &info);
        // Tear the DuckDB bridge down HERE, while the process runtime is still
        // alive — not via the global's atexit destructor. DuckDB extensions
        // (e.g. MotherDuck) log from their own destructors, which segfaults if
        // it runs after their statics are gone during program exit.
        ShutdownHandlers();
        return 0;
    } catch (const std::exception &e) {
        // Startup failed before application_start was emitted — don't send an
        // orphan application_stop.
        log::get().Error("server", "fatal", {{"error", e.what()}});
        // A fatal is the one moment an operator definitely wants the tracker.
        // Routine per-request errors are deliberately left unannotated: this
        // process runs for weeks, and a link on every error line becomes noise.
        log::get().Error("server", datazoo::IssueHint(kBanner).substr(1));
        return 1;
    }
}
