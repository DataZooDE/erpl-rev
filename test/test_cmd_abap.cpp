// `abap export` exists so the sources can leave the binary by a route other than
// ADT. What matters is that every production object arrives, that the file is the
// source and not a truncated copy, and that the dependency order survives the
// trip -- a directory listing sorts alphabetically and would lose it silently.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "abap_assets.hpp"
#include "commands.hpp"

using namespace erpl_rev;

namespace {

std::filesystem::path TempDir(const std::string &leaf) {
    auto d = std::filesystem::temp_directory_path() / ("erpl-rev-abap-export-" + leaf);
    std::filesystem::remove_all(d);
    return d;
}

std::string Slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

cmd::Options WithArgs(std::vector<std::string> args) {
    cmd::Options o;
    o.args = std::move(args);
    return o;
}

} // namespace

TEST_CASE("abap export writes every production source, byte for byte", "[abap]") {
    const auto dir = TempDir("all");
    REQUIRE(cmd::RunAbap(WithArgs({"export", dir.string()})) == 0);

    for (const auto &a : abap::ProductionAssets()) {
        const auto f = dir / std::string(a.file);
        INFO(a.name);
        REQUIRE(std::filesystem::exists(f));
        // Not "is non-empty": a half-written file is non-empty too, and the
        // failure it causes surfaces as an ABAP syntax error hours later.
        CHECK(Slurp(f) == std::string(a.text));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("the manifest preserves the deployment order", "[abap]") {
    const auto dir = TempDir("order");
    REQUIRE(cmd::RunAbap(WithArgs({"export", dir.string()})) == 0);

    const std::string manifest = Slurp(dir / "MANIFEST.txt");
    std::size_t at = 0;
    for (const auto &a : abap::ProductionAssets()) {
        const auto pos = manifest.find(std::string(a.name), at);
        INFO(a.name << " must appear after the object it depends on");
        REQUIRE(pos != std::string::npos);
        at = pos;
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("abap export reports misuse instead of writing something", "[abap]") {
    CHECK(cmd::RunAbap(WithArgs({})) == 2);
    CHECK(cmd::RunAbap(WithArgs({"import", "/tmp"})) == 2);
    CHECK(cmd::RunAbap(WithArgs({"export"})) == 2);
}
