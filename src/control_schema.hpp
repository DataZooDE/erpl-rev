// Control-schema versioning and forward migration.
//
// Every control table erpl-rev owns is created and evolved from ONE ordered
// list. The rules that make it safe:
//
//   * v1 is the DDL the constructor used at commit 1a97fbb, byte for byte. Every
//     statement in it is CREATE ... IF NOT EXISTS, so a file already in the field
//     is treated as version 0, has v1 applied as a no-op, and then gets v2+.
//     That is what makes the upgrade a migration rather than an export/import --
//     and it is why v1 must never be "tidied up".
//   * Every later migration is idempotent (AddColumnIfMissing, UPDATE ... WHERE),
//     so re-running the whole list changes nothing.
//   * The version table is a HISTORY, not a single row. After an upgrade
//     incident the question asked is "which binary applied v7, and when".
//   * A file newer than the binary is REFUSED. Silently running an old binary
//     against a file whose columns it does not know fails much later, somewhere
//     unrelated, and looks like data corruption.
//
// Kept deliberately free of DuckDbBridge so the tests can drive it with a bare
// duckdb::Connection.
#pragma once

#include <duckdb.hpp>

#include <string>
#include <vector>

namespace erpl_rev {
namespace schema {

struct Migration {
    int version;
    const char *name;
    std::vector<std::string> sql;
};

// The ordered list. Index N is version N+1.
const std::vector<Migration> &Migrations();

// Highest version this binary knows.
int LatestVersion();

// 0 when _erpl_rev_schema_version does not exist yet (a pre-versioning file).
int CurrentVersion(duckdb::Connection &con);

// Apply everything newer than the file's version, each migration and its version
// row in one transaction. Throws std::runtime_error if the file is newer than
// this binary, or if a statement fails.
void Migrate(duckdb::Connection &con, const std::string &binary_version);

// ALTER TABLE ... ADD COLUMN, but only when the column is absent. Migrations use
// this instead of a bare ADD COLUMN so the list can be re-run.
void AddColumnIfMissing(duckdb::Connection &con, const std::string &table,
                        const std::string &column, const std::string &type);

}  // namespace schema
}  // namespace erpl_rev
