// Tests for the shared CLI helpers.
//
// Every test that touches the config or the environment does so inside a
// fixture that redirects HOME/XDG to a temp directory. Without that, these
// tests read -- and WriteConfig would overwrite -- the developer's real
// ~/.config/erpl-rev/config.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>    // _getpid
#define ERPL_GETPID _getpid
#else
#include <unistd.h>
#define ERPL_GETPID getpid
#endif

#include "cli_common.hpp"

using namespace erpl_rev::cli;
using Catch::Matchers::ContainsSubstring;

namespace {

// Sets environment variables for the duration of a test and restores whatever
// was there before, including "was not set at all".
class EnvGuard {
public:
    void Set(const char *k, const std::string &v) {
        const char *old = std::getenv(k);
        saved_.emplace_back(k, old ? std::optional<std::string>(old) : std::nullopt);
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        setenv(k, v.c_str(), 1);
#endif
    }
    void Unset(const char *k) {
        const char *old = std::getenv(k);
        saved_.emplace_back(k, old ? std::optional<std::string>(old) : std::nullopt);
#ifdef _WIN32
        _putenv_s(k, "");
#else
        unsetenv(k);
#endif
    }
    ~EnvGuard() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
#ifdef _WIN32
            _putenv_s(it->first.c_str(), it->second ? it->second->c_str() : "");
#else
            if (it->second) setenv(it->first.c_str(), it->second->c_str(), 1);
            else unsetenv(it->first.c_str());
#endif
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

// A private config/state root, removed afterwards.
class TempHome {
public:
    TempHome() {
        dir_ = std::filesystem::temp_directory_path() /
               ("erpl-rev-test-" + std::to_string(ERPL_GETPID()) + "-" +
                std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        env_.Set("XDG_CONFIG_HOME", dir_.string());
        env_.Set("XDG_RUNTIME_DIR", dir_.string());
        env_.Set("XDG_STATE_HOME", dir_.string());
        env_.Set("APPDATA", dir_.string());
        env_.Set("LOCALAPPDATA", dir_.string());
        env_.Set("HOME", dir_.string());
    }
    ~TempHome() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path &path() const { return dir_; }

private:
    static int counter_;
    std::filesystem::path dir_;
    EnvGuard env_;
};
int TempHome::counter_ = 0;

} // namespace

TEST_CASE("precedence: flag beats env beats config beats default", "[cli]") {
    EnvGuard env;
    const ConfigMap cfg{{"host", "from-config"}};

    SECTION("flag wins over everything") {
        env.Set("SAP_HOST", "from-env");
        CHECK(Pick(cfg, "from-flag", true, "SAP_HOST", "host", "def") == "from-flag");
    }
    SECTION("env wins over config") {
        env.Set("SAP_HOST", "from-env");
        CHECK(Pick(cfg, "", false, "SAP_HOST", "host", "def") == "from-env");
    }
    SECTION("config wins over the default") {
        env.Unset("SAP_HOST");
        CHECK(Pick(cfg, "", false, "SAP_HOST", "host", "def") == "from-config");
    }
    SECTION("the default is the last resort") {
        env.Unset("SAP_HOST");
        CHECK(Pick({}, "", false, "SAP_HOST", "host", "def") == "def");
    }
    SECTION("a flag that was set but left empty does not win") {
        // The subtlety the *_set booleans exist for: `--sap-host ""` must not
        // beat the environment, or an empty value silently blanks the config.
        env.Set("SAP_HOST", "from-env");
        CHECK(Pick(cfg, "", true, "SAP_HOST", "host", "def") == "from-env");
    }
}

TEST_CASE("the config parser handles comments and odd lines", "[cli]") {
    TempHome home;
    const auto p = ConfigPath();
    std::filesystem::create_directories(p.parent_path());
    {
        std::ofstream out(p);
        out << "# a comment\n"
            << "  host = example.com  \n"
            << "no equals sign here\n"
            << "\n"
            << "client = 001 # trailing comment\n"
            << "= value with no key\n";
    }
    const auto cfg = ReadConfig();
    CHECK(cfg.at("host") == "example.com");
    CHECK(cfg.at("client") == "001");
    CHECK(cfg.count("no equals sign here") == 0);
}

TEST_CASE("config round-trips and is not world-readable", "[cli]") {
    TempHome home;
    ConfigMap in{{"host", "h"}, {"user", "u"}};
    REQUIRE(WriteConfig(in));
    const auto out = ReadConfig();
    CHECK(out.at("host") == "h");
    CHECK(out.at("user") == "u");
#ifndef _WIN32
    // It can hold a password, so it must never be readable by anyone else.
    const auto perms = std::filesystem::status(ConfigPath()).permissions();
    CHECK((perms & std::filesystem::perms::group_read) == std::filesystem::perms::none);
    CHECK((perms & std::filesystem::perms::others_read) == std::filesystem::perms::none);
#endif
}

TEST_CASE("LineStartingWith matches only at the start of a line", "[cli]") {
    const std::string s = "alpha=1\nbeta=2\nnot alpha=3";
    CHECK(LineStartingWith(s, "alpha=") == "alpha=1");
    CHECK(LineStartingWith(s, "beta=") == "beta=2");
    // A mid-line occurrence is not a match: reading a decision from anywhere in
    // the output is how an echo of an expected value becomes a false pass.
    CHECK(LineStartingWith("prefix not alpha=3", "alpha=").empty());
    // Last line without a trailing newline still matches.
    CHECK(LineStartingWith("a\nzed=9", "zed=") == "zed=9");
    CHECK(LineStartingWith("", "x").empty());
}

TEST_CASE("the server state file round-trips and hides its token", "[cli]") {
    TempHome home;
    ServerState in;
    in.db_path = "/tmp/x.duckdb";
    in.quack_listen = "quack:localhost:9494";
    in.quack_token = "s3cr3t";
    in.version = "test";
    in.pid = static_cast<long>(ERPL_GETPID());   // alive: ourselves
    REQUIRE(WriteServerState(in));

    ServerState out;
    REQUIRE(ReadServerState(out));
    CHECK(out.db_path == in.db_path);
    CHECK(out.quack_listen == in.quack_listen);
    CHECK(out.quack_token == in.quack_token);
    CHECK(out.pid == in.pid);
#ifndef _WIN32
    const auto perms = std::filesystem::status(ServerStatePath()).permissions();
    CHECK((perms & std::filesystem::perms::others_read) == std::filesystem::perms::none);
#endif
}

TEST_CASE("a state file naming a dead process is treated as absent", "[cli]") {
    TempHome home;
    ServerState in;
    in.quack_listen = "quack:localhost:9494";
    in.quack_token = "t";
    // A pid that cannot be running. Leaving such a file in place would make the
    // CLI dial a listener that is not there.
    in.pid = 0x7FFFFFF0;
    REQUIRE(WriteServerState(in));

    ServerState out;
    CHECK_FALSE(ReadServerState(out));
    // ... and it is cleaned up rather than left to confuse the next run.
    CHECK_FALSE(std::filesystem::exists(ServerStatePath()));
}

TEST_CASE("connection flags are consumed, unknown ones are not", "[cli]") {
    ConnOptions o;
    std::string next;
    auto take = [&]() { return next; };

    next = "sap.example.com";
    CHECK(ParseConnOption("--sap-host", take, o));
    CHECK(o.host == "sap.example.com");
    CHECK(o.host_set);

    CHECK(ParseConnOption("--dry-run", take, o));
    CHECK(o.dry_run);
    CHECK(ParseConnOption("-y", take, o));
    CHECK(o.assume_yes);

    CHECK_FALSE(ParseConnOption("--not-a-flag", take, o));
    // No password flag exists by design: an argument is visible in the process
    // list to every user on the machine.
    CHECK_FALSE(ParseConnOption("--sap-password", take, o));
}
