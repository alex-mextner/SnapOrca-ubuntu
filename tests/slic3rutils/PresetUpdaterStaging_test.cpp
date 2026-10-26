// Staging validation of PresetUpdater: the bundled printers resource folder holds machine images and
// gcode, not presets, and used to be rejected as a vendor profile on every start ("Profile staging
// for bbl is missing preset subdirectories"), so the folder was never installed.
#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "slic3r/Utils/PresetStagingValidation.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct TempDir
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("preset-staging-%%%%-%%%%");
    TempDir() { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

void touch(const fs::path &file)
{
    fs::create_directories(file.parent_path());
    boost::nowide::ofstream(file.string()) << "x";
}

} // namespace

TEST_CASE("printers resource staging needs version.txt, not presets", "[PresetUpdater]")
{
    TempDir tmp;
    const fs::path staging = tmp.path / "printers";
    touch(staging / "BL-P001.json");
    touch(staging / "ams_load.gcode");
    CHECK_FALSE(validate_printers_resource_staging(staging));

    touch(staging / "version.txt");
    CHECK(validate_printers_resource_staging(staging));
}

TEST_CASE("vendor profile staging is still rejected without preset subdirectories", "[PresetUpdater]")
{
    TempDir tmp;
    const fs::path vendors = tmp.path / "system";
    const fs::path staging = vendors / "staging" / "Vendor";
    touch(staging / "BL-P001.json"); // machine files only, like the printers resource folder
    touch(vendors / "Vendor.json");
    CHECK_FALSE(validate_vendor_profile_staging(staging, vendors, "Vendor"));
}

TEST_CASE("vendor profile staging needs the installed vendor json", "[PresetUpdater]")
{
    TempDir tmp;
    const fs::path vendors = tmp.path / "system";
    const fs::path staging = tmp.path / "staging" / "Vendor";
    fs::create_directories(staging / "machine");
    CHECK_FALSE(validate_vendor_profile_staging(staging, vendors, "Vendor"));

    touch(vendors / "Vendor.json");
    CHECK(validate_vendor_profile_staging(staging, vendors, "Vendor"));
}
