#include "Exception.hpp"
#include "MeshBoolean.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TryCatchSignal.hpp"
#include "libslic3r/format.hpp"
#undef PI

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

#include <boost/next_prior.hpp>
#include "boost/log/trivial.hpp"
// Include igl first. It defines "L" macro which then clashes with our localization
#include <igl/copyleft/cgal/mesh_boolean.h>
#undef L

// CGAL headers
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Exact_integer.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Cartesian_converter.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/intersection.h>
#include <CGAL/Side_of_triangle_mesh.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <CGAL/iterator.h>
#include <CGAL/utility.h>
// BBS: for segment
#include <CGAL/mesh_segmentation.h>
#include <CGAL/property_map.h>
#include <CGAL/boost/graph/copy_face_graph.h>
#include <CGAL/boost/graph/Face_filtered_graph.h>
// BBS: for boolean using mcut
#include "mcut/include/mcut/mcut.h"
// Orca: mesh repair ("Fix model"), C ABI of deps_src/meshfix
#include "meshfix.h"

namespace Slic3r {
namespace MeshBoolean {

using MapMatrixXfUnaligned = Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;
using MapMatrixXiUnaligned = Eigen::Map<const Eigen::Matrix<int,   Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;

TriangleMesh eigen_to_triangle_mesh(const EigenMesh &emesh)
{
    auto &VC = emesh.first; auto &FC = emesh.second;

    indexed_triangle_set its;
    its.vertices.reserve(size_t(VC.rows()));
    its.indices.reserve(size_t(FC.rows()));

    for (Eigen::Index i = 0; i < VC.rows(); ++i)
        its.vertices.emplace_back(VC.row(i).cast<float>());

    for (Eigen::Index i = 0; i < FC.rows(); ++i)
        its.indices.emplace_back(FC.row(i));

    return TriangleMesh { std::move(its) };
}

EigenMesh triangle_mesh_to_eigen(const TriangleMesh &mesh)
{
    EigenMesh emesh;
    emesh.first = MapMatrixXfUnaligned(mesh.its.vertices.front().data(),
                                       Eigen::Index(mesh.its.vertices.size()),
                                       3).cast<double>();

    emesh.second = MapMatrixXiUnaligned(mesh.its.indices.front().data(),
                                        Eigen::Index(mesh.its.indices.size()),
                                        3);
    return emesh;
}

void minus(EigenMesh &A, const EigenMesh &B)
{
    auto &[VA, FA] = A;
    auto &[VB, FB] = B;

    Eigen::MatrixXd VC;
    Eigen::MatrixXi FC;
    igl::MeshBooleanType boolean_type(igl::MESH_BOOLEAN_TYPE_MINUS);
    igl::copyleft::cgal::mesh_boolean(VA, FA, VB, FB, boolean_type, VC, FC);

    VA = std::move(VC); FA = std::move(FC);
}

void minus(TriangleMesh& A, const TriangleMesh& B)
{
    EigenMesh eA = triangle_mesh_to_eigen(A);
    minus(eA, triangle_mesh_to_eigen(B));
    A = eigen_to_triangle_mesh(eA);
}

void self_union(EigenMesh &A)
{
    EigenMesh result;
    auto &[V, F] = A;
    auto &[VC, FC] = result;

    igl::MeshBooleanType boolean_type(igl::MESH_BOOLEAN_TYPE_UNION);
    igl::copyleft::cgal::mesh_boolean(V, F, Eigen::MatrixXd(), Eigen::MatrixXi(), boolean_type, VC, FC);

    A = std::move(result);
}

void self_union(TriangleMesh& mesh)
{
    auto eM = triangle_mesh_to_eigen(mesh);
    self_union(eM);
    mesh = eigen_to_triangle_mesh(eM);
}

namespace cgal {

namespace CGALProc    = CGAL::Polygon_mesh_processing;
namespace CGALParams  = CGAL::Polygon_mesh_processing::parameters;

using EpecKernel = CGAL::Exact_predicates_exact_constructions_kernel;
using EpicKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using _EpicMesh = CGAL::Surface_mesh<EpicKernel::Point_3>;
using _EpecMesh = CGAL::Surface_mesh<EpecKernel::Point_3>;

struct CGALMesh {
    _EpicMesh m;
    CGALMesh() = default;
    CGALMesh(const _EpicMesh& _m) :m(_m) {}
};

void save_CGALMesh(const std::string& fname, const CGALMesh& cgal_mesh) {
    std::ofstream os(fname);
    os << cgal_mesh.m;
    os.close();
}
// /////////////////////////////////////////////////////////////////////////////
// Converions from and to CGAL mesh
// /////////////////////////////////////////////////////////////////////////////

template<class _Mesh> void triangle_mesh_to_cgal(const TriangleMesh& M, _Mesh& out)
{
    using Index3 = std::array<size_t, 3>;

    if (M.empty()) return;

    std::vector<typename _Mesh::Point> points;
    std::vector<Index3> indices;
    points.reserve(M.its.vertices.size());
    indices.reserve(M.its.indices.size());
    for (auto& v : M.its.vertices) points.emplace_back(v.x(), v.y(), v.z());
    for (auto& _f : M.its.indices) {
        auto f = _f.cast<size_t>();
        indices.emplace_back(Index3{ f(0), f(1), f(2) });
    }

    CGALProc::orient_polygon_soup(points, indices);
    CGALProc::polygon_soup_to_polygon_mesh(points, indices, out);

    // Number the faces because 'orient_to_bound_a_volume' needs a face <--> index map
    unsigned index = 0;
    for (auto face : out.faces()) face = CGAL::SM_Face_index(index++);

    if (CGAL::is_closed(out))
        CGALProc::orient_to_bound_a_volume(out);
    else
        throw Slic3r::RuntimeError("Mesh not watertight");
}

template<class _Mesh>
void triangle_mesh_to_cgal(const std::vector<stl_vertex> &                 V,
                           const std::vector<stl_triangle_vertex_indices> &F,
                           _Mesh &out)
{
    if (F.empty()) return;

    size_t vertices_count = V.size();
    size_t edges_count    = (F.size()* 3) / 2;
    size_t faces_count    = F.size();
    out.reserve(vertices_count, edges_count, faces_count);

    for (auto &v : V)
        out.add_vertex(typename _Mesh::Point{v.x(), v.y(), v.z()});

    using VI = typename _Mesh::Vertex_index;
    for (auto &f : F)
        out.add_face(VI(f(0)), VI(f(1)), VI(f(2)));
}

inline Vec3f to_vec3f(const _EpicMesh::Point& v)
{
    return { float(v.x()), float(v.y()), float(v.z()) };
}

inline Vec3f to_vec3f(const _EpecMesh::Point& v)
{
    CGAL::Cartesian_converter<EpecKernel, EpicKernel> cvt;
    auto iv = cvt(v);
    return { float(iv.x()), float(iv.y()), float(iv.z()) };
}

template<class _Mesh>
indexed_triangle_set cgal_to_indexed_triangle_set(const _Mesh &cgalmesh)
{
    indexed_triangle_set its;
    its.vertices.reserve(cgalmesh.num_vertices());
    its.indices.reserve(cgalmesh.num_faces());

    const auto &faces = cgalmesh.faces();
    const auto &vertices = cgalmesh.vertices();
    int vsize = int(vertices.size());

    for (const auto &vi : vertices) {
        auto &v = cgalmesh.point(vi); // Don't ask...
        its.vertices.emplace_back(to_vec3f(v));
    }

    for (const auto &face : faces) {
        auto vtc = cgalmesh.vertices_around_face(cgalmesh.halfedge(face));

        int i = 0;
        Vec3i32 facet;
        for (auto v : vtc) {
            int iv = v;
            if (i > 2 || iv < 0 || iv >= vsize) { i = 0; break; }
            facet(i++) = iv;
        }

        if (i == 3)
            its.indices.emplace_back(facet);
    }

    return its;
}

template<class _Mesh> TriangleMesh cgal_to_triangle_mesh(const _Mesh &cgalmesh)
{
    indexed_triangle_set its = cgal_to_indexed_triangle_set(cgalmesh);
    return TriangleMesh(std::move(its));
}

std::unique_ptr<CGALMesh, CGALMeshDeleter>
triangle_mesh_to_cgal(const std::vector<stl_vertex> &V,
                      const std::vector<stl_triangle_vertex_indices> &F)
{
    std::unique_ptr<CGALMesh, CGALMeshDeleter> out(new CGALMesh{});
    triangle_mesh_to_cgal(V, F, out->m);
    return out;
}

TriangleMesh cgal_to_triangle_mesh(const CGALMesh &cgalmesh)
{
    return TriangleMesh{cgal_to_indexed_triangle_set(cgalmesh.m)};
}

indexed_triangle_set cgal_to_indexed_triangle_set(const CGALMesh &cgalmesh)
{
    return cgal_to_indexed_triangle_set(cgalmesh.m);
}

// /////////////////////////////////////////////////////////////////////////////
// Boolean operations for CGAL meshes
// /////////////////////////////////////////////////////////////////////////////

static bool _cgal_diff(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_difference(A.m, B.m, R.m, p, p);
}

static bool _cgal_union(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_union(A.m, B.m, R.m, p, p);
}

static bool _cgal_intersection(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_intersection(A.m, B.m, R.m, p, p);
}

template<class Op> void _cgal_do(Op &&op, CGALMesh &A, CGALMesh &B)
{
    bool success = false;
    bool hw_fail = false;
    try {
        CGALMesh result;
        try_catch_signal({SIGSEGV, SIGFPE}, [&success, &A, &B, &result, &op] {
            success = op(A, B, result);
        }, [&] { hw_fail = true; });
        A = std::move(result);      // In-place operation does not work
    } catch (...) {
        success = false;
    }

    if (hw_fail)
        throw Slic3r::HardCrash("CGAL mesh boolean operation crashed.");

    if (! success)
        throw Slic3r::RuntimeError("CGAL mesh boolean operation failed.");
}

void minus(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_diff, A, B); }
void plus(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_union, A, B); }
void intersect(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_intersection, A, B); }
bool does_self_intersect(const CGALMesh &mesh) { return CGALProc::does_self_intersect(mesh.m); }
// BBS
void segment(CGALMesh& src, std::vector<CGALMesh>& dst, double smoothing_alpha = 0.5, int segment_number=5)
{
    typedef boost::graph_traits<_EpicMesh>::face_descriptor face_descriptor;
    typedef _EpicMesh::Property_map<face_descriptor, double> Facet_double_map;
    typedef CGAL::Face_filtered_graph<_EpicMesh> Filtered_graph;

    _EpicMesh mesh = src.m;
    Facet_double_map sdf_property_map;

    sdf_property_map = mesh.add_property_map<face_descriptor, double>("f:sdf").first;

    CGAL::sdf_values(mesh, sdf_property_map);

    // create a property-map for segment-ids
    typedef _EpicMesh::Property_map<face_descriptor, std::size_t> Facet_int_map;
    Facet_int_map segment_property_map = mesh.add_property_map<face_descriptor, std::size_t>("f:sid").first;;
    // segment the mesh using default parameters for number of levels, and smoothing lambda
    // Any other scalar values can be used instead of using SDF values computed using the CGAL function
    std::size_t number_of_segments = CGAL::segmentation_from_sdf_values(mesh, sdf_property_map, segment_property_map, segment_number, smoothing_alpha);
    //print area of each segment and then put it in a Mesh and print it in an OFF file
    Filtered_graph segment_mesh(mesh);
    _EpicMesh mesh_merged;
    for (std::size_t id = 0; id < number_of_segments; ++id)
    {
        segment_mesh.set_selected_faces(id, segment_property_map);
        //std::cout << "Segment " << id << "'s area is : " << CGAL::Polygon_mesh_processing::area(segment_mesh) << std::endl;
        _EpicMesh out;
        CGAL::copy_face_graph(segment_mesh, out);

        // save_CGALMesh("out.off", out);

        // fill holes
        typedef boost::graph_traits<_EpicMesh>::halfedge_descriptor      halfedge_descriptor;
        typedef boost::graph_traits<_EpicMesh>::vertex_descriptor        vertex_descriptor;
        std::vector<halfedge_descriptor> border_cycles;
        CGAL::Polygon_mesh_processing::extract_boundary_cycles(out, std::back_inserter(border_cycles));
        for (halfedge_descriptor h : border_cycles)
        {
            std::vector<face_descriptor>  patch_facets;
#if 0
            std::vector<vertex_descriptor> patch_vertices;
            CGAL::Polygon_mesh_processing::triangulate_and_refine_hole(out, h, std::back_inserter(patch_facets),
                std::back_inserter(patch_vertices));
            std::cout << "* Number of facets in constructed patch: " << patch_facets.size() << std::endl;
            std::cout << "  Number of vertices in constructed patch: " << patch_vertices.size() << std::endl;
#else
            CGAL::Polygon_mesh_processing::triangulate_hole(out, h, std::back_inserter(patch_facets));
#endif
        }

        //if (id > 2) {
        //    mesh_merged.join(out);
        //}
        //else
        {
            dst.emplace_back(std::move(CGALMesh(out)));
        }
    }
    //if (mesh_merged.is_empty() == false) {
    //    CGAL::Polygon_mesh_processing::stitch_borders(mesh_merged);
    //    dst.emplace_back(std::move(CGALMesh(mesh_merged)));
    //}
}

