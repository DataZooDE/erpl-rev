#include "abap_skeletons.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "abap_codegen.hpp"

namespace erpl_rev::abapgen {

namespace {

// Every generated class prints its results with the same nonce, so a stale
// class from an earlier run cannot be mistaken for this one.
constexpr const char *kReplicate = R"ABAP(
CLASS $ERPL_CLASS$ DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS $ERPL_CLASS$ IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    CONSTANTS lc_n TYPE string VALUE $ERPL_NONCE$.
    DATA lv_jc TYPE tbtcjob-jobcount.
    " jobname is btcjob (CHAR 32), not a string. Passing an inline-declared
    " string here dumps inside JOB_OPEN rather than returning a subrc, which
    " surfaces as an opaque HTTP 500 from the classrun.
    DATA lv_jn TYPE btcjob.
    lv_jn = |ERPLCLI_{ sy-uzeit }|.

    " Submit the same report the SAP GUI runs, as a background job. The CLI
    " returns as soon as the job is queued: a full load can outlive any HTTP
    " read timeout, and a connection that dies mid-load must not look like a
    " failed load.
    CALL FUNCTION 'JOB_OPEN'
      EXPORTING jobname = lv_jn
      IMPORTING jobcount = lv_jc
      EXCEPTIONS OTHERS = 4.
    IF sy-subrc <> 0.
      out->write( |ERPL-CLI/{ lc_n } status=error| ).
      out->write( |ERPL-CLI/{ lc_n } error=JOB_OPEN failed subrc={ sy-subrc }| ).
      RETURN.
    ENDIF.

    SUBMIT z_erpl_rev_replicate
      WITH p_tab    = $ERPL_TABLE$
      WITH p_target = $ERPL_TARGET$
      WITH p_cols   = $ERPL_COLUMNS$
      WITH p_where  = $ERPL_WHERE$
      WITH p_cparm  = $ERPL_CDSPARAMS$
      WITH p_init   = $ERPL_INIT$
      WITH p_up     = $ERPL_UPSERT$
      WITH p_ins    = $ERPL_INSERT$
      WITH p_batch  = $ERPL_BATCH$
      WITH p_maxrow = $ERPL_MAXROWS$
      WITH p_trunc  = $ERPL_TRUNCATE$
      WITH p_verify = $ERPL_VERIFY$
      WITH p_par    = $ERPL_PARALLEL$
      WITH p_pcol   = $ERPL_PARTCOL$
      WITH p_jobs   = $ERPL_JOBS$
      WITH r_kd     = $ERPL_KIND_D$
      WITH r_kp     = $ERPL_KIND_P$
      WITH r_kt     = $ERPL_KIND_T$
      WITH p_dest   = $ERPL_DEST$
      WITH p_part   = $ERPL_PARTBY$
      VIA JOB lv_jn NUMBER lv_jc AND RETURN.

    CALL FUNCTION 'JOB_CLOSE'
      EXPORTING jobcount = lv_jc jobname = lv_jn strtimmed = abap_true
      EXCEPTIONS OTHERS = 4.
    IF sy-subrc <> 0.
      out->write( |ERPL-CLI/{ lc_n } status=error| ).
      out->write( |ERPL-CLI/{ lc_n } error=JOB_CLOSE failed subrc={ sy-subrc }| ).
      RETURN.
    ENDIF.

    out->write( |ERPL-CLI/{ lc_n } status=submitted| ).
    out->write( |ERPL-CLI/{ lc_n } job={ lv_jn }| ).
    out->write( |ERPL-CLI/{ lc_n } jobcount={ lv_jc }| ).
    out->write( |ERPL-CLI/{ lc_n } target=$ERPL_TARGET_TXT$| ).
  ENDMETHOD.
ENDCLASS.
)ABAP";

constexpr const char *kRegister = R"ABAP(
CLASS $ERPL_CLASS$ DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS $ERPL_CLASS$ IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    CONSTANTS lc_n TYPE string VALUE $ERPL_NONCE$.

