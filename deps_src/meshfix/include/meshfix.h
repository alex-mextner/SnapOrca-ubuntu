/*
 * meshfix - triangle mesh repair library, stable C ABI.
 *
 * SPDX-License-Identifier: MIT
 *
 * ABI rules (see README "ABI stability"):
 *  - Every function below is extern "C", uses only fixed-width types and opaque handles.
 *  - Structs that cross the boundary start with a `struct_size` field. The caller sets it to
 *    sizeof(struct) of the header it compiled against (use meshfix_options_init()). The library
 *    reads/writes only the bytes the caller's struct has, so an old binary keeps working with a
 *    newer library and a new binary keeps working with an older one (unknown trailing fields are
 *    left untouched / defaulted).
 *  - New fields are only ever appended at the end of a struct; existing ones never change.
 *  - Ownership: a handle returned by the library is released with its own *_free function. Memory
 *    the library lends (meshfix_mesh_vertices/faces) stays valid until that handle is freed.
 */
#ifndef MESHFIX_H
#define MESHFIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHFIX_VERSION_MAJOR 0
#define MESHFIX_VERSION_MINOR 1
#define MESHFIX_VERSION_PATCH 1
#define MESHFIX_VERSION_STRING "0.1.1"
/* Bumped only when the existing ABI is broken (never for appended fields/functions). */
#define MESHFIX_ABI_VERSION 1

#if defined(MESHFIX_STATIC_DEFINE)
#define MESHFIX_API
#elif defined(_WIN32)
#ifdef MESHFIX_BUILDING
#define MESHFIX_API __declspec(dllexport)
#else
#define MESHFIX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MESHFIX_API __attribute__((visibility("default")))
#else
#define MESHFIX_API
#endif

/* ---------------------------------------------------------------- status codes */

typedef enum meshfix_status {
    MESHFIX_OK = 0,
    MESHFIX_ERR_INVALID_ARGUMENT = 1, /* null pointer, index out of range, bad struct_size */
    MESHFIX_ERR_OUT_OF_MEMORY = 2,
    MESHFIX_ERR_EMPTY_MESH = 3,   /* the input has no faces */
    MESHFIX_ERR_CANCELLED = 4,    /* the progress callback returned non-zero */
    MESHFIX_ERR_IO = 5,           /* file could not be read or written */
    MESHFIX_ERR_FORMAT = 6,       /* file is not a supported / valid mesh file */
    MESHFIX_ERR_INTERNAL = 7      /* bug: an exception escaped; message in meshfix_last_error() */
} meshfix_status;

/* ---------------------------------------------------------------- opaque mesh */

/* An indexed triangle mesh: double xyz vertices, uint32 vertex-index triples. */
typedef struct meshfix_mesh meshfix_mesh;

/* Copies the arrays. vertices: 3*vertex_count doubles, faces: 3*face_count indices. */
MESHFIX_API meshfix_status meshfix_mesh_create(const double *vertices, size_t vertex_count,
                                               const uint32_t *faces, size_t face_count,
                                               meshfix_mesh **out);
/* Same for float32 vertices (e.g. STL, GPU buffers). */
MESHFIX_API meshfix_status meshfix_mesh_create_f32(const float *vertices, size_t vertex_count,
                                                   const uint32_t *faces, size_t face_count,
                                                   meshfix_mesh **out);
MESHFIX_API void meshfix_mesh_free(meshfix_mesh *mesh); /* NULL is a no-op */

MESHFIX_API size_t meshfix_mesh_vertex_count(const meshfix_mesh *mesh);
MESHFIX_API size_t meshfix_mesh_face_count(const meshfix_mesh *mesh);
/* Borrowed arrays, valid until meshfix_mesh_free(mesh): 3*vertex_count doubles / 3*face_count uint32. */
MESHFIX_API const double *meshfix_mesh_vertices(const meshfix_mesh *mesh);
MESHFIX_API const uint32_t *meshfix_mesh_faces(const meshfix_mesh *mesh);

/* Reads .stl (binary or ASCII) or .obj; exact duplicate vertices are merged on load. */
MESHFIX_API meshfix_status meshfix_mesh_read(const char *path, meshfix_mesh **out);
/* Writes .stl (binary, float32) or .obj (by extension; default STL). */
MESHFIX_API meshfix_status meshfix_mesh_write(const meshfix_mesh *mesh, const char *path);

/* ---------------------------------------------------------------- options */

/* The "watertight" and "manifold" results are judged the way an STL consumer sees the mesh: vertices with exactly
 * equal coordinates are one vertex. That is also what makes MESHFIX_FIX_SEPARATE necessary for parts that touch.
 *
 * Which repairs to run (meshfix_options.flags). 0 runs nothing: the output equals the input. */
enum {
    MESHFIX_FIX_WELD = 1u << 0,            /* merge coincident / near-coincident vertices */
    MESHFIX_FIX_DEGENERATE = 1u << 1,      /* drop zero-area and repeated-index faces */
    MESHFIX_FIX_DUPLICATE_FACES = 1u << 2, /* drop duplicate faces, cancel back-to-back pairs */
    MESHFIX_FIX_T_JUNCTIONS = 1u << 3,     /* split edges a vertex of a neighbour lies on */
    MESHFIX_FIX_ORIENTATION = 1u << 4,     /* consistent winding, outward normals */
    MESHFIX_FIX_NON_MANIFOLD = 1u << 5,    /* split non-manifold edges and vertices */
    MESHFIX_FIX_HOLES = 1u << 6,           /* fill boundary loops */
    MESHFIX_FIX_COMPONENTS = 1u << 7,      /* drop tiny islands and zero-volume shells */
    MESHFIX_FIX_NESTED_SHELLS = 1u << 8,   /* shells nested at odd depth become cavities */
    MESHFIX_FIX_SEPARATE = 1u << 9,        /* nudge vertex copies that coincide apart, see meshfix_options.separation */
    MESHFIX_FIX_ALL = 0x3FFu,
    /* Diagnostics, independent of the repairs above. */
    MESHFIX_CHECK_SELF_INTERSECTIONS = 1u << 16
};