std::vector<TriangleMesh> segment(const TriangleMesh& src, double smoothing_alpha, int segment_number)
{
    CGALMesh in_cgal_mesh;
    MeshBoolean::cgal::triangle_mesh_to_cgal(src, in_cgal_mesh.m);
    std::vector<CGALMesh> out_cgal_meshes;
    segment(in_cgal_mesh, out_cgal_meshes, smoothing_alpha, segment_number);

    std::vector<TriangleMesh> out_meshes;
    for (auto& outf_cgal_mesh: out_cgal_meshes)
    {
        out_meshes.emplace_back(std::move(cgal_to_triangle_mesh(outf_cgal_mesh.m)));
    }

    return out_meshes;
}

void merge(std::vector<_EpicMesh>& srcs, _EpicMesh& dst)
{
    _EpicMesh mesh_merged;
    for (size_t i = 0; i < srcs.size(); i++)
    {
        mesh_merged.join(srcs[i]);
    }
    if (mesh_merged.is_empty() == false) {
        CGAL::Polygon_mesh_processing::stitch_borders(mesh_merged);
        dst = std::move(mesh_merged);
    }
}

TriangleMesh merge(std::vector<TriangleMesh> meshes)
{
    std::vector<_EpicMesh> srcs(meshes.size());
    for (size_t i = 0; i < meshes.size(); i++)
    {
        MeshBoolean::cgal::triangle_mesh_to_cgal(meshes[i], srcs[i]);
    }
    _EpicMesh dst;
    merge(srcs, dst);
    return cgal_to_triangle_mesh(dst);
}