    " Registering through the ABAP API rather than writing the DuckDB table
    " from the CLI keeps one writer and reuses the granularity gate that
    " rejects, for instance, a date-only column on a sub-hourly cadence.
    DATA(lv_err) = zcl_erpl_rev_delta=>register( VALUE #(
$ERPL_REGFIELDS$ ) ).

    DATA(lv_e) = replace( val = lv_err
                          sub = cl_abap_char_utilities=>newline
                          with = ` ` occ = 0 ).
    out->write( |ERPL-CLI/{ lc_n } status={ COND string(
                   WHEN lv_err IS INITIAL THEN `ok` ELSE `error` ) }| ).
    out->write( |ERPL-CLI/{ lc_n } error={ lv_e }| ).
  ENDMETHOD.
ENDCLASS.
)ABAP";

constexpr const char *kSyncRun = R"ABAP(
CLASS $ERPL_CLASS$ DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS $ERPL_CLASS$ IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    CONSTANTS lc_n TYPE string VALUE $ERPL_NONCE$.
    DATA lt TYPE zcl_erpl_rev_delta=>tt_run.

    IF $ERPL_ONE_TARGET$ IS INITIAL.
      lt = zcl_erpl_rev_delta=>run_due( ).
    ELSE.
      APPEND zcl_erpl_rev_delta=>run( $ERPL_ONE_TARGET$ ) TO lt.
    ENDIF.

    out->write( |ERPL-CLI/{ lc_n } count={ lines( lt ) }| ).
    LOOP AT lt INTO DATA(ls).
      DATA(lv_e) = replace( val = ls-error
                            sub = cl_abap_char_utilities=>newline
                            with = ` ` occ = 0 ).
      out->write( |ERPL-CLI/{ lc_n } run={ ls-target }| &&
                  |;method={ ls-method };rows={ ls-rows }| &&
                  |;ins={ ls-ins };upd={ ls-upd };del={ ls-del }| &&
                  |;wm={ ls-wm };skipped={ ls-skipped };error={ lv_e }| ).
    ENDLOOP.
  ENDMETHOD.
ENDCLASS.
)ABAP";

constexpr const char *kSchedule = R"ABAP(
CLASS $ERPL_CLASS$ DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS $ERPL_CLASS$ IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    CONSTANTS lc_n TYPE string VALUE $ERPL_NONCE$.
    DATA lv_jc TYPE tbtcjob-jobcount.
    DATA lv_jn TYPE btcjob.
    lv_jn = |ERPLCLI_{ sy-uzeit }|.

    " Installing a periodic job means BP_JOB_DELETE and JOB_CLOSE, which dump in
    " the HTTP work process a classrun runs in. So this does what the SAP GUI
    " does -- runs report Z_ERPL_REV_DELTA with its scheduling flags -- as a
    " background job, where those calls are allowed.
    CALL FUNCTION 'JOB_OPEN'
      EXPORTING jobname = lv_jn
      IMPORTING jobcount = lv_jc
      EXCEPTIONS OTHERS = 4.
    IF sy-subrc <> 0.
      out->write( |ERPL-CLI/{ lc_n } msg=ERROR: JOB_OPEN subrc={ sy-subrc }| ).
      RETURN.
    ENDIF.

    SUBMIT z_erpl_rev_delta
      WITH p_sched = $ERPL_SCHED$
      WITH p_unsch = $ERPL_UNSCHED$
      WITH p_min   = $ERPL_MINUTES$
      VIA JOB lv_jn NUMBER lv_jc AND RETURN.

    CALL FUNCTION 'JOB_CLOSE'
      EXPORTING jobcount = lv_jc jobname = lv_jn strtimmed = abap_true
      EXCEPTIONS OTHERS = 4.
    IF sy-subrc <> 0.
      out->write( |ERPL-CLI/{ lc_n } msg=ERROR: JOB_CLOSE subrc={ sy-subrc }| ).
      RETURN.
    ENDIF.

    out->write( |ERPL-CLI/{ lc_n } msg=submitted { lv_jn }| ).
  ENDMETHOD.
ENDCLASS.
)ABAP";

