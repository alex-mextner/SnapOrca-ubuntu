# meshfix (vendored)

Snapshot of <https://github.com/alex-mextner/meshfix> (MIT, see LICENSE.txt), the mesh repair library behind
"Fix model": a C++17 library with a C ABI (`include/meshfix.h`) and no dependencies.

- Upstream tag: **v0.1.1** (commit recorded in `VERSION`)
- Copied unchanged: `include/meshfix.h`, `src/*`, `LICENSE.txt`. `CMakeLists.txt` and this file are local.
- Update by copying the same files from a newer upstream tag and bumping `VERSION`; never edit them here.
- Used by `src/libslic3r/MeshBoolean.cpp` (`MeshBoolean::cgal::repair`) through the C ABI only.