template<class Op> void _mesh_boolean_do(Op &&op, indexed_triangle_set &A, const indexed_triangle_set &B)
{
    CGALMesh meshA;
    CGALMesh meshB;
    triangle_mesh_to_cgal(A.vertices, A.indices, meshA.m);
    triangle_mesh_to_cgal(B.vertices, B.indices, meshB.m);

    _cgal_do(op, meshA, meshB);

    A = cgal_to_indexed_triangle_set(meshA.m);
}

template<class Op> void _mesh_boolean_do(Op &&op, TriangleMesh &A, const TriangleMesh &B)
{
    CGALMesh meshA;
    CGALMesh meshB;
    triangle_mesh_to_cgal(A.its.vertices, A.its.indices, meshA.m);
    triangle_mesh_to_cgal(B.its.vertices, B.its.indices, meshB.m);

    _cgal_do(op, meshA, meshB);

    A = cgal_to_triangle_mesh(meshA);
}

void minus(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_diff, A, B);
}

void plus(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_union, A, B);
}

void intersect(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_intersection, A, B);
}

void minus(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_diff, A, B);
}

void plus(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_union, A, B);
}

void intersect(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_intersection, A, B);
}

bool does_self_intersect(const TriangleMesh &mesh)
{
    CGALMesh cgalm;
    triangle_mesh_to_cgal(mesh.its.vertices, mesh.its.indices, cgalm.m);
    return CGALProc::does_self_intersect(cgalm.m);
}

void CGALMeshDeleter::operator()(CGALMesh *ptr) { delete ptr; }

bool does_bound_a_volume(const CGALMesh &mesh)
{
    return CGAL::is_closed(mesh.m) && CGALProc::does_bound_a_volume(mesh.m);
}

