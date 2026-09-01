#include "sap_setup.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "abap_assets.hpp"
#include "adt.hpp"
#include "cli_common.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#ifdef _WIN32
// winsock2 must precede windows.h, or windows.h pulls in the winsock 1.1
// declarations and every socket symbol collides.
#include <winsock2.h>
#include <ws2tcpip.h>   // addrinfo / getaddrinfo
#include <io.h>         // _isatty
#include <windows.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace erpl_rev::setup {
namespace {

// ---------------------------------------------------------------------------
// Credential resolution: flags > env > config > prompt.
// ---------------------------------------------------------------------------

void Resolve(Options &o, bool allow_prompt) {
    const auto cfg = cli::ReadConfig();
    cli::ResolveConn(o, cfg, allow_prompt);
    o.package    = cli::Pick(cfg, o.package, o.package_set, "ERPL_REV_PACKAGE", "package", "");
    o.program_id = cli::Pick(cfg, o.program_id, o.program_set, "ERPL_REV_PROGRAM_ID",
                             "program_id", "ERPL_REV");
    // gwhost defaults to the already-resolved host, so it must come after it.
    o.gwhost     = cli::Pick(cfg, o.gwhost, o.gwhost_set, "ERPL_REV_GWHOST", "gwhost", o.host);
    o.gwserv     = cli::Pick(cfg, o.gwserv, o.gwserv_set, "ERPL_REV_GWSERV", "gwserv", "3300");
}

// Having typed a host, a client and a user once, nobody wants to type them
// again. Offer to keep them -- but never assume: this writes a file naming a
// system and an account, and doing that unasked is a surprise.
//
// The password is excluded unless --save-password was passed explicitly, and
// even then it is called out. A config file is not a credential store.
void OfferToSave(const Options &o) {
    auto cfg = cli::ReadConfig();
    const bool already =
        cfg.count("host") && cfg["host"] == o.host &&
        cfg.count("client") && cfg["client"] == o.client &&
        cfg.count("user") && cfg["user"] == o.user &&
        (o.save_password ? cfg.count("password") > 0 : cfg.count("password") == 0);
    if (already) return;

    const auto path = cli::ConfigPath();
    if (path.empty()) return;
    if (!cli::Confirm("Save these connection settings to " + path.string() + "?", false)) return;

    cfg["host"] = o.host;
    cfg["port"] = o.port;
    cfg["client"] = o.client;
    cfg["user"] = o.user;
    cfg["gwhost"] = o.gwhost;
    cfg["gwserv"] = o.gwserv;
    cfg["program_id"] = o.program_id;
    if (!o.package.empty()) cfg["package"] = o.package;
    // Consent to store a password is given per run, not inherited from whatever
    // is already in the file: without this, one --save-password keeps the secret
    // alive through every later save that did not ask for it.
    cfg.erase("password");
    if (o.save_password) {
        cfg["password"] = o.password;
        std::cout << "  ! --save-password: the password goes into " << path.string()
                  << " in clear text (0600). Prefer SAP_PASSWORD in the environment.\n";
    }
    if (cli::WriteConfig(cfg)) std::cout << "  Saved " << path.string() << "\n";
    else std::cout << "  Could not write " << path.string() << " — carrying on.\n";
}

// The gateway service name SAP expects in the destination: sapgwNN, derived from
// the numeric port. This is the value hardcoded as `sapgw00` in the shipped
// ABAP -- wrong on every instance that is not 00, and it fails later as an empty
// SYSTEM_FAILURE rather than anything that names the cause.
std::string GwService(const std::string &gwserv) {
    if (gwserv.rfind("sapgw", 0) == 0) return gwserv;
    if (gwserv.size() == 4 && gwserv.rfind("33", 0) == 0) return "sapgw" + gwserv.substr(2);
    return gwserv;
}

// ---------------------------------------------------------------------------
// Can this machine reach the gateway at all? Answered before anything is
// deployed, because "the destination exists but nothing ever connects" is the
// most expensive failure to debug after the fact.
// ---------------------------------------------------------------------------

} // namespace

