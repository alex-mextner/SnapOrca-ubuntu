// The repair pipeline: stage order, counters, report. SPDX-License-Identifier: MIT
#include <algorithm>
#include <chrono>
#include <cmath>

#include "core.hpp"

namespace meshfix {

namespace {

constexpr uint32_t kRepairFlags = MESHFIX_FIX_ALL;

double auto_tolerance(double diag) { return std::min(diag * 1e-5, 1e-3); }

Mesh sanitize(const Mesh &input, meshfix_report &rep) {
    Mesh m;
    m.v = input.v;
    m.f.reserve(input.f.size());
    const size_t nv = input.v.size();
    for (const Tri &t : input.f) {
        bool ok = true;
        for (uint32_t x : t)
            ok = ok && x < nv && finite(input.v[x]);
        if (ok)
            m.f.push_back(t);
        else
            ++rep.invalid_faces_removed;
    }
    return m;
}

double bbox_diagonal(const Mesh &m) {
    if (m.f.empty())
        return 0;
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (const Tri &t : m.f)
        for (uint32_t x : t) {
            const Vec3 &p = m.v[x];
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    return norm(hi - lo);
}

void fill_before(meshfix_report &rep, const Topology &t, double volume) {
    rep.watertight_before = t.watertight;
    rep.manifold_before = t.manifold;
    rep.non_manifold_edges_before = t.non_manifold_edges;
    rep.boundary_edges_before = t.boundary_edges;
    rep.components_before = t.components;
    rep.volume_before = volume;
}

} // namespace

void analyze_into(const Mesh &input, const Ctx &ctx) {
    meshfix_report &rep = ctx.rep;
    rep.input_vertices = input.v.size();
    rep.input_faces = input.f.size();
    Mesh m = sanitize(input, rep);
    const double volume = signed_volume(m);
    weld_vertices(m, 0);
    const Topology t = analyze_topology(m);
    fill_before(rep, t, volume);
    // Nothing is changed: the "after" side mirrors the input.
    rep.watertight_after = t.watertight;
    rep.manifold_after = t.manifold;
    rep.non_manifold_edges_after = t.non_manifold_edges;
    rep.boundary_edges_after = t.boundary_edges;
    rep.components_after = t.components;
    rep.volume_after = volume;
    rep.output_vertices = input.v.size();
    rep.output_faces = m.f.size();
}

Mesh repair_mesh(const Mesh &input, const meshfix_options &opt, meshfix_report &rep) {
    const auto t0 = std::chrono::steady_clock::now();
    Ctx ctx{opt, rep};
    rep.input_vertices = input.v.size();
    rep.input_faces = input.f.size();

    ctx.progress("sanitize", 0.0);
    Mesh m = sanitize(input, rep);
    ctx.diag = bbox_diagonal(m);
    ctx.tol = opt.weld_tolerance < 0 ? auto_tolerance(ctx.diag) : opt.weld_tolerance;
    rep.weld_tolerance_used = ctx.tol;

    // The state of the input, judged with exactly equal vertices merged (an STL soup is not "open").
    ctx.progress("analyze", 0.05);
    const double volume_before = signed_volume(m);
    Mesh exact = m;
    const uint64_t exact_merged = weld_vertices(exact, 0);
    fill_before(rep, analyze_topology(exact), volume_before);

    const double separation = opt.separation >= 0 ? opt.separation : 2e-5 * ctx.diag;
    const bool any = (opt.flags & kRepairFlags) != 0;
    if (any) {
        if (ctx.on(MESHFIX_FIX_WELD)) {
            ctx.progress("weld", 0.15);
            m = std::move(exact);
            rep.vertices_merged = exact_merged;
            if (ctx.tol > 0)
                rep.vertices_merged += weld_vertices(m, ctx.tol);
        }
        exact = Mesh{};
        if (ctx.on(MESHFIX_FIX_DEGENERATE)) {
            ctx.progress("degenerate", 0.25);
            rep.degenerate_faces_removed = remove_degenerate_faces(m);
        }
        if (ctx.on(MESHFIX_FIX_DUPLICATE_FACES)) {
            ctx.progress("duplicates", 0.30);
            rep.duplicate_faces_removed = remove_duplicate_faces(m);
        }
        if (ctx.on(MESHFIX_FIX_T_JUNCTIONS)) {
            ctx.progress("t-junctions", 0.35);
            rep.t_junctions_fixed = stitch_t_junctions(m, ctx.tol > 0 ? ctx.tol : 1e-12 * ctx.diag);
        }
        if (ctx.on(MESHFIX_FIX_ORIENTATION)) {
            ctx.progress("orientation", 0.40);
            rep.faces_flipped += orient_consistent(m, true);
        }
        if (ctx.on(MESHFIX_FIX_NON_MANIFOLD)) {
            ctx.progress("non-manifold", 0.50);
            split_non_manifold(m, rep.non_manifold_vertices_split);
            if (ctx.on(MESHFIX_FIX_ORIENTATION))
                rep.faces_flipped += orient_consistent(m, true);
        }
        if (ctx.on(MESHFIX_FIX_HOLES)) {
            ctx.progress("holes", 0.60);
            const HoleStats h = fill_holes(m, opt.max_hole_edges, ctx);
            rep.holes_found = h.found;
            rep.holes_filled = h.filled;
            rep.holes_skipped = h.skipped;
            rep.hole_faces_added = h.faces_added;
        }
        if (ctx.on(MESHFIX_FIX_COMPONENTS | MESHFIX_FIX_ORIENTATION)) {
            ctx.progress("components", 0.75);
            process_components(m, ctx);
        }
        if (ctx.on(MESHFIX_FIX_SEPARATE)) {
            ctx.progress("separate", 0.80);
            rep.vertices_separated = separate_coincident_vertices(m, separation);
        }
        compact_vertices(m);
    }

    ctx.progress("verify", 0.85);
    Mesh view = m;
    weld_vertices(view, 0);
    const Topology after = analyze_topology(view);
    view = Mesh{};
    rep.watertight_after = after.watertight;
    rep.manifold_after = after.manifold;
    rep.boundary_edges_after = after.boundary_edges;
    rep.non_manifold_edges_after = after.non_manifold_edges;
    rep.components_after = after.components;
    rep.volume_after = signed_volume(m);
    ctx.progress("verify", 0.88);
    rep.output_vertices = m.v.size();
    rep.output_faces = m.f.size();
    rep.self_intersections_remain = -1;
    if (ctx.on(MESHFIX_CHECK_SELF_INTERSECTIONS)) {
        ctx.progress("self-intersections", 0.9);
        rep.self_intersecting_pairs = count_self_intersections(m, std::max({ctx.tol, 1.5 * separation, 1e-9 * ctx.diag}), 1000);
        rep.self_intersections_remain = rep.self_intersecting_pairs > 0;
    }
    rep.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ctx.progress("done", 1.0);
    return m;
}

} // namespace meshfix