bool empty(const CGALMesh &mesh)
{
    return mesh.m.is_empty();
}

namespace {

using SoupPoint   = _EpicMesh::Point;
using SoupPolygon = std::vector<std::size_t>;

// Orca: The region the shells enclose, all of them oriented outwards, where depth[i] is the nesting level of shell i:
// the shells at level 0 minus (the shells at level 1 minus (the shells at level 2 minus ...)). Computed from the
// deepest shells outwards, so that a solid inside a cavity survives. Returns false when CGAL cannot compute it, e.g.
// for shells touching at an edge.
bool enclosed_region(const std::vector<_EpicMesh> &shells, const std::vector<size_t> &depth, _EpicMesh &result)
{
    namespace PMP = CGAL::Polygon_mesh_processing;

    if (shells.empty())
        return false;
    // The region enclosed by the shells deeper than the current depth; corefinement changes its inputs, so the shells
    // are copied.
    _EpicMesh inner;
    bool      has_inner = false;
    try {
        for (size_t d = *std::max_element(depth.begin(), depth.end()) + 1; d-- > 0;) {
            _EpicMesh level;
            bool      has_level = false;
            for (size_t i = 0; i < shells.size(); ++i)
                if (depth[i] == d) {
                    _EpicMesh shell = shells[i];
                    _EpicMesh out;
                    if (!has_level)
                        out = std::move(shell);
                    else if (!PMP::corefine_and_compute_union(level, shell, out))
                        return false;
                    level     = std::move(out);
                    has_level = true;
                }
            if (has_level && has_inner) {
                _EpicMesh out;
                if (!PMP::corefine_and_compute_difference(level, inner, out))
                    return false;
                level = std::move(out);
            }
            // Without shells at this level, the region from this level inwards is empty.
            inner     = std::move(level);
            has_inner = has_level;
        }
    } catch (const std::exception &) {
        // Corefinement rejects some degenerate configurations by throwing.
        return false;
    }
    result = std::move(inner);
    return has_inner;
}

// Orca: Resolves a closed mesh whose shells overlap. A shell at an odd nesting level is a cavity, the others are solids,
// see enclosed_region(). A self-union with corefine_and_compute_union() is not possible, it requires a mesh without
// self-intersections. When the booleans fail, e.g. for shells touching at an edge, or a shell intersects itself, the
// shells are only oriented: solids outwards, cavities inwards.
void resolve_overlapping_shells(_EpicMesh &cgal_mesh)
{
    namespace PMP = CGAL::Polygon_mesh_processing;

    std::vector<_EpicMesh> shells;
    PMP::split_connected_components(cgal_mesh, shells);
    std::vector<bool> self_intersecting(shells.size());
    for (size_t i = 0; i < shells.size(); ++i) {
        self_intersecting[i] = PMP::does_self_intersect(shells[i]);
        if (!PMP::is_outward_oriented(shells[i]))
            PMP::reverse_face_orientations(shells[i]);
    }

    // One AABB tree per shell, built once.
    std::vector<std::unique_ptr<CGAL::Side_of_triangle_mesh<_EpicMesh, EpicKernel>>> side_of(shells.size());
    for (size_t j = 0; j < shells.size(); ++j)
        if (!self_intersecting[j])
            side_of[j] = std::make_unique<CGAL::Side_of_triangle_mesh<_EpicMesh, EpicKernel>>(shells[j]);

    // Shells that contain shell i without intersecting it.
    std::vector<std::vector<size_t>> containers(shells.size());
    for (size_t i = 0; i < shells.size(); ++i) {
        const SoupPoint &probe = shells[i].point(*shells[i].vertices().begin());
        for (size_t j = 0; j < shells.size(); ++j)
            if (j != i && side_of[j] && (*side_of[j])(probe) == CGAL::ON_BOUNDED_SIDE && !PMP::do_intersect(shells[i], shells[j]))
                containers[i].push_back(j);
    }
    // The nesting level of a shell is the length of the longest chain of shells containing each other around it, not
    // the number of shells around it: overlapping shells around it make one solid, one level. The containers of a shell
    // also contain its containers, so they have fewer containers and get their levels first.
    std::vector<size_t> order(shells.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&containers](size_t a, size_t b) { return containers[a].size() < containers[b].size(); });
    std::vector<size_t> depth(shells.size(), 0);
    for (size_t i : order)
        for (size_t j : containers[i])
            depth[i] = std::max(depth[i], depth[j] + 1);

    _EpicMesh result;
    if (std::none_of(self_intersecting.begin(), self_intersecting.end(), [](bool b) { return b; }) &&
        enclosed_region(shells, depth, result)) {
        cgal_mesh = std::move(result);
        return;
    }

    _EpicMesh oriented;
    for (size_t i = 0; i < shells.size(); ++i) {
        if (depth[i] % 2 == 1)
            PMP::reverse_face_orientations(shells[i]);
        CGAL::copy_face_graph(shells[i], oriented);
    }
    cgal_mesh = std::move(oriented);
}

