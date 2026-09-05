// Tests for the pure half of `erpl-rev setup`: the planner and the handout.
//
// The planner is the piece that decides what to write into a customer's SAP
// system, so every branch of it is worth pinning down here rather than
// discovering against a live system. Both functions are deliberately free of
// I/O -- they take a Diagnosis and produce a decision -- which is what makes
// this testable at all.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "sap_setup.hpp"

using namespace erpl_rev::setup;
using Catch::Matchers::ContainsSubstring;

namespace {

Options DefaultOptions() {
    Options o;
    o.host = "sap.example.com";
    o.port = "50000";
    o.client = "001";
    o.user = "ERPLREV";
    o.program_id = "ERPL_REV";
    o.gwhost = "sap.example.com";
    o.gwserv = "sapgw00";
    return o;
}

// A system where everything already works.
Diagnosis HealthyDiagnosis() {
    Diagnosis d;
    d.have_uvx = true;
    d.adt_reachable = true;
    d.objects_present = true;
    d.probe_present = true;
    d.destination_ok = true;
    d.gateway_reachable = true;
    return d;
}

bool HasStepContaining(const Plan &p, const std::string &needle) {
    for (const auto &s : p.manual_steps)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("a bare system gets the full deployment", "[setup]") {
    Diagnosis d;   // nothing present
    d.have_uvx = true;
    d.adt_reachable = true;
    const Plan p = MakePlan(d, DefaultOptions());

    CHECK(p.deploy_objects);
    CHECK(p.create_function_group);
    CHECK(p.run_mkfm);
    CHECK(p.run_setup_class);
    CHECK_FALSE(p.nothing_to_do);
}

TEST_CASE("a working system plans no writes at all", "[setup]") {
    const Plan p = MakePlan(HealthyDiagnosis(), DefaultOptions());

    CHECK(p.nothing_to_do);
    CHECK_FALSE(p.deploy_objects);
    CHECK_FALSE(p.run_mkfm);
    CHECK_FALSE(p.run_setup_class);
    // Idempotence is the property that matters: re-running setup against a
    // healthy system must not touch it.
}

TEST_CASE("objects present but round trip broken is repaired, not redeployed", "[setup]") {
    Diagnosis d = HealthyDiagnosis();
    d.destination_ok = false;
    const Plan p = MakePlan(d, DefaultOptions());

    CHECK_FALSE(p.deploy_objects);   // sources are fine; do not rewrite them
    CHECK(p.run_setup_class);        // the destination is what is missing
    CHECK(p.run_mkfm);
}

TEST_CASE("a missing probe class forces a deployment even when other objects exist", "[setup]") {
    // Regression: checking one representative object made a system deployed
    // before ZCL_ERPL_REV_DIAG existed look complete, so setup would plan to
    // prove a round trip using a class it had never deployed.
    Diagnosis d = HealthyDiagnosis();
    d.probe_present = false;
    d.destination_ok = false;
    const Plan p = MakePlan(d, DefaultOptions());

    CHECK(p.deploy_objects);
}

TEST_CASE("package selection follows STMS, and --package overrides it", "[setup]") {
    Diagnosis d;
    d.have_uvx = true;
    d.adt_reachable = true;

    SECTION("no transport routes: $TMP, with the reaping warning") {
        d.stms_available = false;
        const Plan p = MakePlan(d, DefaultOptions());
        CHECK(p.target_package == "$TMP");
        CHECK_FALSE(p.needs_transport);
        CHECK(HasStepContaining(p, "non-transportable"));
    }

    SECTION("transport routes exist: a real package") {
        d.stms_available = true;
        const Plan p = MakePlan(d, DefaultOptions());
        CHECK(p.target_package == "ZERPL_CORE");
        CHECK(p.needs_transport);
        CHECK_FALSE(HasStepContaining(p, "non-transportable"));
    }

    SECTION("an explicit --package wins over the diagnosis") {
        d.stms_available = false;
        Options o = DefaultOptions();
        o.package = "ZCUSTOM";
        o.package_set = true;
        const Plan p = MakePlan(d, o);
        CHECK(p.target_package == "ZCUSTOM");
        CHECK(p.needs_transport);
    }
}

TEST_CASE("an unreachable gateway is called out as a blocker", "[setup]") {
    Diagnosis d = HealthyDiagnosis();
    d.gateway_reachable = false;
    d.destination_ok = false;
    const Plan p = MakePlan(d, DefaultOptions());

    CHECK(HasStepContaining(p, "gateway reachable"));
    CHECK(HasStepContaining(p, "sapgw00"));
}

TEST_CASE("the reginfo and RFC-user steps are always handed over", "[setup]") {
    // These two cannot be done over ADT at all, so they must appear even for a
    // system that otherwise needs nothing -- the Basis team still has to have
    // done them, and setup has no way to check.
    const Plan p = MakePlan(HealthyDiagnosis(), DefaultOptions());
    CHECK(HasStepContaining(p, "reginfo"));
    CHECK(HasStepContaining(p, "RFC communication user"));
}

TEST_CASE("the handout is filled in for the system that was diagnosed", "[setup]") {
    Options o = DefaultOptions();
    o.program_id = "ERPL_PROD";
    o.gwserv = "sapgw42";
    const std::string h = RenderBasisHandout(HealthyDiagnosis(), o, "ingest01.corp");

    // The whole point is that nothing is left for the reader to compose.
    CHECK_THAT(h, ContainsSubstring("TP=ERPL_PROD"));
    CHECK_THAT(h, ContainsSubstring("HOST=ingest01.corp"));
    CHECK_THAT(h, ContainsSubstring("sapgw42"));
    CHECK_THAT(h, ContainsSubstring("sap.example.com:50000"));
    CHECK_THAT(h, ContainsSubstring("D TP=*"));      // the deny line, or the ACL is moot
    CHECK_THAT(h, ContainsSubstring("RFC_NAME=ZERPL_REV"));
    CHECK_THAT(h, ContainsSubstring("restart"));     // acl_mode needs one; say so
}

TEST_CASE("the handout reports failures rather than claiming success", "[setup]") {
    Diagnosis d = HealthyDiagnosis();
    d.destination_ok = false;
    Check c;
    c.id = "sap.roundtrip";
    c.title = "destination ERPL_REV answers (round trip)";
    c.detail = "subrc=2 CM_ALLOCATE_FAILURE_RETRY";
    c.status = Status::Fail;
    d.checks.push_back(c);

    const std::string h = RenderBasisHandout(d, DefaultOptions(), "ingest01.corp");
    CHECK_THAT(h, ContainsSubstring("CM_ALLOCATE_FAILURE_RETRY"));
}

// ---------------------------------------------------------------------------
// Classrun outcome parsing. Both ABAP classes exit 0 whatever happens, so these
// two functions are the only thing standing between a failed deployment and
// setup reporting success. The strings below are verbatim from a live A4H run.
// ---------------------------------------------------------------------------

TEST_CASE("a good ZCL_ERPL_REV_SETUP run is recognised", "[setup]") {
    const std::string out =
        "setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=ERPL_REV,Y=2,h=2,y=-2,z=-2,W=Y,]";
    std::string why;
    CHECK(SetupClassSucceeded(out, "ERPL_REV", "sapgw00", why));
    CHECK(why.empty());
}

TEST_CASE("ZCL_ERPL_REV_SETUP failures are not mistaken for success", "[setup]") {
    std::string why;

    SECTION("the destination was never written") {
        // RFC_MODIFY_TCPIP_DESTINATION failed; EXCEPTIONS OTHERS = 9 swallowed it
        // and the class still exited 0.
        CHECK_FALSE(SetupClassSucceeded("setup subrc=4 opts=[]", "ERPL_REV", "sapgw00", why));
        CHECK_THAT(why, ContainsSubstring("RFCDES"));
    }
    SECTION("created in Start mode instead of registration mode") {
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[g=sapgw00,N=ERPL_REV,]", "ERPL_REV", "sapgw00", why));
        CHECK_THAT(why, ContainsSubstring("registration mode"));
    }
    SECTION("gateway service does not match this instance") {
        // The shipped ABAP says sapgw00; on instance 42 that silently produces a
        // destination nothing ever connects to.
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=ERPL_REV,]",
            "ERPL_REV", "sapgw42", why));
        CHECK_THAT(why, ContainsSubstring("sapgw42"));
    }
    SECTION("wrong program id") {
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=OTHER,]",
            "ERPL_REV", "sapgw00", why));
    }
    SECTION("short dump: no result line at all") {
        CHECK_FALSE(SetupClassSucceeded("Internal Server Error", "ERPL_REV", "sapgw00", why));
        CHECK_THAT(why, ContainsSubstring("short dump"));
    }
}