// ---------------------------------------------------------------------------
// Options parsing / help
// ---------------------------------------------------------------------------

bool ParseOption(const std::string &key, bool, const std::function<std::string()> &take,
                 Options &o) {
    // The connection flags are shared with every other command; only the
    // setup-specific ones are decided here.
    if (cli::ParseConnOption(key, take, o)) return true;
    if (key == "--package")         { o.package = take();    o.package_set = true; }
    else if (key == "--program-id") { o.program_id = take(); o.program_set = true; }
    else if (key == "--gwhost")     { o.gwhost = take();     o.gwhost_set = true; }
    else if (key == "--gwserv")     { o.gwserv = take();     o.gwserv_set = true; }
    else if (key == "--print-runbook")  { o.print_runbook = true; }
    else if (key == "--save-password")  { o.save_password = true; }
    else return false;
    return true;
}

void PrintHelp() {
    std::printf(
        "\nCommands:\n"
        "  serve (default)          Run the RFC server.\n"
        "  doctor                   Diagnose the SAP-side setup. Never writes.\n"
        "  setup                    Diagnose, show what will change, do it, verify it.\n"
        "  sql <statement>          Run DuckDB SQL against the running server.\n"
        "  sync <ls|show|create|run|run-due|schedule>\n"
        "                           Manage and run delta sync jobs.\n"
        "  replicate --table T      Full-load a SAP table (submits a background job).\n"
        "\nsetup / doctor options:\n"
        "  --sap-host <h>           ADT host   (env SAP_HOST, default localhost)\n"
        "  --sap-port <p>           ADT port   (env SAP_PORT, default 50000)\n"
        "  --sap-client <nnn>       SAP client (env SAP_CLIENT, default 001)\n"
        "  --sap-user <u>           SAP user   (env SAP_USER; prompted if unset)\n"
        "                           The password comes from SAP_PASSWORD or a prompt --\n"
        "                           never a flag, which the process list would expose.\n"
        "  --package <pkg>          Target ABAP package ($TMP, or e.g. ZERPL_CORE)\n"
        "  --program-id <id>        Gateway PROGRAM_ID (default ERPL_REV)\n"
        "  --gwhost <h>             Gateway host (default: the SAP host)\n"
        "  --gwserv <p>             Gateway service/port (default 3300)\n"
        "  --dry-run                Show the change set and stop. Writes nothing.\n"
        "  --non-interactive        Never prompt; fail instead. For CI. This is not\n"
        "                           consent to change the system -- add --yes for that.\n"
        "  --print-runbook          Print the manual steps and the Basis handout, then stop.\n"
        "  --json                   Machine-readable diagnosis (doctor).\n"
        "  --save-password          When offered to save the connection settings, include\n"
        "                           the password (clear text, 0600). Prefer SAP_PASSWORD.\n"
        "  -y, --yes                Confirm in advance; required to change the system\n"
        "                           when there is no terminal to ask on.\n");
}

// ---------------------------------------------------------------------------
// Diagnosis
// ---------------------------------------------------------------------------

const Check *Diagnosis::Find(const std::string &id) const {
    for (const auto &c : checks) if (c.id == id) return &c;
    return nullptr;
}

bool Diagnosis::AnyFailed() const {
    return std::any_of(checks.begin(), checks.end(),
                       [](const Check &c) { return c.status == Status::Fail; });
}