// Orca: Hands the mesh to libmeshfix (deps_src/meshfix, C ABI) and back. Returns false with a message when the
// library fails; `report` is filled on success.
bool meshfix_repair_its(const indexed_triangle_set &src, indexed_triangle_set &result, meshfix_report &report, std::string *error)
{
    const auto fail = [error](const std::string &message) {
        if (error)
            *error = message;
        return false;
    };
    // float xyz and index triples as the library takes them; a negative index becomes a huge one, which the library drops
    // as an invalid face.
    std::vector<float> vertices;
    vertices.reserve(src.vertices.size() * 3);
    for (const stl_vertex &v : src.vertices)
        vertices.insert(vertices.end(), {v.x(), v.y(), v.z()});
    std::vector<uint32_t> faces;
    faces.reserve(src.indices.size() * 3);
    for (const stl_triangle_vertex_indices &f : src.indices)
        faces.insert(faces.end(), {uint32_t(f(0)), uint32_t(f(1)), uint32_t(f(2))});

    using MeshfixPtr = std::unique_ptr<meshfix_mesh, decltype(&meshfix_mesh_free)>;
    meshfix_mesh *raw = nullptr;
    if (meshfix_mesh_create_f32(vertices.data(), src.vertices.size(), faces.data(), src.indices.size(), &raw) != MESHFIX_OK)
        return fail(std::string("Repair failed: ") + meshfix_last_error());
    MeshfixPtr in(raw, &meshfix_mesh_free);

    // Every repair; the self-intersection check is CGAL's, in the caller (it is exact, the library's is not).
    meshfix_options options;
    meshfix_options_init(&options);
    // The smallest closed solid has four faces. The library's default of eight would drop a tetrahedral part, which
    // the CGAL repair kept; open fragments of fewer faces are still dropped.
    options.min_component_faces = 4;
    options.flags &= ~uint32_t(MESHFIX_CHECK_SELF_INTERSECTIONS);
    meshfix_report_init(&report);
    raw = nullptr;
    if (meshfix_repair(in.get(), &options, &raw, &report) != MESHFIX_OK)
        return fail(std::string("Repair failed: ") + meshfix_last_error());
    MeshfixPtr out(raw, &meshfix_mesh_free);

    const size_t    vertex_count = meshfix_mesh_vertex_count(out.get());
    const size_t    face_count   = meshfix_mesh_face_count(out.get());
    const double   *v            = meshfix_mesh_vertices(out.get());
    const uint32_t *f            = meshfix_mesh_faces(out.get());
    result.vertices.clear();
    result.indices.clear();
    result.vertices.reserve(vertex_count);
    result.indices.reserve(face_count);
    for (size_t i = 0; i < vertex_count; ++i)
        result.vertices.emplace_back(float(v[3 * i]), float(v[3 * i + 1]), float(v[3 * i + 2]));
    for (size_t i = 0; i < face_count; ++i)
        result.indices.emplace_back(int(f[3 * i]), int(f[3 * i + 1]), int(f[3 * i + 2]));
    return true;
}

// Orca: The closed, consistently oriented mesh as a CGAL surface mesh, without the orientation step of
// triangle_mesh_to_cgal(), which assumes shells that do not overlap.
_EpicMesh its_to_surface_mesh(const indexed_triangle_set &its)
{
    std::vector<SoupPoint>   points;
    std::vector<SoupPolygon> polygons;
    points.reserve(its.vertices.size());
    polygons.reserve(its.indices.size());
    for (const stl_vertex &v : its.vertices)
        points.emplace_back(v.x(), v.y(), v.z());
    for (const stl_triangle_vertex_indices &f : its.indices)
        polygons.push_back({size_t(f(0)), size_t(f(1)), size_t(f(2))});
    _EpicMesh mesh;
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(points, polygons, mesh);
    return mesh;
}