TEST_CASE("MKFM is judged by TFDIR, not by the insert return code", "[setup]") {
    std::string out;
    // Driven by the contract rather than a fourth copy of the name list, so
    // adding an FM does not silently leave this test asserting the old set.
    const auto names = FunctionModuleNames();
    const auto total = std::to_string(names.size());
    // subrc=3 is "already exists" -- a perfectly good outcome on a re-run.
    for (const auto &n : names) {
        out += std::string(n) + " insert subrc=3 [FL/050] already exists\n";
        out += std::string(n) + " tfdir subrc=0 fmode=R\n";
    }
    std::string why;
    CHECK(MkfmSucceeded(out, FunctionModuleNames(), why));

    SECTION("a module that exists but is not remote-enabled does not count") {
        // fmode blank: callable in ABAP, invisible over RFC.
        auto broken = out;
        const auto p = broken.find("Z_DUCKDB_CLOSE tfdir subrc=0 fmode=R");
        broken.replace(p, 36, "Z_DUCKDB_CLOSE tfdir subrc=0 fmode= ");
        CHECK_FALSE(MkfmSucceeded(broken, FunctionModuleNames(), why));
        CHECK_THAT(why, ContainsSubstring(std::to_string(names.size() - 1) + " of " + total));
    }
    SECTION("invalid_function_pool: nothing was created, class still exited 0") {
        CHECK_FALSE(MkfmSucceeded("Z_DUCKDB_QUERY insert subrc=4 invalid_function_pool\n",
                                  FunctionModuleNames(), why));
    }
}


