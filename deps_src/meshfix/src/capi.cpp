// The C ABI of meshfix.h. SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "core.hpp"
#include "io.hpp"

static_assert(sizeof(meshfix::Vec3) == 3 * sizeof(double), "Vec3 is handed out as a double[3] array");
static_assert(sizeof(meshfix::Tri) == 3 * sizeof(uint32_t), "Tri is handed out as a uint32[3] array");

struct meshfix_mesh {
    meshfix::Mesh m;
};

namespace {

thread_local std::string g_last_error;

meshfix_status fail(meshfix_status status, const std::string &message) {
    g_last_error = message;
    return status;
}

// Runs `body`, mapping exceptions to status codes.
template <class F> meshfix_status guarded(F &&body) {
    g_last_error.clear();
    try {
        return body();
    } catch (const meshfix::Cancelled &) {
        return fail(MESHFIX_ERR_CANCELLED, "cancelled by the progress callback");
    } catch (const std::bad_alloc &) {
        return fail(MESHFIX_ERR_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception &e) {
        return fail(MESHFIX_ERR_INTERNAL, e.what());
    } catch (...) {
        return fail(MESHFIX_ERR_INTERNAL, "unknown exception");
    }
}

// The caller's struct may be older or newer than ours: take the common prefix.
bool read_options(const meshfix_options *in, meshfix_options &out) {
    meshfix_options_init(&out);
    if (!in)
        return true;
    if (in->struct_size < offsetof(meshfix_options, weld_tolerance))
        return false;
    std::memcpy(&out, in, std::min<size_t>(in->struct_size, sizeof out));
    out.struct_size = sizeof out;
    return true;
}

void write_report(const meshfix_report &full, meshfix_report *out) {
    const uint32_t size = out->struct_size;
    std::memcpy(out, &full, std::min<size_t>(size, sizeof full));
    out->struct_size = size;
}

meshfix_mesh *make_mesh(meshfix::Mesh &&m) { return new meshfix_mesh{std::move(m)}; }

template <class T>
meshfix_status create_mesh(const T *vertices, size_t vertex_count, const uint32_t *faces, size_t face_count, meshfix_mesh **out) {
    if (!out)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "out is NULL");
    *out = nullptr;
    if ((vertex_count && !vertices) || (face_count && !faces))
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "NULL array with a non-zero count");
    if (vertex_count > 0xFFFFFFFFu)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "more than 2^32-1 vertices");
    return guarded([&] {
        meshfix::Mesh m;
        m.v.resize(vertex_count);
        for (size_t i = 0; i < vertex_count; ++i)
            m.v[i] = {double(vertices[3 * i]), double(vertices[3 * i + 1]), double(vertices[3 * i + 2])};
        m.f.resize(face_count);
        for (size_t i = 0; i < face_count; ++i)
            m.f[i] = {faces[3 * i], faces[3 * i + 1], faces[3 * i + 2]};
        *out = make_mesh(std::move(m));
        return MESHFIX_OK;
    });
}

} // namespace