// Orca: repair() without its guard against crashes inside CGAL.
bool repair_unguarded(TriangleMesh &mesh, RepairedMeshErrors *repaired_errors, std::string *error, bool *self_intersections_remain)
{
    namespace PMP = CGAL::Polygon_mesh_processing;

    if (self_intersections_remain)
        *self_intersections_remain = false;
    if (mesh.empty())
        return true;

    try {
        // 1) libmeshfix: weld, degenerate and duplicate faces, T-junctions, orientation, non-manifold edges and
        // vertices, holes, islands, cavities, and a check for self-intersections.
        indexed_triangle_set its;
        meshfix_report       report;
        if (!meshfix_repair_its(mesh.its, its, report, error))
            return false;
        if (its.indices.empty() || its_volume(its) <= EPSILON) {
            if (error)
                *error = "Repair failed: the mesh does not enclose a volume.";
            return false;
        }
        if (!report.watertight_after) {
            if (error)
                *error = "Repair failed: mesh still open after hole filling.";
            return false;
        }

        // 2) Orca: The exact self-intersection test stays CGAL's (the library's check only finds faces that pierce each
        // other, not coplanar overlaps). Overlapping shells become one solid, see resolve_overlapping_shells(), which
        // leaves the self-intersections it cannot resolve.
        _EpicMesh cgal_mesh = its_to_surface_mesh(its);
        if (!CGAL::is_closed(cgal_mesh)) {
            if (error)
                *error = "Repair failed: mesh still open after hole filling.";
            return false;
        }
        bool self_intersecting = PMP::does_self_intersect(cgal_mesh);
        if (self_intersecting) {
            resolve_overlapping_shells(cgal_mesh);
            self_intersecting = PMP::does_self_intersect(cgal_mesh);
            its               = cgal_to_indexed_triangle_set(cgal_mesh);
            if (its_volume(its) <= EPSILON) {
                if (error)
                    *error = "Repair failed: the mesh does not enclose a volume.";
                return false;
            }
        }

        RepairedMeshErrors errs{};
        const auto         saturate = [](uint64_t n) { return int(std::min<uint64_t>(n, uint64_t(std::numeric_limits<int>::max()))); };
        errs.degenerate_facets = saturate(report.degenerate_faces_removed);
        errs.facets_removed    = saturate(report.invalid_faces_removed + report.degenerate_faces_removed + report.duplicate_faces_removed +
                                       report.small_components_removed + report.flat_components_removed);
        errs.facets_reversed   = saturate(report.faces_flipped);
        errs.edges_fixed       = saturate(report.vertices_merged + report.t_junctions_fixed + report.non_manifold_edges_before);

        mesh = TriangleMesh(std::move(its), errs);

        if (repaired_errors)
            *repaired_errors = errs;
        if (self_intersections_remain)
            *self_intersections_remain = self_intersecting;

        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace

bool repair(TriangleMesh& mesh, RepairedMeshErrors* repaired_errors, std::string* error, bool* self_intersections_remain)
{
    // Orca: CGAL may crash on the meshes of arbitrary files. Trapped as the mesh booleans above do, which works where
    // try_catch_signal() traps signals (Windows); elsewhere it only runs the repair.
    bool success = false;
    bool crashed = false;
    if (self_intersections_remain)
        *self_intersections_remain = false;
    try_catch_signal({SIGSEGV, SIGFPE}, [&success, &mesh, repaired_errors, error, self_intersections_remain] {
        success = repair_unguarded(mesh, repaired_errors, error, self_intersections_remain);
    }, [&crashed] { crashed = true; });
    if (crashed) {
        if (error)
            *error = "Repair failed: CGAL crashed.";
        return false;
    }
    return success;
}

CGALMeshPtr clone(const CGALMesh &m)
{
    return CGALMeshPtr{new CGALMesh{m}};
}

} // namespace cgal


namespace mcut {
/* BBS: MusangKing
 * mcut mesh array format for Boolean Opts calculation
 */
struct McutMesh
{
    // variables for mesh data in a format suited for mcut
    std::vector<uint32_t> faceSizesArray;
    std::vector<uint32_t> faceIndicesArray;
    std::vector<double>   vertexCoordsArray;
};
void McutMeshDeleter::operator()(McutMesh *ptr) { delete ptr; }

bool empty(const McutMesh &mesh) { return mesh.vertexCoordsArray.empty() || mesh.faceIndicesArray.empty(); }
void triangle_mesh_to_mcut(const TriangleMesh &src_mesh, McutMesh &srcMesh, const Transform3d &src_nm = Transform3d::Identity())
{
    // vertices precision convention and copy
    srcMesh.vertexCoordsArray.reserve(src_mesh.its.vertices.size() * 3);
    for (int i = 0; i < src_mesh.its.vertices.size(); ++i) {
        const Vec3d v = src_nm * src_mesh.its.vertices[i].cast<double>();
        srcMesh.vertexCoordsArray.push_back(v[0]);
        srcMesh.vertexCoordsArray.push_back(v[1]);
        srcMesh.vertexCoordsArray.push_back(v[2]);
    }

    // faces copy
    srcMesh.faceIndicesArray.reserve(src_mesh.its.indices.size() * 3);
    srcMesh.faceSizesArray.reserve(src_mesh.its.indices.size());
    for (int i = 0; i < src_mesh.its.indices.size(); ++i) {
        const int &f0 = src_mesh.its.indices[i][0];
        const int &f1 = src_mesh.its.indices[i][1];
        const int &f2 = src_mesh.its.indices[i][2];
        srcMesh.faceIndicesArray.push_back(f0);
        srcMesh.faceIndicesArray.push_back(f1);
        srcMesh.faceIndicesArray.push_back(f2);

        srcMesh.faceSizesArray.push_back((uint32_t) 3);
    }
}

McutMeshPtr triangle_mesh_to_mcut(const indexed_triangle_set &M)
{
    std::unique_ptr<McutMesh, McutMeshDeleter> out(new McutMesh{});
    TriangleMesh                               trimesh(M);
    triangle_mesh_to_mcut(trimesh, *out.get());
    return out;
}

TriangleMesh mcut_to_triangle_mesh(const McutMesh &mcutmesh)
{
    uint32_t ccVertexCount = mcutmesh.vertexCoordsArray.size() / 3;
    auto    &ccVertices    = mcutmesh.vertexCoordsArray;
    auto    &ccFaceIndices = mcutmesh.faceIndicesArray;
    auto    &faceSizes     = mcutmesh.faceSizesArray;
    uint32_t ccFaceCount   = faceSizes.size();
    // rearrange vertices/faces and save into result mesh
    std::vector<Vec3f> vertices(ccVertexCount);
    for (uint32_t i = 0; i < ccVertexCount; i++) {
        vertices[i][0] = (float) ccVertices[(uint64_t) i * 3 + 0];
        vertices[i][1] = (float) ccVertices[(uint64_t) i * 3 + 1];
        vertices[i][2] = (float) ccVertices[(uint64_t) i * 3 + 2];
    }

    // output faces
    int faceVertexOffsetBase = 0;

    // for each face in CC
    std::vector<Vec3i32> faces(ccFaceCount);
    for (uint32_t f = 0; f < ccFaceCount; ++f) {
        int faceSize = faceSizes.at(f);

        // for each vertex in face
        for (int v = 0; v < faceSize; v++) { faces[f][v] = ccFaceIndices[(uint64_t) faceVertexOffsetBase + v]; }
        faceVertexOffsetBase += faceSize;
    }

    TriangleMesh out(vertices, faces);
    return out;
}

void merge_mcut_meshes(McutMesh& src, const McutMesh& cut) {
    indexed_triangle_set all_its;
    TriangleMesh tri_src = mcut_to_triangle_mesh(src);
    TriangleMesh tri_cut = mcut_to_triangle_mesh(cut);
    its_merge(all_its, tri_src.its);
    its_merge(all_its, tri_cut.its);
    src = *triangle_mesh_to_mcut(all_its);
    }

MCAPI_ATTR void MCAPI_CALL mcDebugOutput(McDebugSource source,
    McDebugType type,
    unsigned int id,
    McDebugSeverity severity,
    size_t length,
    const char* message,
    const void* userParam)
{
    BOOST_LOG_TRIVIAL(debug)<<Slic3r::format("mcut mcDebugOutput message ( %d ): %s ", id, message);

    switch (source) {
        case MC_DEBUG_SOURCE_API:
            BOOST_LOG_TRIVIAL(debug)<<("Source: API");
            break;
        case MC_DEBUG_SOURCE_KERNEL:
            BOOST_LOG_TRIVIAL(debug)<<("Source: Kernel");
            break;
        }

    switch (type) {
        case MC_DEBUG_TYPE_ERROR:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Error");
            break;
        case MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Deprecated Behaviour");
            break;
        case MC_DEBUG_TYPE_OTHER:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Other");
            break;
        }

    switch (severity) {
        case MC_DEBUG_SEVERITY_HIGH:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: high");
            break;
        case MC_DEBUG_SEVERITY_MEDIUM:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: medium");
            break;
        case MC_DEBUG_SEVERITY_LOW:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: low");
            break;
        case MC_DEBUG_SEVERITY_NOTIFICATION:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: notification");
            break;
        }
}


bool do_boolean_single(McutMesh &srcMesh, const McutMesh &cutMesh, const std::string &boolean_opts)
{
    // create context
    McContext context = MC_NULL_HANDLE;
    McResult  err     = mcCreateContext(&context, 0);
    // add debug callback according to https://cutdigital.github.io/mcut.site/tutorials/debugging/
    mcDebugMessageCallback(context, mcDebugOutput, nullptr);
    mcDebugMessageControl(
        context,
        MC_DEBUG_SOURCE_ALL,
        MC_DEBUG_TYPE_ERROR,
        MC_DEBUG_SEVERITY_MEDIUM,
        true);
    // We can either let MCUT compute all possible meshes (including patches etc.), or we can
    // constrain the library to compute exactly the boolean op mesh we want. This 'constrained' case
    // is done with the following flags.
    // NOTE#1: you can extend these flags by bitwise ORing with additional flags (see `McDispatchFlags' in mcut.h)
    // NOTE#2: below order of columns MATTERS
    const std::map<std::string, McFlags> booleanOpts = {
        {"A_NOT_B", MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE},
        {"B_NOT_A", MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW},
        {"UNION", MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE},
        {"INTERSECTION", MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW},
    };

    std::map<std::string, McFlags>::const_iterator it          = booleanOpts.find(boolean_opts);
    McFlags                                        boolOpFlags = it->second;

    if (srcMesh.vertexCoordsArray.empty() && (boolean_opts == "UNION" || boolean_opts == "B_NOT_A")) {
        srcMesh = cutMesh;
        mcReleaseContext(context);
        return true;
    }

    err = mcDispatch(context,
                     MC_DISPATCH_VERTEX_ARRAY_DOUBLE |          // vertices are in array of doubles
                         MC_DISPATCH_ENFORCE_GENERAL_POSITION | // perturb if necessary
                         boolOpFlags,                           // filter flags which specify the type of output we want
                     // source mesh
                     reinterpret_cast<const void *>(srcMesh.vertexCoordsArray.data()), reinterpret_cast<const uint32_t *>(srcMesh.faceIndicesArray.data()),
                     srcMesh.faceSizesArray.data(), static_cast<uint32_t>(srcMesh.vertexCoordsArray.size() / 3), static_cast<uint32_t>(srcMesh.faceSizesArray.size()),
                     // cut mesh
                     reinterpret_cast<const void *>(cutMesh.vertexCoordsArray.data()), cutMesh.faceIndicesArray.data(), cutMesh.faceSizesArray.data(),
                     static_cast<uint32_t>(cutMesh.vertexCoordsArray.size() / 3), static_cast<uint32_t>(cutMesh.faceSizesArray.size()));
    if (err != MC_NO_ERROR) {
        BOOST_LOG_TRIVIAL(debug) << "MCUT mcDispatch fails! err=" << err;
        mcReleaseContext(context);
        if (boolean_opts == "UNION") {
            merge_mcut_meshes(srcMesh, cutMesh);
            return true;
        }
        return false;
    }

    // query the number of available connected component
    uint32_t numConnComps;
    err = mcGetConnectedComponents(context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, 0, NULL, &numConnComps);
    if (err != MC_NO_ERROR || numConnComps==0) {
        BOOST_LOG_TRIVIAL(debug) << "MCUT mcGetConnectedComponents fails! err=" << err << ", numConnComps" << numConnComps;
        mcReleaseContext(context);
        if (numConnComps == 0 && boolean_opts == "UNION") {
            merge_mcut_meshes(srcMesh, cutMesh);
            return true;
        }
        return false;
    }

    std::vector<McConnectedComponent> connectedComponents(numConnComps, MC_NULL_HANDLE);
    err = mcGetConnectedComponents(context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, (uint32_t) connectedComponents.size(), connectedComponents.data(), NULL);

    McutMesh outMesh;
    int N_vertices = 0;
    // traversal of all connected components
    for (int n = 0; n < numConnComps; ++n) {
        // query the data of each connected component from MCUT
        McConnectedComponent connComp = connectedComponents[n];

        // query the vertices
        McSize numBytes                  = 0;
        err                               = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, 0, NULL, &numBytes);
        uint32_t            ccVertexCount = (uint32_t) (numBytes / (sizeof(double) * 3));
        std::vector<double> ccVertices((uint64_t) ccVertexCount * 3u, 0);
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, numBytes, (void *) ccVertices.data(), NULL);

        // query the faces
        numBytes = 0;
        err      = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, 0, NULL, &numBytes);
        std::vector<uint32_t> ccFaceIndices(numBytes / sizeof(uint32_t), 0);
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, numBytes, ccFaceIndices.data(), NULL);
        std::vector<uint32_t> faceSizes(ccFaceIndices.size() / 3, 3);

        const uint32_t ccFaceCount = static_cast<uint32_t>(faceSizes.size());

        // Here we show, how to know when connected components, pertain particular boolean operations.
        McPatchLocation patchLocation = (McPatchLocation) 0;
        err                           = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION, sizeof(McPatchLocation), &patchLocation, NULL);

        McFragmentLocation fragmentLocation = (McFragmentLocation) 0;
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION, sizeof(McFragmentLocation), &fragmentLocation, NULL);

        outMesh.vertexCoordsArray.insert(outMesh.vertexCoordsArray.end(), ccVertices.begin(), ccVertices.end());

        // add offset to face index
        for (size_t i = 0; i < ccFaceIndices.size(); i++) {
            ccFaceIndices[i] += N_vertices;
        }

        int faceVertexOffsetBase = 0;

        // for each face in CC
        std::vector<Vec3i32> faces(ccFaceCount);
        for (uint32_t f = 0; f < ccFaceCount; ++f) {
            bool reverseWindingOrder = (fragmentLocation == MC_FRAGMENT_LOCATION_BELOW) && (patchLocation == MC_PATCH_LOCATION_OUTSIDE);
            int  faceSize            = faceSizes.at(f);
            if (reverseWindingOrder) {
                std::vector<uint32_t> faceIndex(faceSize);
                // for each vertex in face
                for (int v = faceSize - 1; v >= 0; v--) { faceIndex[v] = ccFaceIndices[(uint64_t) faceVertexOffsetBase + v]; }
                std::copy(faceIndex.begin(), faceIndex.end(), ccFaceIndices.begin() + faceVertexOffsetBase);
            }
            faceVertexOffsetBase += faceSize;
        }

        outMesh.faceIndicesArray.insert(outMesh.faceIndicesArray.end(), ccFaceIndices.begin(), ccFaceIndices.end());
        outMesh.faceSizesArray.insert(outMesh.faceSizesArray.end(), faceSizes.begin(), faceSizes.end());

        N_vertices += ccVertexCount;
    }

    // free connected component data
    err = mcReleaseConnectedComponents(context, 0, NULL);
    // destroy context
    err = mcReleaseContext(context);

    srcMesh = outMesh;

    return true;
}

