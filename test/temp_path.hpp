// A portable temp path for tests that need a real file on disk.
//
// Under the OS temp directory, with forward slashes: DuckDB accepts '/' on every
// platform, and it sidesteps backslash escaping inside SQL literals. std::remove
// accepts it too.
//
// This exists because "/tmp/... + ::getpid()" appeared in three test files, which
// is two non-portable things at once: MSVC has no <unistd.h>, so the Windows
// build stopped at the include, and Windows has no /tmp either, so the tests
// would have failed at runtime had it compiled.
//
// test_cli_common.cpp shows the pid done properly, behind an _WIN32 guard on
// <process.h>/_getpid. The uniquifier here is a per-process random value instead,
// because a temp file only needs to be unique -- not to be a pid -- and that is
// one fewer #ifdef to get wrong.
#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace erpl_rev_test {

inline std::string TmpPath(const std::string &name) {
    return (std::filesystem::temp_directory_path() / name).generic_string();
}

// A path unique to this process and this call, with any leftovers from a
// previous run removed -- including DuckDB's sidecar write-ahead log, which is
// what makes a "fresh" database quietly resume an old one.
inline std::string TmpDbPath(const std::string &stem) {
    static const unsigned salt = std::random_device{}();
    static int n = 0;
    const std::string p =
        TmpPath("erpl_rev_" + stem + "_" + std::to_string(salt) + "_" + std::to_string(++n) +
                ".duckdb");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p + ".wal", ec);
    return p;
}

}  // namespace erpl_rev_test
