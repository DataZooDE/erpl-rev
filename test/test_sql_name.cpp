// Identifier sanitizing, in one place.
//
// The CDC dialect grew a private NameToken() that folds every non-alphanumeric
// character to '_'. That is fine as a *name* rule and it must not change --
// existing installs have ZCDC_* objects named by it, and renaming the rule
// orphans them. But it is not injective, so it cannot be reused as-is for a
// second namespace whose inputs are customer-chosen target names.

#include <catch2/catch_test_macros.hpp>
#include "sql_name.hpp"

using namespace erpl_rev::sqlname;

TEST_CASE("sqlname: Token keeps the established ZCDC_ naming rule", "[sqlname]") {
    // Byte-for-byte what cdc_dialect.cpp's NameToken produced. Locked down so a
    // future tidy-up cannot silently orphan a customer's provisioned objects.
    CHECK(Token("SFLIGHT") == "SFLIGHT");
    CHECK(Token("sflight") == "SFLIGHT");
    CHECK(Token("/BIC/FOO") == "_BIC_FOO");
    CHECK(Token("ZDELTA_WM") == "ZDELTA_WM");
}

TEST_CASE("sqlname: Token is not injective, which is why it is not the whole story", "[sqlname]") {
    CHECK(Token("MY-TAB") == Token("MY_TAB"));
}

TEST_CASE("sqlname: UniqueToken separates names Token would collide", "[sqlname]") {
    // Two distinct registered targets must never land on one change-log table.
    CHECK(UniqueToken("MY-TAB") != UniqueToken("MY_TAB"));
    CHECK(UniqueToken("/BIC/FOO") != UniqueToken("_BIC_FOO"));

    // A name that is already a clean identifier is left alone, so the common
    // case stays readable and stable.
    CHECK(UniqueToken("SFLIGHT") == "SFLIGHT");
    CHECK(UniqueToken("ZDELTA_WM") == "ZDELTA_WM");

    // Case folding alone still collapses -- that is intended (SAP names are
    // case-insensitive), so it must NOT earn a suffix.
    CHECK(UniqueToken("sflight") == "SFLIGHT");

    // Deterministic across calls: the table name has to survive a restart.
    CHECK(UniqueToken("MY-TAB") == UniqueToken("MY-TAB"));
}

TEST_CASE("sqlname: QuoteIdent escapes an embedded double quote", "[sqlname]") {
    CHECK(QuoteIdent("PLAIN") == "\"PLAIN\"");
    // The existing Quote() in cdc_dialect.cpp concatenated blindly; an
    // identifier carrying a quote would have ended the quoted string early.
    CHECK(QuoteIdent("A\"B") == "\"A\"\"B\"");
}