void do_boolean(McutMesh& srcMesh, const McutMesh& cutMesh, const std::string& boolean_opts)
{
    TriangleMesh tri_src = mcut_to_triangle_mesh(srcMesh);
    std::vector<indexed_triangle_set> src_parts = its_split(tri_src.its);

    TriangleMesh tri_cut = mcut_to_triangle_mesh(cutMesh);
    std::vector<indexed_triangle_set> cut_parts = its_split(tri_cut.its);

    if (src_parts.empty() && boolean_opts == "UNION") {
        srcMesh = cutMesh;
        return;
    }
    if(cut_parts.empty()) return;

    // when src mesh has multiple connected components, mcut refuses to work.
    // But we can force it to work by spliting the src mesh into disconnected components,
    // and do booleans seperately, then merge all the results.
    indexed_triangle_set all_its;
    if (boolean_opts == "UNION" || boolean_opts == "A_NOT_B") {
        for (size_t i = 0; i < src_parts.size(); i++) {
            auto src_part = triangle_mesh_to_mcut(src_parts[i]);
            for (size_t j = 0; j < cut_parts.size(); j++) {
                auto cut_part = triangle_mesh_to_mcut(cut_parts[j]);
                do_boolean_single(*src_part, *cut_part, boolean_opts);
            }
            TriangleMesh tri_part = mcut_to_triangle_mesh(*src_part);
            its_merge(all_its, tri_part.its);
        }
    }
    else if (boolean_opts == "INTERSECTION") {
        for (size_t i = 0; i < src_parts.size(); i++) {
            for (size_t j = 0; j < cut_parts.size(); j++) {
                auto src_part = triangle_mesh_to_mcut(src_parts[i]);
                auto cut_part = triangle_mesh_to_mcut(cut_parts[j]);
                bool success = do_boolean_single(*src_part, *cut_part, boolean_opts);
                if (success) {
                    TriangleMesh tri_part = mcut_to_triangle_mesh(*src_part);
                    its_merge(all_its, tri_part.its);
                }
            }
        }
    }
    srcMesh = *triangle_mesh_to_mcut(all_its);
}

void make_boolean(const TriangleMesh &src_mesh, const TriangleMesh &cut_mesh, std::vector<TriangleMesh> &dst_mesh, const std::string &boolean_opts)
{
    McutMesh srcMesh, cutMesh;
    triangle_mesh_to_mcut(src_mesh, srcMesh);
    triangle_mesh_to_mcut(cut_mesh, cutMesh);
    //dst_mesh = make_boolean(srcMesh, cutMesh, boolean_opts);
    do_boolean(srcMesh, cutMesh, boolean_opts);
    TriangleMesh tri_src = mcut_to_triangle_mesh(srcMesh);
    if (!tri_src.empty())
        dst_mesh.push_back(std::move(tri_src));
}

} // namespace mcut


} // namespace MeshBoolean
} // namespace Slic3r
