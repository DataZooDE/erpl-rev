// `erpl-rev abap export <dir>` -- write the embedded ABAP sources to disk.
//
// The binary already carries every production ABAP object (abap_assets.hpp), and
// `setup` pushes them over ADT. That covers the system where the CLI may write to
// the repository. It covers nothing else: a customer whose delivery route is
// abapGit, a developer who wants to read the source before it reaches their
// system, an SAP Basis team that will import the objects by hand, or anyone
// building a transport on a machine the CLI cannot reach.
//
// So: the same fourteen sources, in the same dependency order, as files.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

#include "abap_assets.hpp"
#include "commands.hpp"

namespace erpl_rev::cmd {

int RunAbap(Options o) {
    const std::string sub = o.args.empty() ? "" : o.args[0];
    if (sub != "export") {
        std::fprintf(stderr,
                     "erpl-rev abap: expected a subcommand.\n"
                     "  abap export <dir>   Write the embedded ABAP sources to <dir>.\n");
        return 2;
    }
    if (o.args.size() < 2) {
        std::fprintf(stderr, "erpl-rev abap export: needs a target directory.\n");
        return 2;
    }

    const std::filesystem::path dir = o.args[1];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "erpl-rev abap export: cannot create %s: %s\n",
                     dir.string().c_str(), ec.message().c_str());
        return 1;
    }

    // The deployment order is part of the delivery, not an implementation
    // detail: an interface has to exist before the class whose signature names
    // it. A plain directory listing sorts alphabetically and loses it, so the
    // order is written down where whoever imports these will look for it.
    std::ofstream manifest(dir / "MANIFEST.txt");
    if (!manifest) {
        std::fprintf(stderr, "erpl-rev abap export: cannot write into %s\n",
                     dir.string().c_str());
        return 1;
    }
    manifest << "# erpl-rev ABAP sources, in deployment order.\n"
                "# Import them top to bottom: each object may reference the ones above it.\n"
                "# name\tADT type\tfile\n";

    int written = 0;
    for (const auto &a : abap::ProductionAssets()) {
        const auto file = dir / std::string(a.file);
        std::ofstream out(file, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "erpl-rev abap export: cannot write %s\n",
                         file.string().c_str());
            return 1;
        }
        out << a.text;
        out.close();
        manifest << a.name << "\t" << a.adt_type << "\t" << a.file << "\n";
        written++;
    }
    manifest.close();

    std::cout << "Wrote " << written << " ABAP sources to " << dir.string()
              << " (see MANIFEST.txt for the import order).\n"
                 "The function group ZERPL_REV and its nine Z_DUCKDB_* modules are not\n"
                 "source files: ZCL_ERPL_REV_MKFM generates them once it is active.\n";
    return 0;
}

} // namespace erpl_rev::cmd
