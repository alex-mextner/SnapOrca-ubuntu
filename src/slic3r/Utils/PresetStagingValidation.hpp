#ifndef slic3r_PresetStagingValidation_hpp_
#define slic3r_PresetStagingValidation_hpp_

#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/format.hpp"

// Checks PresetUpdater runs on a staging copy of a directory before it replaces the installed one.
// Header-only so that the tests can use them without linking the GUI.
namespace Slic3r {

// A vendor profile bundle needs preset subdirectories and an installed <vendor>.json in installed_vendors_dir.
inline bool validate_vendor_profile_staging(const boost::filesystem::path &staging,
                                            const boost::filesystem::path &installed_vendors_dir,
                                            const std::string             &vendor)
{
    namespace fs = boost::filesystem;
    const bool has_preset_tree = fs::exists(staging / PRESET_PRINTER_NAME) || fs::exists(staging / PRESET_FILAMENT_NAME) ||
                                 fs::exists(staging / PRESET_PRINT_NAME);
    if (!has_preset_tree) {
        BOOST_LOG_TRIVIAL(error) << Slic3r::format("Profile staging for %1% is missing preset subdirectories", vendor);
        return false;
    }

    if (!fs::exists(installed_vendors_dir / (vendor + ".json"))) {
        BOOST_LOG_TRIVIAL(error) << Slic3r::format("Profile staging: %1%.json is not installed yet", vendor);
        return false;
    }
    return true;
}

// The printers resource folder (machine images, gcode) is versioned by version.txt and has no presets.
inline bool validate_printers_resource_staging(const boost::filesystem::path &staging)
{
    if (!boost::filesystem::exists(staging / "version.txt")) {
        BOOST_LOG_TRIVIAL(error) << "Printers resource staging is missing version.txt";
        return false;
    }
    return true;
}

} // namespace Slic3r

#endif // slic3r_PresetStagingValidation_hpp_