extern "C" {

meshfix_status meshfix_mesh_create(const double *vertices, size_t vertex_count, const uint32_t *faces, size_t face_count, meshfix_mesh **out) {
    return create_mesh(vertices, vertex_count, faces, face_count, out);
}

meshfix_status meshfix_mesh_create_f32(const float *vertices, size_t vertex_count, const uint32_t *faces, size_t face_count, meshfix_mesh **out) {
    return create_mesh(vertices, vertex_count, faces, face_count, out);
}

void meshfix_mesh_free(meshfix_mesh *mesh) { delete mesh; }

size_t meshfix_mesh_vertex_count(const meshfix_mesh *mesh) { return mesh ? mesh->m.v.size() : 0; }
size_t meshfix_mesh_face_count(const meshfix_mesh *mesh) { return mesh ? mesh->m.f.size() : 0; }
const double *meshfix_mesh_vertices(const meshfix_mesh *mesh) {
    return mesh && !mesh->m.v.empty() ? &mesh->m.v[0].x : nullptr;
}
const uint32_t *meshfix_mesh_faces(const meshfix_mesh *mesh) {
    return mesh && !mesh->m.f.empty() ? mesh->m.f[0].data() : nullptr;
}

meshfix_status meshfix_mesh_read(const char *path, meshfix_mesh **out) {
    if (!path || !out)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "NULL path or out");
    *out = nullptr;
    return guarded([&] {
        meshfix::Mesh m;
        const meshfix::IoStatus st = meshfix::read_mesh_file(path, m);
        if (st == meshfix::IoStatus::Io)
            return fail(MESHFIX_ERR_IO, std::string("cannot read ") + path);
        if (st == meshfix::IoStatus::Format)
            return fail(MESHFIX_ERR_FORMAT, std::string("not a valid .stl/.obj file: ") + path);
        // Exactly equal vertices are one vertex, as every STL consumer treats them.
        for (const meshfix::Tri &t : m.f)
            for (uint32_t x : t)
                if (x >= m.v.size())
                    return fail(MESHFIX_ERR_FORMAT, std::string("face index out of range: ") + path);
        meshfix::weld_vertices(m, 0);
        *out = make_mesh(std::move(m));
        return MESHFIX_OK;
    });
}

meshfix_status meshfix_mesh_write(const meshfix_mesh *mesh, const char *path) {
    if (!mesh || !path)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "NULL mesh or path");
    return guarded([&] {
        const meshfix::IoStatus st = meshfix::write_mesh_file(mesh->m, path);
        if (st == meshfix::IoStatus::Format)
            return fail(MESHFIX_ERR_FORMAT, "too many faces for a binary STL");
        return st == meshfix::IoStatus::Ok ? MESHFIX_OK : fail(MESHFIX_ERR_IO, std::string("cannot write ") + path);
    });
}

void meshfix_options_init(meshfix_options *opts) {
    if (!opts)
        return;
    std::memset(opts, 0, sizeof *opts);
    opts->struct_size = sizeof *opts;
    opts->flags = MESHFIX_FIX_ALL | MESHFIX_CHECK_SELF_INTERSECTIONS;
    opts->weld_tolerance = -1.0;
    opts->max_hole_edges = 0;
    opts->min_component_faces = 8;
    opts->separation = -1.0;
}

void meshfix_report_init(meshfix_report *report) {
    if (!report)
        return;
    std::memset(report, 0, sizeof *report);
    report->struct_size = sizeof *report;
    report->self_intersections_remain = -1;
}