// ---------------------------------------------------------------------------
// False-pass regressions. Every input below was accepted by the first version
// of these parsers, which matched loose substrings anywhere in the output and
// counted anonymous fragments. A parser that decides whether a customer's ERP
// was configured correctly has to be exact.
// ---------------------------------------------------------------------------

TEST_CASE("RFCDES options are matched as exact tokens, not prefixes", "[setup]") {
    std::string why;

    SECTION("a longer PROGRAM_ID must not satisfy a shorter one") {
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=ERPL_REV2,]",
            "ERPL_REV", "sapgw00", why));
    }
    SECTION("a longer gateway service must not satisfy a shorter one") {
        // Expecting sapgw0 on a destination pointing at sapgw00 is a real
        // mismatch: instance 0 is not instance 00's gateway.
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[H=%%RFCSERVER%%,g=sapgw00,N=ERPL_REV,]",
            "ERPL_REV", "sapgw0", why));
    }
    SECTION("values echoed elsewhere in the output do not count") {
        // The destination is empty; the expected values appear only in a
        // diagnostic line. Reading them from anywhere would report success on a
        // system with no usable destination at all.
        CHECK_FALSE(SetupClassSucceeded(
            "setup subrc=0 opts=[]\n"
            "debug: H=%%RFCSERVER%% N=ERPL_REV g=sapgw00\n",
            "ERPL_REV", "sapgw00", why));
    }
}