constexpr const char *kJobCheck = R"ABAP(
CLASS $ERPL_CLASS$ DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.

CLASS $ERPL_CLASS$ IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    CONSTANTS lc_n TYPE string VALUE $ERPL_NONCE$.
    DATA lv_n TYPE i.
    SELECT COUNT(*) INTO lv_n FROM tbtco
      WHERE jobname = 'ERPL_REV_DELTA' AND status IN ('S','R','Y','P').
    out->write( |ERPL-CLI/{ lc_n } jobs={ lv_n }| ).
  ENDMETHOD.
ENDCLASS.
)ABAP";

// The class name is bound as a bare identifier, not a literal, so it is
// validated here rather than escaped.
void CheckClassName(const std::string &n) {
    if (n.size() > 30 || n.rfind("ZCL_ERPL_REV_CLI_", 0) != 0)
        throw std::logic_error("abapgen: bad generated class name " + n);
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            throw std::logic_error("abapgen: bad generated class name " + n);
}

} // namespace

std::string RenderReplicate(const ReplicateParams &p, const std::string &nonce) {
    const std::string cls = "ZCL_ERPL_REV_CLI_R" + nonce;
    CheckClassName(cls);
    const bool insert = (p.mode == "INSERT" || p.mode == "insert");

    Template t(kReplicate);
    t.Set("CLASS", cls).Set("NONCE", Backtick(nonce, "nonce"))
     .Set("TABLE", Backtick(p.table, "--table"))
     .Set("TARGET", Backtick(p.target, "--target"))
     .Set("TARGET_TXT", TemplateBody(p.target, "--target"))
     .Set("COLUMNS", Backtick(p.columns, "--columns"))
     .Set("WHERE", Backtick(p.where, "--where"))
     .Set("CDSPARAMS", Backtick(p.cds_params, "--cds-params"))
     .Set("INIT", Backtick(p.init, "--init"))
     .Set("UPSERT", Bool(!insert)).Set("INSERT", Bool(insert))
     .Set("BATCH", Int(p.batch)).Set("MAXROWS", Int(p.maxrows))
     .Set("TRUNCATE", Bool(p.truncate)).Set("VERIFY", Bool(p.verify))
     .Set("PARALLEL", Bool(p.parallel))
     .Set("PARTCOL", Backtick(p.part_col, "--part-col"))
     .Set("JOBS", Int(p.jobs))
     .Set("KIND_D", Bool(p.target_kind == "duckdb"))
     .Set("KIND_P", Bool(p.target_kind == "parquet"))
     .Set("KIND_T", Bool(p.target_kind == "table"))
     .Set("DEST", Backtick(p.dest, "--dest"))
     .Set("PARTBY", Backtick(p.partition_by, "--partition-by"));
    return t.Render();
}

std::vector<std::pair<std::string, std::string>> RegisterFields(const SyncState &s) {
    return {
        {"target", Backtick(s.target, "<target>")},
        {"method", Backtick(s.method, "--method")},
        {"source_from", Backtick(s.source_from, "--source")},
        {"keys", Backtick(s.keys, "--keys")},
        {"chg_col", Backtick(s.chg_col, "--chg-col")},
        {"time_col", Backtick(s.time_col, "--time-col")},
        {"wm_kind", Backtick(s.wm_kind, "--wm-kind")},
        {"wm_value", Backtick(s.wm_value, "--wm-value")},
        {"safety_secs", Int(s.safety_secs)},
        {"safety_units", Int(s.safety_units)},
        {"cadence", Backtick(s.cadence, "--cadence")},
        {"extra", Backtick(s.extra, "--extra")},
        {"log_enabled", s.log_enabled ? "abap_true" : "abap_false"},
        {"load_type_default",
         Backtick(s.load_type_default.empty() ? "D" : s.load_type_default, "--load-type-default")},
        {"allow_empty_reload", s.allow_empty_reload ? "abap_true" : "abap_false"},
    };
}

