/**
 * @file  Tests that PartPlate works in the command line slicer, where no wxApp, GUI_App or Plater exists.
 *
 * Accessed via: ctest, target slic3rutils_tests.
 *
 * Assumptions: the process is a plain Catch2 executable, so wxTheApp is null exactly as in `snapmaker-orca --slice`;
 *              any dereference of wxGetApp() or of the null Plater crashes the test binary.
 *
 * Past bugs: `snapmaker-orca --slice 0 project.3mf` segfaulted in PartPlate::generate_plate_name_texture() for a
 *            plate that carries a name, and in expand_plate_extruders() for a project that uses mixed filaments
 *            (https://github.com/alex-mextner/SnapOrca-ubuntu/issues/21).
 */
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/PartPlate.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using Slic3r::GUI::PartPlate;

namespace {

// Same construction as the command line slicer: a PartPlateList without a Plater.
struct CliPlates
{
    Model         model;
    GUI::PartPlateList plates{nullptr, &model, ptFFF};

    PartPlate &first_plate() { return *plates.get_plate(0); }
};

// A plate holding one object whose single volume prints with `extruder`.
void put_cube_on_first_plate(CliPlates &cli, int extruder)
{
    ModelObject *object = cli.model.add_object();
    object->name        = "cube";
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));
    volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    object->add_instance();
    REQUIRE(cli.first_plate().add_instance(0, 0, false) == 0);
}

} // namespace

TEST_CASE("naming a plate does not need a Plater", "[PartPlate][cli]")
{
    CliPlates cli;
    cli.first_plate().set_plate_name("Chekhov plate");
    CHECK(cli.first_plate().get_plate_name() == "Chekhov plate");
}

TEST_CASE("plate extruders without mixed filaments are the extruders the volumes use", "[PartPlate][cli]")
{
    CliPlates cli;
    put_cube_on_first_plate(cli, 2);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#1B5E20", "#080A0D", "#E72F1D", "#FFFFFF"};

    CHECK(cli.first_plate().get_extruders_under_cli(false, config) == std::vector<int>{2});
}

TEST_CASE("a mixed filament on a plate expands to its physical filaments without a GUI", "[PartPlate][cli]")
{
    const std::vector<std::string> colours = {"#1B5E20", "#080A0D", "#E72F1D", "#FFFFFF"};

    // The first mixed filament (physical 1 + 2) takes the id after the four physical filaments.
    MixedFilamentManager definitions;
    definitions.add_custom_filament(1, 2, 50, colours);
    const int virtual_extruder = int(colours.size()) + 1;

    CliPlates cli;
    put_cube_on_first_plate(cli, virtual_extruder);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_colour", true)->values = colours;
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions.serialize_custom_entries()));

    CHECK(cli.first_plate().get_extruders_under_cli(false, config) == std::vector<int>{1, 2});
}