size_t meshfix_report_to_json(const meshfix_report *caller, char *buf, size_t cap) {
    if (!caller || caller->struct_size < sizeof(uint32_t))
        return 0;
    // An older caller's struct ends early: read only what it has, the rest counts as zero.
    meshfix_report full;
    meshfix_report_init(&full);
    std::memcpy(&full, caller, std::min<size_t>(caller->struct_size, sizeof full));
    const meshfix_report *r = &full;
    std::string s = "{";
    const auto num = [&](const char *name, uint64_t v) { s += std::string("\"") + name + "\": " + std::to_string(v) + ", "; };
    const auto flt = [&](const char *name, double v) {
        char t[64];
        std::snprintf(t, sizeof t, "%.9g", v);
        s += std::string("\"") + name + "\": " + t + ", ";
    };
    const auto boolean = [&](const char *name, int32_t v) { s += std::string("\"") + name + "\": " + (v ? "true" : "false") + ", "; };
    boolean("watertight_before", r->watertight_before);
    boolean("watertight_after", r->watertight_after);
    boolean("manifold_before", r->manifold_before);
    boolean("manifold_after", r->manifold_after);
    s += std::string("\"self_intersections_remain\": ") +
         (r->self_intersections_remain < 0 ? "null" : (r->self_intersections_remain ? "true" : "false")) + ", ";
    num("input_vertices", r->input_vertices);
    num("input_faces", r->input_faces);
    num("output_vertices", r->output_vertices);
    num("output_faces", r->output_faces);
    num("vertices_merged", r->vertices_merged);
    num("invalid_faces_removed", r->invalid_faces_removed);
    num("degenerate_faces_removed", r->degenerate_faces_removed);
    num("duplicate_faces_removed", r->duplicate_faces_removed);
    num("t_junctions_fixed", r->t_junctions_fixed);
    num("faces_flipped", r->faces_flipped);
    num("non_manifold_edges_before", r->non_manifold_edges_before);
    num("non_manifold_edges_after", r->non_manifold_edges_after);
    num("non_manifold_vertices_split", r->non_manifold_vertices_split);
    num("boundary_edges_before", r->boundary_edges_before);
    num("boundary_edges_after", r->boundary_edges_after);
    num("holes_found", r->holes_found);
    num("holes_filled", r->holes_filled);
    num("holes_skipped", r->holes_skipped);
    num("hole_faces_added", r->hole_faces_added);
    num("components_before", r->components_before);
    num("components_after", r->components_after);
    num("small_components_removed", r->small_components_removed);
    num("flat_components_removed", r->flat_components_removed);
    num("cavity_shells", r->cavity_shells);
    num("self_intersecting_pairs", r->self_intersecting_pairs);
    num("vertices_separated", r->vertices_separated);
    flt("volume_before", r->volume_before);
    flt("volume_after", r->volume_after);
    flt("weld_tolerance_used", r->weld_tolerance_used);
    flt("elapsed_ms", r->elapsed_ms);
    s.resize(s.size() - 2);
    s += "}";
    if (cap > 0 && buf) {
        const size_t n = std::min(s.size(), cap - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return s.size();
}

meshfix_status meshfix_repair(const meshfix_mesh *in, const meshfix_options *opts, meshfix_mesh **out, meshfix_report *report) {
    if (!out)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "out is NULL");
    *out = nullptr;
    if (!in)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "mesh is NULL");
    if (report && report->struct_size < sizeof(uint32_t))
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "report->struct_size not set (use meshfix_report_init)");
    meshfix_options options;
    if (!read_options(opts, options))
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "options->struct_size too small");
    if (in->m.f.empty())
        return fail(MESHFIX_ERR_EMPTY_MESH, "the mesh has no faces");
    return guarded([&] {
        meshfix_report full;
        meshfix_report_init(&full);
        meshfix::Mesh result = meshfix::repair_mesh(in->m, options, full);
        *out = make_mesh(std::move(result));
        if (report)
            write_report(full, report);
        return MESHFIX_OK;
    });
}

meshfix_status meshfix_analyze(const meshfix_mesh *mesh, const meshfix_options *opts, meshfix_report *report) {
    if (!mesh || !report)
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "NULL mesh or report");
    if (report->struct_size < sizeof(uint32_t))
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "report->struct_size not set (use meshfix_report_init)");
    meshfix_options options;
    if (!read_options(opts, options))
        return fail(MESHFIX_ERR_INVALID_ARGUMENT, "options->struct_size too small");
    if (mesh->m.f.empty())
        return fail(MESHFIX_ERR_EMPTY_MESH, "the mesh has no faces");
    return guarded([&] {
        meshfix_report full;
        meshfix_report_init(&full);
        meshfix::Ctx ctx{options, full};
        meshfix::analyze_into(mesh->m, ctx);
        write_report(full, report);
        return MESHFIX_OK;
    });
}

const char *meshfix_version(void) { return MESHFIX_VERSION_STRING; }
uint32_t meshfix_abi_version(void) { return MESHFIX_ABI_VERSION; }

const char *meshfix_status_string(meshfix_status status) {
    switch (status) {
    case MESHFIX_OK: return "ok";
    case MESHFIX_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MESHFIX_ERR_OUT_OF_MEMORY: return "out of memory";
    case MESHFIX_ERR_EMPTY_MESH: return "empty mesh";
    case MESHFIX_ERR_CANCELLED: return "cancelled";
    case MESHFIX_ERR_IO: return "i/o error";
    case MESHFIX_ERR_FORMAT: return "unsupported or invalid file format";
    case MESHFIX_ERR_INTERNAL: return "internal error";
    }
    return "unknown status";
}

const char *meshfix_last_error(void) { return g_last_error.c_str(); }

} // extern "C"