std::string RenderSyncRegister(const SyncState &s, const std::string &nonce) {
    const std::string cls = "ZCL_ERPL_REV_CLI_C" + nonce;
    CheckClassName(cls);
    Template t(kRegister);
    std::string body;
    for (const auto &f : RegisterFields(s)) {
        body += "      " + f.first;
        body += std::string(12 > f.first.size() ? 12 - f.first.size() : 1, ' ');
        body += "= " + f.second + "\n";
    }
    if (!body.empty()) body.pop_back();
    t.Set("CLASS", cls).Set("NONCE", Backtick(nonce, "nonce")).Set("REGFIELDS", body);
    return t.Render();
}

std::string RenderSyncRun(const std::string &target, const std::string &nonce) {
    const std::string cls = "ZCL_ERPL_REV_CLI_S" + nonce;
    CheckClassName(cls);
    Template t(kSyncRun);
    t.Set("CLASS", cls).Set("NONCE", Backtick(nonce, "nonce"))
     .Set("ONE_TARGET", Backtick(target, "<target>"));
    return t.Render();
}

std::string RenderJobCheck(const std::string &nonce) {
    const std::string cls = "ZCL_ERPL_REV_CLI_Q" + nonce;
    CheckClassName(cls);
    Template t(kJobCheck);
    t.Set("CLASS", cls).Set("NONCE", Backtick(nonce, "nonce"));
    return t.Render();
}

std::string RenderSchedule(long long minutes, bool remove, const std::string &nonce) {
    const std::string cls = "ZCL_ERPL_REV_CLI_J" + nonce;
    CheckClassName(cls);
    Template t(kSchedule);
    t.Set("CLASS", cls).Set("NONCE", Backtick(nonce, "nonce"))
     .Set("MINUTES", Int(minutes))
     .Set("SCHED", Bool(!remove)).Set("UNSCHED", Bool(remove));
    return t.Render();
}

// ---------------------------------------------------------------------------
// TempClassrun
// ---------------------------------------------------------------------------

namespace {
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

TempClassrun::TempClassrun(const adt::Conn &conn, const std::string &kind_letter,
                           const std::string &nonce, bool keep)
    : conn_(conn), name_("ZCL_ERPL_REV_CLI_" + kind_letter + nonce), keep_(keep) {}

std::string TempClassrun::DeleteHint() const {
    return "uvx erpl-adt --host " + conn_.host + " --port " + conn_.port +
           " --user " + conn_.user + " --client " + conn_.client +
           " object delete /sap/bc/adt/oo/classes/" + Lower(name_);
}

std::string TempClassrun::Deploy(const std::string &source) {
    auto r = adt::CreateObject(conn_, "CLAS/OC", name_, "$TMP",
                               "erpl-rev CLI (temporary)");
    created_ = true;   // set even on failure: a half-created object still needs removing

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "erpl-rev-cli";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / (Lower(name_) + ".abap");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + file.string();
        out << source;
    }

    auto w = adt::WriteSource(conn_, name_, file.string());
    std::filesystem::remove(file, ec);
    if (!adt::ActivationSucceeded(w)) {
        return "the generated ABAP did not activate. Run the same command with "
               "--print-abap to see it.\n" + w.output.substr(0, 800);
    }
    return {};
}

std::string TempClassrun::Run(std::string &out) {
    auto r = adt::RunClass(conn_, name_);
    out = r.output;
    if (r.spawn_failed) return "erpl-adt could not be started";
    return {};
}

TempClassrun::~TempClassrun() {
    if (!created_) return;
    if (keep_) {
        std::fprintf(stderr, "erpl-rev: kept %s (--keep-generated).\n  Remove it with: %s\n",
                     name_.c_str(), DeleteHint().c_str());
        return;
    }
    auto r = adt::DeleteObject(conn_, "/sap/bc/adt/oo/classes/" + Lower(name_));
    if (!r.ok()) {
        // Never silently leak an object into a customer's system.
        std::fprintf(stderr,
                     "erpl-rev: WARNING could not delete the temporary class %s.\n"
                     "  Remove it with: %s\n",
                     name_.c_str(), DeleteHint().c_str());
    }
}

} // namespace erpl_rev::abapgen
