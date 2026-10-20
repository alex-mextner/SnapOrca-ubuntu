// Internal types and stage interfaces of the repair pipeline. Not part of the ABI.
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "meshfix.h"

namespace meshfix {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double norm(Vec3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

using Tri = std::array<uint32_t, 3>;
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Mesh {
    std::vector<Vec3> v;
    std::vector<Tri> f;
};

struct Cancelled {};

// Pipeline state shared by the stages.
struct Ctx {
    const meshfix_options &opt;
    meshfix_report &rep;
    double tol = 0;  // weld / T-junction tolerance in model units
    double diag = 0; // bounding-box diagonal of the input
    bool on(uint32_t flag) const { return (opt.flags & flag) != 0; }
    void progress(const char *stage, double fraction) const {
        if (opt.progress && opt.progress(opt.progress_user, stage, fraction) != 0)
            throw Cancelled{};
    }
};

// ---- edges.cpp -------------------------------------------------------------------------------

// One directed face edge: corner `id` = 3*face + i runs from f[i] to f[(i+1)%3].
struct EdgeRec {
    uint64_t key; // (min << 32) | max of the two vertex indices
    uint32_t id;
};

// All face edges sorted by (key, id); faces sharing an edge are one run of equal keys.
std::vector<EdgeRec> build_edges(const Mesh &m);

inline uint64_t edge_key(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }
inline uint32_t corner_from(const Mesh &m, uint32_t id) { return m.f[id / 3][id % 3]; }
inline uint32_t corner_to(const Mesh &m, uint32_t id) { return m.f[id / 3][(id % 3 + 1) % 3]; }

struct Topology {
    uint64_t boundary_edges = 0;
    uint64_t non_manifold_edges = 0; // edges with more than two faces
    uint64_t inconsistent_edges = 0; // two faces traversing the edge in the same direction
    uint64_t non_manifold_vertices = 0; // extra fans: vertex copies a split would create
    uint64_t components = 0;
    bool watertight = false;
    bool manifold = false;
};
Topology analyze_topology(const Mesh &m);

// Edge-connected component id per face (faces sharing any edge, also non-manifold ones, are joined).
uint32_t label_components(const Mesh &m, std::vector<uint32_t> &comp);

// Edges shared by exactly two faces of different faces pair up: partner[corner] is the other corner, else kNone.
std::vector<uint32_t> pair_manifold_edges(const Mesh &m, const std::vector<EdgeRec> &recs);
// Gives every disconnected fan of corners (joined through `partner`) its own vertex; returns the number of
// copies that created. fan_classes() only classifies: cls[corner] = fan id, returns the fan count.
uint32_t fan_classes(const Mesh &m, const std::vector<uint32_t> &partner, std::vector<uint32_t> &cls);
uint64_t split_fans(Mesh &m, const std::vector<uint32_t> &partner);

// Makes the winding of every manifold-edge-connected patch consistent (the smaller side of each patch is
// flipped) and, with `outward`, turns patches of negative volume inside out. Returns the faces flipped.
uint64_t orient_consistent(Mesh &m, bool outward);

double signed_volume(const Mesh &m);
// Signed volume of the faces in `faces`, measured from the bbox centre of those faces for accuracy.
double signed_volume_of(const Mesh &m, const std::vector<uint32_t> &faces);

// ---- stages ----------------------------------------------------------------------------------

// Merges vertices (exactly equal ones always; within tol when tol > 0). Faces must already be in range.
// Vertices no face uses are dropped. Returns the number of vertices removed.
uint64_t weld_vertices(Mesh &m, double tol);

uint64_t remove_degenerate_faces(Mesh &m);
uint64_t remove_duplicate_faces(Mesh &m);
uint64_t stitch_t_junctions(Mesh &m, double tol);

// Splits non-manifold edges and vertices into disc-like fans. Needs consistently oriented, outward
// patches. Returns the number of non-manifold edges found; adds the vertex copies to `split`.
uint64_t split_non_manifold(Mesh &m, uint64_t &split);

struct HoleStats {
    uint64_t found = 0, filled = 0, skipped = 0, faces_added = 0;
};
HoleStats fill_holes(Mesh &m, uint32_t max_hole_edges, const Ctx &ctx);

// Orients every shell outward (when `outward`) and, with `nested`, turns shells at odd nesting depth
// into cavities. Drops small / flat shells as requested. Updates the report counters.
void process_components(Mesh &m, const Ctx &ctx);

// Moves the copies of coincident vertices apart (all but the copy with the most faces), `distance` inward along
// the normal of their faces. Returns the number of vertices moved.
uint64_t separate_coincident_vertices(Mesh &m, double distance);

// Drops unreferenced vertices.
void compact_vertices(Mesh &m);

// ---- selfint.cpp -----------------------------------------------------------------------------

// Number of proper triangle piercings (capped at `cap`); faces sharing a vertex are skipped.
uint64_t count_self_intersections(const Mesh &m, double eps, uint64_t cap);

// ---- repair.cpp ------------------------------------------------------------------------------

void analyze_into(const Mesh &input, const Ctx &ctx);
Mesh repair_mesh(const Mesh &input, const meshfix_options &opt, meshfix_report &rep);

} // namespace meshfix
