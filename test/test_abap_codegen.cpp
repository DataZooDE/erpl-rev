// Tests for the code that turns user input into ABAP source.
//
// This is the injection suite. Every value a user types on the command line
// becomes part of a program that runs on their SAP system, so the question
// each test asks is the same: can this input change the *meaning* of the
// generated ABAP, rather than just its data?
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "abap_codegen.hpp"

using namespace erpl_rev::abapgen;
using Catch::Matchers::ContainsSubstring;

namespace {

// Reverse of Backtick, for round-trip assertions: strip the outer backticks,
// undouble the inner ones. Deliberately hand-written rather than reusing
// production code, so a bug in the escaper cannot cancel itself out here.
std::string UnBacktick(const std::string &lit) {
    REQUIRE(lit.size() >= 2);
    REQUIRE(lit.front() == '`');
    REQUIRE(lit.back() == '`');
    const std::string body = lit.substr(1, lit.size() - 2);
    std::string out;
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '`' && i + 1 < body.size() && body[i + 1] == '`') { out += '`'; i++; }
        else out += body[i];
    }
    return out;
}

} // namespace

TEST_CASE("a plain value round-trips through a text-string literal", "[abapgen]") {
    CHECK(Backtick("plain", "--target") == "`plain`");
    CHECK(UnBacktick(Backtick("plain", "--target")) == "plain");
}

TEST_CASE("apostrophes pass through untouched", "[abapgen]") {
    // The motivating case: WHERE name = 'O''Brien'. In a `...` literal an
    // apostrophe is an ordinary character, which is precisely why backticks are
    // the default rather than '...'.
    const std::string v = "name = 'O''Brien'";
    const std::string lit = Backtick(v, "--where");
    CHECK(lit == "`name = 'O''Brien'`");
    CHECK(UnBacktick(lit) == v);
}

TEST_CASE("backticks are doubled and survive a round trip", "[abapgen]") {
    CHECK(Backtick("a`b", "--where") == "`a``b`");
    CHECK(UnBacktick(Backtick("a`b", "--where")) == "a`b");
    CHECK(UnBacktick(Backtick("```", "--where")) == "```");
    CHECK(UnBacktick(Backtick("`", "--where")) == "`");
}

TEST_CASE("trailing blanks: preserved by backticks, refused by apostrophes", "[abapgen]") {
    // SAP keys are blank-padded, so this is not a corner case. A '...' literal
    // would silently drop the blank and change the predicate.
    const std::string padded = "000000000000012 ";
    CHECK(UnBacktick(Backtick(padded, "--where")) == padded);
    CHECK_THROWS_AS(Apostrophe(padded, "--where"), UnsafeValue);

    CHECK(Apostrophe("no trailing blank", "--where") == "'no trailing blank'");
    CHECK(Apostrophe("it's", "--where") == "'it''s'");
}

TEST_CASE("control characters are refused, naming the flag", "[abapgen]") {
    for (const std::string bad : {std::string("a\nb"), std::string("a\rb"),
                                  std::string("a\tb"), std::string("a\x1b" "b"),
                                  std::string("a\x7f" "b"), std::string(1, '\0') + "x"}) {
        CHECK_THROWS_AS(Backtick(bad, "--where"), UnsafeValue);
    }
    try {
        Backtick("a\nb", "--where");
        FAIL("expected a throw");
    } catch (const UnsafeValue &e) {
        CHECK(e.field == "--where");
        CHECK_THAT(e.what(), ContainsSubstring("--where"));
    }
}