/* Return non-zero to cancel. `stage` is a static string, `fraction` is 0..1 overall. */
typedef int (*meshfix_progress_fn)(void *user, const char *stage, double fraction);

typedef struct meshfix_options {
    uint32_t struct_size;         /* sizeof(meshfix_options); set by meshfix_options_init() */
    uint32_t flags;               /* MESHFIX_FIX_* | MESHFIX_CHECK_*; default FIX_ALL | CHECK_SELF_INTERSECTIONS */
    double weld_tolerance;        /* model units; < 0 = auto: min(1e-5 * bbox diagonal, 1e-3); 0 = exact merge only */
    uint32_t max_hole_edges;      /* holes with more boundary edges stay open; 0 = no limit */
    uint32_t min_component_faces; /* components with fewer faces are dropped (default 8) */
    meshfix_progress_fn progress; /* optional */
    void *progress_user;
    /* Distance (model units) the copies of a split vertex are moved apart by, inward along their fan's normal,
     * so that touching parts stop sharing edges when a consumer merges equal vertices. < 0 = auto: 2e-5 * bbox
     * diagonal. Only used with MESHFIX_FIX_SEPARATE. */
    double separation;
    /* append new fields below this line only */
} meshfix_options;

/* Fills `opts` with the defaults and sets struct_size. */
MESHFIX_API void meshfix_options_init(meshfix_options *opts);

/* ---------------------------------------------------------------- report */

typedef struct meshfix_report {
    uint32_t struct_size; /* caller sets sizeof(meshfix_report) (meshfix_report_init) */
    int32_t watertight_before; /* every edge has exactly two faces, winding consistent (see FIX_* note above) */
    int32_t watertight_after;
    int32_t manifold_before;   /* no edge with >2 faces and no vertex whose faces form several fans */
    int32_t manifold_after;
    int32_t self_intersections_remain; /* -1 not checked, 0 none found, 1 found (heuristic: proper piercings only) */
    int32_t reserved0, reserved1; /* keeps the 64-bit counters aligned */

    uint64_t input_vertices, input_faces, output_vertices, output_faces;
    uint64_t vertices_merged;
    uint64_t invalid_faces_removed; /* index out of range or non-finite coordinates */
    uint64_t degenerate_faces_removed;
    uint64_t duplicate_faces_removed;
    uint64_t t_junctions_fixed;
    uint64_t faces_flipped;
    uint64_t non_manifold_edges_before;
    uint64_t non_manifold_edges_after;
    uint64_t non_manifold_vertices_split; /* vertex copies created by splitting fans / edges */
    uint64_t boundary_edges_before;
    uint64_t boundary_edges_after;
    uint64_t holes_found;
    uint64_t holes_filled;
    uint64_t holes_skipped;
    uint64_t hole_faces_added;
    uint64_t components_before; /* edge-connected components */
    uint64_t components_after;
    uint64_t small_components_removed;
    uint64_t flat_components_removed; /* closed shells without volume */
    uint64_t cavity_shells;           /* nested shells turned into cavities */
    uint64_t self_intersecting_pairs; /* capped at 1000 */

    double volume_before; /* signed volume of the input faces (divergence theorem) */
    double volume_after;
    double weld_tolerance_used;
    double elapsed_ms;
    uint64_t vertices_separated; /* vertex copies nudged by MESHFIX_FIX_SEPARATE */
    /* append new fields below this line only */
} meshfix_report;

MESHFIX_API void meshfix_report_init(meshfix_report *report);

/* snprintf-style: writes the report as JSON to buf (if cap > 0), returns the length the full text needs
 * without the terminating NUL. */
MESHFIX_API size_t meshfix_report_to_json(const meshfix_report *report, char *buf, size_t cap);

/* ---------------------------------------------------------------- operations */

/* Repairs `in` (unchanged) into a new mesh. opts and report may be NULL. On error *out is NULL. */
MESHFIX_API meshfix_status meshfix_repair(const meshfix_mesh *in, const meshfix_options *opts,
                                          meshfix_mesh **out, meshfix_report *report);

/* Inspection only: fills the *_before fields; the *_after fields mirror them, since nothing changes. */
MESHFIX_API meshfix_status meshfix_analyze(const meshfix_mesh *mesh, const meshfix_options *opts,
                                          meshfix_report *report);

/* ---------------------------------------------------------------- misc */

MESHFIX_API const char *meshfix_version(void);       /* "0.1.1" */
MESHFIX_API uint32_t meshfix_abi_version(void);      /* MESHFIX_ABI_VERSION of the loaded library */
MESHFIX_API const char *meshfix_status_string(meshfix_status status);
/* Message of the last failure on this thread ("" if none). Valid until the next meshfix call on the thread. */
MESHFIX_API const char *meshfix_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* MESHFIX_H */