TEST_CASE("MKFM lines are bound to module names", "[setup]") {
    // One module reporting eight times is not eight modules.
    std::string out;
    for (int i = 0; i < 8; i++) out += "Z_DUCKDB_QUERY tfdir subrc=0 fmode=R\n";
    std::string why;
    CHECK_FALSE(MkfmSucceeded(out, FunctionModuleNames(), why));
    CHECK_THAT(why, ContainsSubstring("Z_DUCKDB_CLOSE"));   // names what is absent
}

// ---------------------------------------------------------------------------
// S_DEVELOP. setup creates and activates ABAP, and sync/replicate generate a
// temporary class, so both need a developer authorisation that a production
// service user normally does not have. Getting this verdict wrong either
// blocks a capable user or lets setup fail halfway through a deploy.
// ---------------------------------------------------------------------------

TEST_CASE("the S_DEVELOP verdict is read from the probe", "[setup]") {
    // subrc 0 from AUTHORITY-CHECK means granted.
    CHECK(DevelopAuthFromProbe("s_develop create=0 change=0 user=DEVELOPER") == Status::Ok);

    SECTION("create refused") {
        CHECK(DevelopAuthFromProbe("s_develop create=4 change=0 user=SVC") == Status::Fail);
    }
    SECTION("change refused") {
        CHECK(DevelopAuthFromProbe("s_develop create=0 change=4 user=SVC") == Status::Fail);
    }
    SECTION("both refused") {
        CHECK(DevelopAuthFromProbe("s_develop create=4 change=4 user=SVC") == Status::Fail);
    }
    SECTION("an older probe that does not report it is Unknown, not Ok") {
        // Silence must never read as permission: a probe deployed before this
        // check existed says nothing, and guessing Ok would let setup start
        // writing and fail partway.
        CHECK(DevelopAuthFromProbe("subrc=0\nmsg=[]\necho=[hi]") == Status::Unknown);
        CHECK(DevelopAuthFromProbe("") == Status::Unknown);
    }
    SECTION("the marker is only honoured at the start of a line") {
        CHECK(DevelopAuthFromProbe("note: s_develop create=0 change=0") == Status::Unknown);
    }
}

// ---------------------------------------------------------------------------
// The handout under a tunnel
//
// reginfo's HOST= is matched against the source address the GATEWAY observes. With
// a tunnel that is the exit node, not this machine, so printing a gethostname()
// value as if it were authoritative generates a line that refuses the registration
// -- and the usual field fix for a refused registration is the wildcard the deny
// line exists to prevent. Better to say where the real value comes from.
// ---------------------------------------------------------------------------

TEST_CASE("without a tunnel the handout still names this host", "[setup]") {
    Options o = DefaultOptions();
    const std::string h = RenderBasisHandout(HealthyDiagnosis(), o, "ingest01.corp");
    CHECK_THAT(h, ContainsSubstring("HOST=ingest01.corp"));
    CHECK_THAT(h, ContainsSubstring("`HOST` is the machine erpl-rev runs on."));
}

TEST_CASE("under a tunnel the handout refuses to guess HOST=", "[setup]") {
    Options o = DefaultOptions();
    o.tunnel_secret = "sap";
    const std::string h = RenderBasisHandout(HealthyDiagnosis(), o, "ingest01.corp");

    // The inferred hostname must not appear as the ACL value at all.
    CHECK_THAT(h, !ContainsSubstring("HOST=ingest01.corp"));
    CHECK_THAT(h, ContainsSubstring("HOST=<see below>"));
    // ... and the reader is told where to get it, and warned off the wildcard.
    CHECK_THAT(h, ContainsSubstring("SMGW"));
    CHECK_THAT(h, ContainsSubstring("wildcard"));
    CHECK_THAT(h, ContainsSubstring("sap"));          // names the tunnel secret
    // ACCESS/CANCEL still name the real gateway, not the forward's local end.
    CHECK_THAT(h, ContainsSubstring("ACCESS=sap.example.com"));
}