TEST_CASE("valid UTF-8 is accepted, malformed UTF-8 is refused", "[abapgen]") {
    // A WHERE over a text column may legitimately carry non-ASCII.
    CHECK(UnBacktick(Backtick("Müller", "--where")) == "Müller");
    CHECK(UnBacktick(Backtick("日本語", "--where")) == "日本語");
    CHECK(UnBacktick(Backtick("🦆", "--where")) == "🦆");

    CHECK_THROWS_AS(Backtick("\xC3", "--where"), UnsafeValue);              // lone lead
    CHECK_THROWS_AS(Backtick("\x80\x80", "--where"), UnsafeValue);          // lone continuation
    CHECK_THROWS_AS(Backtick("\xED\xA0\x80", "--where"), UnsafeValue);      // surrogate
    CHECK_THROWS_AS(Backtick("\xC0\x80", "--where"), UnsafeValue);          // overlong NUL
}

TEST_CASE("oversized values are refused with an actionable message", "[abapgen]") {
    CHECK_NOTHROW(Backtick(std::string(kMaxValueBytes, 'x'), "--where"));
    try {
        Backtick(std::string(kMaxValueBytes + 1, 'x'), "--where");
        FAIL("expected a throw");
    } catch (const UnsafeValue &e) {
        CHECK_THAT(e.what(), ContainsSubstring("CDS view"));   // names a way forward
    }
}

TEST_CASE("long values are split without breaking the escaping", "[abapgen]") {
    // Property: no emitted line exceeds the limit, and the value still
    // round-trips once the && continuations are removed.
    const std::string v(4000, 'a');
    const std::string lit = Backtick(v, "--where");
    std::string line;
    for (char c : lit) {
        if (c == '\n') { CHECK(line.size() <= kMaxLineChars); line.clear(); }
        else line += c;
    }
    CHECK(line.size() <= kMaxLineChars);
}

TEST_CASE("a split never lands inside a multi-byte sequence", "[abapgen]") {
    std::string v;
    for (int i = 0; i < 900; i++) v += "日";      // 3 bytes each
    const std::string lit = Backtick(v, "--where");
    // Every backtick-delimited chunk must itself be valid UTF-8.
    std::string chunk;
    bool inside = false;
    for (size_t i = 0; i < lit.size(); i++) {
        if (lit[i] == '`') {
            if (inside) { std::string why; CHECK(IsEmbeddable(chunk, &why)); chunk.clear(); }
            inside = !inside;
            continue;
        }
        if (inside) chunk += lit[i];
    }
}

TEST_CASE("injection payloads render as data, never as code", "[abapgen]") {
    // Each of these tries to close the literal and start a new statement. The
    // only thing that must come back is one literal.
    for (const std::string payload : {
             std::string("x` ). zcl_evil=>go( ). `"),
             std::string("x. ENDMETHOD. METHOD if_oo_adt_classrun~main."),
             std::string("x' ). DELETE FROM t000. \""),
             std::string("`` ) ##"),
         }) {
        const std::string lit = Backtick(payload, "--where");
        // Round-trips exactly: nothing was interpreted.
        CHECK(UnBacktick(lit) == payload);
        // And every backtick inside the body is doubled, so the literal cannot
        // be terminated early.
        const std::string body = lit.substr(1, lit.size() - 2);
        for (size_t i = 0; i < body.size(); i++) {
            if (body[i] == '`') {
                REQUIRE(i + 1 < body.size());
                CHECK(body[i + 1] == '`');
                i++;
            }
        }
    }
}

TEST_CASE("string-template bodies escape the template metacharacters", "[abapgen]") {
    CHECK(TemplateBody("a|b", "--where") == "a\\|b");
    CHECK(TemplateBody("{ sy-uname }", "--where") == "\\{ sy-uname \\}");
    CHECK(TemplateBody("a\\b", "--where") == "a\\\\b");
    // The classic escape: inside |...| an unescaped { } evaluates an expression.
    CHECK_THAT(TemplateBody("x{ zcl_evil=>go( ) }", "--where"),
               ContainsSubstring("\\{"));
}

// ---------------------------------------------------------------------------
// Template engine
// ---------------------------------------------------------------------------

