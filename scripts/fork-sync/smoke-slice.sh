#!/usr/bin/env bash
# CLI smoke slice of a built AppImage: slices tests/data/20mm_cube.obj with the Snapmaker U1 profiles and
# checks the G-code, then slices the project tests/data/test_3mf/mixed_named_plate.3mf (named plate, mixed
# filament). Run from the repo root inside the build container or on the build server
# (release.sh does), never on the laptop: it starts the slicer binary (AGENTS.md host rules).
#
#   scripts/fork-sync/smoke-slice.sh APPIMAGE
#
# Exit status: 0 valid G-code, 13 the slicer crashed or produced no valid G-code (the work directory with
# the log is kept and named), 2 usage.
set -euo pipefail

[[ $# -eq 1 && -x $1 ]] || { echo "usage: $0 APPIMAGE" >&2; exit 2; }
appimage=$(readlink -f "$1")
# Not git: the build server's tree is an rsync copy without .git.
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
cd "$ROOT"

smoke=$(mktemp -d)
scripts/flatten_profile.py -o "$smoke/profiles" \
    --machine "Snapmaker U1 (0.4 nozzle)" \
    --process "0.20mm Standard @Snapmaker U1 (0.4 nozzle)" \
    --filament "Snapmaker PLA Basic @U1" >/dev/null
"$appimage" --appimage-extract-and-run --datadir "$smoke/data" --slice 0 --outputdir "$smoke/out" \
    --load-settings "$smoke/profiles/machine-Snapmaker U1 (0.4 nozzle).json;$smoke/profiles/process-0.20mm Standard @Snapmaker U1 (0.4 nozzle).json" \
    --load-filaments "$smoke/profiles/filament-Snapmaker PLA Basic @U1.json" \
    tests/data/20mm_cube.obj >"$smoke/slice.log" 2>&1 || { echo "smoke slice crashed, see $smoke/slice.log" >&2; exit 13; }
python3 - "$smoke/out" <<'EOF' || { echo "smoke slice produced no valid G-code, see $smoke" >&2; exit 13; }
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
result = json.loads((out / "result.json").read_text())
gcode = (out / "plate_1.gcode").read_text(errors="replace")
assert result["return_code"] == 0, result
assert gcode.count(";LAYER_CHANGE") > 10 and "printer_model = Snapmaker U1" in gcode
EOF

# A project 3mf with a named plate and a mixed (virtual) filament: the two things that made the command line
# slicer dereference GUI state that does not exist there (issue #21). The project carries its own settings and was
# saved by the 2.4.0 GUI, whose version the command line slicer (01.10.01.50) refuses without --allow-newer-file.
"$appimage" --appimage-extract-and-run --datadir "$smoke/project-data" --allow-newer-file --slice 0 --outputdir "$smoke/project-out" \
    tests/data/test_3mf/mixed_named_plate.3mf >"$smoke/project-slice.log" 2>&1 || { echo "project smoke slice crashed, see $smoke/project-slice.log" >&2; exit 13; }
python3 - "$smoke/project-out" <<'EOF' || { echo "project smoke slice produced no valid G-code, see $smoke" >&2; exit 13; }
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
result = json.loads((out / "result.json").read_text())
gcode = (out / "plate_1.gcode").read_text(errors="replace")
assert result["return_code"] == 0, result
assert gcode.count(";LAYER_CHANGE") > 10, "too few layers"
EOF
rm -rf "$smoke"
echo "smoke slice OK"