namespace {

Diagnosis Diagnose(const Options &o) {
    Diagnosis d;
    auto add = [&](std::string id, std::string title, Status st, std::string detail,
                   std::string remedy = "") {
        d.checks.push_back({std::move(id), std::move(title), std::move(detail),
                            std::move(remedy), st});
    };

    // 1. Can we drive ADT at all?
    const std::string ver = adt::ToolVersion();
    d.have_uvx = !ver.empty();
    if (d.have_uvx)
        add("local.adt", "erpl-adt available", Status::Ok, ver);
    else
        add("local.adt", "erpl-adt available", Status::Fail, "uvx erpl-adt did not run",
            "Install uv (https://docs.astral.sh/uv/) so setup can deploy the ABAP objects.\n"
            "        Without it, run `erpl-rev setup --print-runbook` and do the steps by hand.");

    adt::Conn conn{o.host, o.port, o.client, o.user, o.password, false, 600};

    // 2. Does ADT answer, and are the credentials good?
    if (d.have_uvx) {
        auto r = adt::Run(conn, {"search", "ZCL_ERPL_REV_UTIL", "--max", "1"});
        d.adt_reachable = r.ok();
        if (d.adt_reachable) {
            add("sap.adt", "ADT reachable, credentials accepted", Status::Ok,
                o.host + ":" + o.port + " client " + o.client + " as " + o.user);
        } else {
            add("sap.adt", "ADT reachable, credentials accepted", Status::Fail,
                r.output.substr(0, 300),
                "Check --sap-host/--sap-port/--sap-client and the credentials.\n"
                "        The ADT service must be active (SICF: /sap/bc/adt).");
        }
    } else {
        add("sap.adt", "ADT reachable, credentials accepted", Status::Unknown,
            "skipped: no erpl-adt");
    }

    // 3. Are the ABAP objects there? One representative object per layer, rather
    //    than all of them -- a partial deployment is repaired by re-running
    //    setup, which is idempotent, so the useful question is "any" not "all".
    if (d.adt_reachable) {
        auto r = adt::Run(conn, {"search", "ZCL_ERPL_REV_UTIL", "--max", "5"});
        d.objects_present = r.ok() && r.output.find("ZCL_ERPL_REV_UTIL") != std::string::npos;
        add("sap.objects", "erpl-rev ABAP objects deployed",
            d.objects_present ? Status::Ok : Status::Fail,
            d.objects_present ? "ZCL_ERPL_REV_UTIL found" : "ZCL_ERPL_REV_UTIL not found",
            d.objects_present ? "" : "Run `erpl-rev setup` to deploy them.");
    } else {
        add("sap.objects", "erpl-rev ABAP objects deployed", Status::Unknown,
            "skipped: ADT not reachable");
    }

    // 4. The destination and the function modules live in RFCDES/TFDIR, not in
    //    the repository, so they can only be read by running the probe class --
    //    which setup deploys. Before that, the honest answer is "unknown", not
    //    "missing": reporting a definite state we have not observed is worse
    //    than admitting the gap.
    if (d.objects_present) {
        auto r = adt::RunClass(conn, "ZCL_ERPL_REV_DIAG");
        const bool pong = r.output.find("PONG") != std::string::npos;
        // "the probe is not deployed" and "the round trip failed" are different
        // findings, and calling the first a failure sends people looking at the
        // gateway when nothing has been installed yet.
        const bool no_probe = r.output.find("does not exist") != std::string::npos;
        d.probe_present = !no_probe;
        d.destination_ok = pong;
        if (pong) {
            add("sap.destination", "destination ERPL_REV answers (round trip)", Status::Ok,
                "STFC_CONNECTION returned PONG");
        } else if (no_probe) {
            add("sap.destination", "destination ERPL_REV answers (round trip)", Status::Unknown,
                "probe class ZCL_ERPL_REV_DIAG is not deployed, so the round trip is untested",
                "`erpl-rev setup` deploys it and then proves the round trip.");
        } else {
            add("sap.destination", "destination ERPL_REV answers (round trip)", Status::Fail,
                r.output.substr(0, 300),
                "The destination exists only if ZCL_ERPL_REV_SETUP has run, the server is\n"
                "        running, and the gateway permits the registration. `erpl-rev setup`\n"
                "        does the first; the handout covers the third.");
        }
    } else {
        add("sap.destination", "destination ERPL_REV answers (round trip)", Status::Unknown,
            "skipped: probe class not deployed yet");
    }

    // 5. Can this machine even reach the gateway? Checked from here, because
    //    that is where the server will register from.
    d.gateway_reachable = cli::TcpReachable(o.gwhost, o.gwserv);
    add("gateway.reachable", "gateway reachable from this machine",
        d.gateway_reachable ? Status::Ok : Status::Fail,
        o.gwhost + ":" + o.gwserv,
        d.gateway_reachable ? "" :
        "Nothing can register until this host can open a TCP connection to the\n"
        "        gateway. Check the host/port (--gwhost/--gwserv), firewalls and routing.");

    // The reginfo allow-list cannot be read remotely -- it is a file on the SAP
    // host. Say so plainly rather than leaving a silent gap in the report.
    add("gateway.reginfo", "gateway registration allow-list", Status::Unknown,
        "cannot be read remotely; proven only by an actual registration",
        "If registration is refused, add the reginfo line from the generated handout\n"
        "        and reload it in SMGW (no restart needed for a content change).");

    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Planning -- pure, so every branch is testable without an SAP system
// ---------------------------------------------------------------------------

Plan MakePlan(const Diagnosis &d, const Options &o) {
    Plan p;
    p.target_package = o.package.empty() ? (d.stms_available ? "ZERPL_CORE" : "$TMP")
                                         : o.package;
    p.needs_transport = p.target_package != "$TMP";

    // Deploy when anything is missing OR when the round-trip probe specifically
    // is absent: checking one representative object is enough to notice a bare
    // system, but a system deployed before the probe existed looks "present"
    // while the one class setup needs to prove success is not there.
    if (!d.objects_present || !d.probe_present) {
        p.deploy_objects = true;
        p.create_function_group = true;
        p.run_mkfm = true;
        p.run_setup_class = true;
    } else if (!d.destination_ok) {   // includes "probe absent, so unproven"
        // Objects are there but the round trip does not work. Re-running the two
        // classruns is cheap and idempotent, and repairs the common case of a
        // system that reaped $TMP or never had the setup class run.
        p.create_function_group = true;
        p.run_mkfm = true;
        p.run_setup_class = true;
    }

    if (p.target_package == "$TMP") {
        p.manual_steps.push_back(
            "$TMP is a local, non-transportable package and some systems reap it. "
            "Fine to evaluate with; use --package ZERPL_CORE for anything lasting.");
    }
    if (!d.gateway_reachable) {
        p.manual_steps.push_back(
            "Make the gateway reachable from this host (" + o.gwhost + ":" + o.gwserv +
            ") -- nothing can register until then.");
    }
    p.manual_steps.push_back(
        "Add the reginfo line from erpl-rev-basis-handout.md and reload it in SMGW.");
    p.manual_steps.push_back(
        "Create the RFC communication user and role from the handout, if this system "
        "requires a dedicated one.");

    p.nothing_to_do = !p.deploy_objects && !p.run_mkfm && !p.run_setup_class;
    return p;
}

const std::vector<std::string> &FunctionModuleNames() {
    static const std::vector<std::string> kNames = {
        "Z_DUCKDB_QUERY", "Z_DUCKDB_INGEST", "Z_DUCKDB_SNAPSHOT_MERGE",
        "Z_DUCKDB_CDC_PLAN", "Z_DUCKDB_CDC_APPLY", "Z_DUCKDB_OPEN",
        "Z_DUCKDB_FETCH", "Z_DUCKDB_CLOSE",
    };
    return kNames;
}

namespace {

// Pull an exact `key=value` out of the comma-separated RFCDES option string.
// Exact, not prefix: `N=ERPL_REV` must not be satisfied by `N=ERPL_REV2`, and
// `g=sapgw0` must not be satisfied by `g=sapgw00`.
bool OptionEquals(const std::string &opts, const std::string &key,
                  const std::string &want) {
    size_t pos = 0;
    while (pos <= opts.size()) {
        const size_t comma = opts.find(',', pos);
        const size_t len = (comma == std::string::npos ? opts.size() : comma) - pos;
        const std::string tok = opts.substr(pos, len);
        const size_t eq = tok.find('=');
        if (eq != std::string::npos && tok.substr(0, eq) == key)
            return tok.substr(eq + 1) == want;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

} // namespace

bool SetupClassSucceeded(const std::string &output, const std::string &program_id,
                         const std::string &gwservice, std::string &why) {
    // The class ends with one line read back out of RFCDES, e.g.
    //   setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=ERPL_REV,Y=2,...]
    // subrc is the SELECT's, so subrc=0 means the destination row really exists
    // -- a stronger statement than the swallowed FM return code.
    const std::string line = cli::LineStartingWith(output, "setup subrc=");
    if (line.empty()) {
        why = "ZCL_ERPL_REV_SETUP printed no result line (short dump?)";
        return false;
    }
    if (line.compare(0, 14, "setup subrc=0 ") != 0) {
        why = "the ERPL destination is not in RFCDES after the run";
        return false;
    }
    const size_t ob = line.find("opts=[");
    const size_t cb = ob == std::string::npos ? std::string::npos : line.rfind(']');
    if (ob == std::string::npos || cb == std::string::npos || cb < ob) {
        why = "could not read the destination options back from RFCDES";
        return false;
    }
    const std::string opts = line.substr(ob + 6, cb - (ob + 6));

    // H=%%RFCSERVER%% is registration mode. Without it the destination is in
    // "Start" mode, which fails much later as an empty SYSTEM_FAILURE.
    if (!OptionEquals(opts, "H", "%%RFCSERVER%%")) {
        why = "destination exists but is not in registration mode (method='R')";
        return false;
    }
    if (!OptionEquals(opts, "N", program_id)) {
        why = "destination PROGRAM_ID is not " + program_id;
        return false;
    }
    // Catches the shipped-`sapgw00`-on-instance-42 case at the moment it happens
    // rather than as a connection that never arrives.
    if (!OptionEquals(opts, "g", gwservice)) {
        why = "destination gateway service is not " + gwservice;
        return false;
    }
    return true;
}

bool MkfmSucceeded(const std::string &output, const std::vector<std::string> &expected,
                   std::string &why) {
    // One `<NAME> tfdir subrc=0 fmode=R` line per module, read back from TFDIR
    // after the insert. fmode=R is the part that matters: a module that exists
    // but is not remote-enabled cannot be called over RFC at all.
    //
    // Bound to the name each time: counting bare `tfdir subrc=0 fmode=R`
    // fragments would accept one module reporting eight times.
    std::vector<std::string> missing;
    for (const auto &name : expected) {
        if (cli::LineStartingWith(output, name + " tfdir subrc=0 fmode=R").empty())
            missing.push_back(name);
    }
    if (!missing.empty()) {
        why = std::to_string(expected.size() - missing.size()) + " of " +
              std::to_string(expected.size()) +
              " Z_DUCKDB_* modules are remote-enabled in TFDIR; missing: ";
        for (size_t i = 0; i < missing.size(); i++) {
            if (i) why += ", ";
            why += missing[i];
        }
        return false;
    }
    return true;
}

std::string RenderBasisHandout(const Diagnosis &d, const Options &o,
                               const std::string &server_host) {
    std::ostringstream s;
    s << "# erpl-rev — steps that need Basis rights\n\n"
      << "Generated by `erpl-rev setup` for SAP system " << o.host << ":" << o.port
      << " (client " << o.client << ").\n"
      // With --print-runbook nothing has run yet, so claiming otherwise would
      // send a Basis team looking for objects that are not there.
      << (d.checks.empty()
              ? "These are the parts a client cannot do; the rest is what `erpl-rev setup` will do.\n\n"
              : "Everything else has already been done; these are the parts a client cannot do.\n\n")
      << "## 1. Allow the gateway registration\n\n"
      << "The server registers the PROGRAM_ID `" << o.program_id << "` at gateway `"
      << o.gwhost << ":" << o.gwserv << "`.\n"
      << "Add to the `reginfo` file (SMGW → Goto → Expert Functions → External Security,\n"
      << "or the file named by the `gw/reg_info` profile parameter):\n\n"
      << "```\n"
      << "P TP=" << o.program_id << " HOST=" << server_host
      << " ACCESS=" << o.gwhost << " CANCEL=" << o.gwhost << "\n"
      << "D TP=*\n"
      << "```\n\n"
      << "`TP` is the PROGRAM_ID exactly, no wildcards. `HOST` is the machine erpl-rev\n"
      << "runs on. Keep the trailing deny line, or the allow-list permits everything.\n\n"
      << "**Reloading reginfo needs no restart**: SMGW → Goto → Expert Functions →\n"
      << "External Security → Reread.\n\n"
      << "## 2. Profile parameters — only if registration is still refused\n\n"
      << "`gw/acl_mode = 1` (enforce the ACL) with `gw/reg_info` pointing at the file\n"
      << "above. **Changing these two needs an instance restart**, unlike the file\n"
      << "content itself. With `acl_mode=1` and no reginfo, every registration is\n"
      << "denied — which looks exactly like a network problem from the client side.\n\n"
      << "## 3. RFC communication user and role\n\n"
      << "Not created automatically: creating authorisation objects unattended is not\n"
      << "something a deployment tool should do.\n\n"
      << "- SU01: a user of type **Communications Data** (not Dialog).\n"
      << "- PFCG role with `S_RFC`: `ACTVT=16`, `RFC_TYPE=FUGR`, `RFC_NAME=ZERPL_REV`.\n"
      << "  That grants exactly the eight `Z_DUCKDB_*` modules and nothing else.\n\n"
      << "## What setup already did\n\n";
    for (const auto &c : d.checks) {
        if (c.status == Status::Ok) s << "- ok: " << c.title << " — " << c.detail << "\n";
    }
    s << "\n## What was still failing when this was written\n\n";
    bool any = false;
    for (const auto &c : d.checks) {
        if (c.status == Status::Fail) { s << "- " << c.title << ": " << c.detail << "\n"; any = true; }
    }
    if (!any) s << "- nothing\n";
    return s.str();
}

// ---------------------------------------------------------------------------
// doctor
// ---------------------------------------------------------------------------

namespace {

void PrintReport(const Diagnosis &d) {
    std::cout << "\nerpl-rev doctor\n\n";
    for (const auto &c : d.checks) {
        std::cout << "[" << cli::Mark(c.status) << "] " << c.title << "\n";
        if (!c.detail.empty()) std::cout << "        " << c.detail << "\n";
        if (c.status != Status::Ok && !c.remedy.empty())
            std::cout << "        → " << c.remedy << "\n";
    }
    std::cout << "\n";
}

std::string JsonEscape(const std::string &s) {
    std::string o;
    for (char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': break;
            case '\t': o += "\\t";  break;
            default:   o += ch;
        }
    }
    return o;
}

void PrintJson(const Diagnosis &d) {
    auto name = [](Status s) {
        switch (s) {
            case Status::Ok: return "ok";
            case Status::Warn: return "warn";
            case Status::Fail: return "fail";
            default: return "unknown";
        }
    };
    std::cout << "{\"ready\":" << (d.AnyFailed() ? "false" : "true") << ",\"checks\":[";
    for (size_t i = 0; i < d.checks.size(); i++) {
        const auto &c = d.checks[i];
        if (i) std::cout << ",";
        std::cout << "{\"id\":\"" << JsonEscape(c.id) << "\",\"status\":\"" << name(c.status)
                  << "\",\"title\":\"" << JsonEscape(c.title) << "\",\"detail\":\""
                  << JsonEscape(c.detail) << "\",\"remedy\":\"" << JsonEscape(c.remedy) << "\"}";
    }
    std::cout << "]}\n";
}

} // namespace

int RunDoctor(Options o) {
    Resolve(o, !o.non_interactive && cli::IsTty() && !o.json);
    const Diagnosis d = Diagnose(o);
    if (o.json) PrintJson(d);
    else {
        PrintReport(d);
        if (d.AnyFailed())
            std::cout << "Not ready. Run `erpl-rev setup` to fix what can be fixed\n"
                         "automatically, or `erpl-rev setup --print-runbook` to do it by hand.\n\n";
        else
            std::cout << "Ready.\n\n";
    }
    return d.AnyFailed() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

namespace {

// Write an embedded ABAP source to a temp file, substituting the values that are
// hardcoded in the shipped source. A classrun takes no parameters, so the only
// way to parameterise the destination's gateway service and program id is to
// template the source before deploying it.
std::filesystem::path Materialise(const abap::Asset &a, const Options &o,
                                  const std::filesystem::path &dir) {
    std::string src(a.text);
    auto sub = [&](const std::string &from, const std::string &to) {
        for (size_t p = 0; (p = src.find(from, p)) != std::string::npos; p += to.size())
            src.replace(p, from.size(), to);
    };
    sub("'sapgw00'", "'" + GwService(o.gwserv) + "'");
    if (o.program_id != "ERPL_REV") sub("program = 'ERPL_REV'", "program = '" + o.program_id + "'");

    const auto path = dir / a.file;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(src.data(), static_cast<std::streamsize>(src.size()));
    return path;
}

} // namespace

int RunSetup(Options o) {
    const bool interactive = !o.non_interactive && cli::IsTty();
    Resolve(o, interactive);

    if (o.non_interactive && (o.user.empty() || o.password.empty())) {
        std::fprintf(stderr,
            "erpl-rev setup: --non-interactive needs SAP_USER and SAP_PASSWORD "
            "in the environment (or --sap-user).\n");
        return 2;
    }

    if (interactive) OfferToSave(o);

    const std::string server_host = cli::LocalHostname();

    if (o.print_runbook) {
        Diagnosis empty;
        std::cout << RenderBasisHandout(empty, o, server_host);
        return 0;
    }

    std::cout << "Diagnosing " << o.host << ":" << o.port << " (client " << o.client << ")…\n";
    Diagnosis d = Diagnose(o);
    PrintReport(d);

    const Plan p = MakePlan(d, o);

    std::cout << "Planned changes\n\n";
    if (p.nothing_to_do) {
        std::cout << "  nothing — the SAP side is already set up.\n\n";
    } else {
        if (p.deploy_objects)
            std::cout << "  · deploy " << abap::ProductionAssets().size()
                      << " ABAP objects into " << p.target_package << "\n";
        if (p.create_function_group)
            std::cout << "  · create function group ZERPL_REV\n";
        if (p.run_setup_class)
            std::cout << "  · run ZCL_ERPL_REV_SETUP  (destination " << o.program_id
                      << ", gateway " << GwService(o.gwserv) << ", registration mode)\n";
        if (p.run_mkfm)
            std::cout << "  · run ZCL_ERPL_REV_MKFM   (the eight Z_DUCKDB_* function modules)\n";
        std::cout << "\n";
    }
    if (!p.manual_steps.empty()) {
        std::cout << "Left for a human (setup cannot do these from a client)\n\n";
        for (const auto &m : p.manual_steps) std::cout << "  · " << m << "\n";
        std::cout << "\n";
    }

    if (o.dry_run) {
        std::cout << "--dry-run: nothing was changed.\n";
        return 0;
    }
    if (p.nothing_to_do) {
        std::cout << "Nothing to do.\n";
        return 0;
    }
    if (!d.have_uvx || !d.adt_reachable) {
        std::cout << "Cannot deploy automatically (see the failures above).\n"
                     "Run `erpl-rev setup --print-runbook` for the manual steps.\n";
        return 1;
    }
    if (interactive) {
        if (!cli::Confirm("Apply these changes to " + o.host + "?", o.assume_yes)) {
            std::cout << "Aborted; nothing was changed.\n";
            return 1;
        }
    } else if (!o.assume_yes) {
        // Nobody was asked, so nobody consented. --non-interactive says "do not
        // prompt me", which is not the same as "yes, change my ERP system": a CI
        // job that grew a TTY-less shell should not start writing to production
        // because of it. Only -y/--yes means yes.
        std::cout << "Refusing to change " << o.host << " without confirmation: there is no\n"
                     "terminal to ask on. Re-run with --yes if that is what you meant, or\n"
                     "with --dry-run to see the plan only.\n";
        return 2;
    }

    // --- execute -----------------------------------------------------------
    adt::Conn conn{o.host, o.port, o.client, o.user, o.password, false, 600};
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec) / "erpl-rev-abap";
    std::filesystem::create_directories(tmp, ec);

    int failures = 0;
    if (p.deploy_objects) {
        // The order below is not cosmetic: the interface precedes util because
        // replicate's signature references it, typemap precedes util because util
        // depends on it, and the worker report precedes the replicate report which
        // SUBMITs it. deploy-abap.sh learned each of these the hard way.
        for (const auto &a : abap::ProductionAssets()) {
            const auto file = Materialise(a, o, tmp);
            adt::CreateObject(conn, std::string(a.adt_type), std::string(a.name),
                              p.target_package, std::string(a.description));
            auto w = adt::WriteSource(conn, std::string(a.name), file.string(),
                                      std::string(a.src_type));
            const bool ok = adt::ActivationSucceeded(w);
            std::cout << (ok ? "  ok   " : "  FAIL ") << a.name << "\n";
            if (!ok) {
                failures++;
                std::cout << "        " << w.output.substr(0, 200) << "\n";
            }
        }
    }
    if (p.create_function_group) {
        // Must exist before MKFM: RS_FUNCTIONMODULE_INSERT fails with
        // invalid_function_pool otherwise, and nothing else creates it.
        adt::CreateObject(conn, "FUGR/F", "ZERPL_REV", p.target_package,
                          "erpl-rev RFC function group");
        std::cout << "  ok   function group ZERPL_REV\n";
    }
    if (p.run_setup_class) {
        auto r = adt::RunClass(conn, "ZCL_ERPL_REV_SETUP");
        std::string why;
        const bool ok = r.ok() &&
            SetupClassSucceeded(r.output, o.program_id, GwService(o.gwserv), why);
        std::cout << (ok ? "  ok   " : "  FAIL ") << "ZCL_ERPL_REV_SETUP (destination)\n";
        if (!ok) {
            failures++;
            std::cout << "        " << (why.empty() ? r.output.substr(0, 300) : why) << "\n";
        }
    }
    if (p.run_mkfm) {
        auto r = adt::RunClass(conn, "ZCL_ERPL_REV_MKFM");
        std::string why;
        const bool ok = r.ok() && MkfmSucceeded(r.output, FunctionModuleNames(), why);
        std::cout << (ok ? "  ok   " : "  FAIL ") << "ZCL_ERPL_REV_MKFM (function modules)\n";
        if (!ok) {
            failures++;
            std::cout << "        " << (why.empty() ? r.output.substr(0, 300) : why) << "\n";
        }
    }

    // --- the handout, always -----------------------------------------------
    Diagnosis after = Diagnose(o);
    const std::string handout = RenderBasisHandout(after, o, server_host);
    const auto hp = std::filesystem::current_path(ec) / "erpl-rev-basis-handout.md";
    { std::ofstream h(hp, std::ios::trunc); h << handout; }
    std::cout << "\nWrote " << hp.string() << " — the steps that need Basis rights.\n";

    // --- verify -------------------------------------------------------------
    // "Deployed" is not "working". The only claim worth making is that a round
    // trip happened, so that is the one setup makes.
    const Check *rt = after.Find("sap.destination");
    if (rt && rt->status == Status::Ok) {
        std::cout << "\nRound trip verified: ABAP called out through " << o.program_id
                  << " and got an answer.\n";
        return failures ? 1 : 0;
    }
    std::cout << "\nDeployed, but the round trip did NOT complete yet.\n"
                 "That is expected if the server is not running, or if the gateway has not\n"
                 "been told to accept the registration — see the handout, then start the\n"
                 "server and re-run `erpl-rev doctor`.\n";
    return 1;
}

} // namespace erpl_rev::setup