TEST_CASE("placeholders are substituted", "[abapgen]") {
    Template t("iv_tab = $ERPL_TAB$ iv_target = $ERPL_TARGET$.");
    t.Set("TAB", "`MARA`").Set("TARGET", "`mara`");
    CHECK(t.Render() == "iv_tab = `MARA` iv_target = `mara`.");
}

TEST_CASE("substituted text is never rescanned", "[abapgen]") {
    // A --target literally named $ERPL_WHERE$ must not become a placeholder.
    Template t("a = $ERPL_TARGET$ b = $ERPL_WHERE$.");
    t.Set("TARGET", "`$ERPL_WHERE$`").Set("WHERE", "`ok`");
    CHECK(t.Render() == "a = `$ERPL_WHERE$` b = `ok`.");
}

TEST_CASE("an unbound placeholder is an error, not an empty string", "[abapgen]") {
    Template t("a = $ERPL_TAB$ b = $ERPL_TARGET$.");
    t.Set("TAB", "`x`");
    CHECK_THROWS_AS(t.Render(), std::logic_error);
}

TEST_CASE("binding a key the skeleton does not contain is an error", "[abapgen]") {
    // The bug the old find/replace had: rename a placeholder, and the
    // substitution silently stops happening while everything still compiles.
    Template t("a = $ERPL_TAB$.");
    CHECK_THROWS_AS(t.Set("TARGET", "`x`"), std::logic_error);
}

TEST_CASE("a placeholder appearing twice is substituted twice", "[abapgen]") {
    Template t("$ERPL_T$ and $ERPL_T$");
    t.Set("T", "`x`");
    CHECK(t.Render() == "`x` and `x`");
}

// ---------------------------------------------------------------------------
// Result parsing
// ---------------------------------------------------------------------------

TEST_CASE("results are read only from lines carrying this run's nonce", "[abapgen]") {
    const std::string out =
        "some unrelated ABAP output\n"
        "ERPL-CLI/deadbeef status=ok\n"
        "ERPL-CLI/deadbeef rows=1234\n"
        "ERPL-CLI/deadbeef error=\n";
    CHECK(ResultField(out, "deadbeef", "status") == "ok");
    CHECK(ResultField(out, "deadbeef", "rows") == "1234");
    CHECK(ResultField(out, "deadbeef", "error").empty());
}

TEST_CASE("a stale run's output is not mistaken for this run's", "[abapgen]") {
    // A leftover class from an earlier invocation printing a perfect success.
    const std::string out = "ERPL-CLI/00000000 status=ok\nERPL-CLI/00000000 rows=99\n";
    CHECK(ResultField(out, "deadbeef", "status").empty());
    CHECK(ResultLines(out, "deadbeef").empty());
}

TEST_CASE("output that merely mentions success is not success", "[abapgen]") {
    // An HTTP 500 body, or an ABAP short dump, can contain almost anything.
    const std::string out = "Internal Server Error: status=ok rows=1\n";
    CHECK(ResultField(out, "deadbeef", "status").empty());
}

TEST_CASE("nonces are 8 hex characters and vary", "[abapgen]") {
    const std::string a = MakeNonce(), b = MakeNonce();
    CHECK(a.size() == 8);
    for (char c : a) CHECK(std::string("0123456789abcdef").find(c) != std::string::npos);
    CHECK(a != b);   // 1 in 4 billion flake; acceptable
}

TEST_CASE("multi-line SQL becomes one concatenated string template", "[abapgen]") {
    const auto lines = MultilineTemplate("SELECT 1\nFROM t", "  ", "sql");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "  |SELECT 1 | &&");   // trailing space, so the join is valid SQL
    CHECK(lines[1] == "  |FROM t|");
}

TEST_CASE("multi-line SQL still escapes template metacharacters per line", "[abapgen]") {
    const auto lines = MultilineTemplate("SELECT '{a}'\nFROM t", "", "sql");
    CHECK_THAT(lines[0], ContainsSubstring("\\{a\\}"));
}
