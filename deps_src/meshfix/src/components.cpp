// Shell (connected component) processing: islands, zero-volume shells, outward orientation, cavities.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

#include "core.hpp"

namespace meshfix {

namespace {

struct Box {
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    void add(const Vec3 &p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    double diag() const { return norm(hi - lo); }
    bool strictly_inside(const Box &o) const { // this inside o
        return lo.x > o.lo.x && lo.y > o.lo.y && lo.z > o.lo.z && hi.x < o.hi.x && hi.y < o.hi.y && hi.z < o.hi.z;
    }
};

struct Shell {
    std::vector<uint32_t> faces;
    Box box;
    uint64_t boundary_edges = 0;
    double volume = 0;
    bool removed = false;
    bool closed() const { return boundary_edges == 0; }
};

// Parity of the crossings of a +x ray with the faces of one closed shell. The ray origin is nudged by an
// irrational fraction of the shell size, so that it practically never meets an edge exactly.
class RayCaster {
  public:
    RayCaster(const Mesh &m, const Shell &s) : m_(m), shell_(s) {
        const Box &b = s.box;
        const double d = b.diag();
        eps_y_ = d * 1.2345678e-9;
        eps_z_ = d * 7.8901234e-10;
        grid_ = std::clamp<int>(int(std::sqrt(double(s.faces.size()))), 1, 512);
        lo_y_ = b.lo.y;
        lo_z_ = b.lo.z;
        inv_y_ = grid_ / std::max(b.hi.y - b.lo.y, 1e-300);
        inv_z_ = grid_ / std::max(b.hi.z - b.lo.z, 1e-300);
        cells_.assign(size_t(grid_) * grid_, {});
        for (uint32_t fi : s.faces) {
            const Tri &t = m.f[fi];
            double y0 = 1e300, y1 = -1e300, z0 = 1e300, z1 = -1e300;
            for (uint32_t x : t) {
                y0 = std::min(y0, m.v[x].y);
                y1 = std::max(y1, m.v[x].y);
                z0 = std::min(z0, m.v[x].z);
                z1 = std::max(z1, m.v[x].z);
            }
            for (int cy = cell_y(y0); cy <= cell_y(y1); ++cy)
                for (int cz = cell_z(z0); cz <= cell_z(z1); ++cz)
                    cells_[size_t(cy) * grid_ + cz].push_back(fi);
        }
    }
    bool inside(const Vec3 &p) const {
        const double py = p.y + eps_y_, pz = p.z + eps_z_;
        if (py < shell_.box.lo.y || py > shell_.box.hi.y || pz < shell_.box.lo.z || pz > shell_.box.hi.z || p.x > shell_.box.hi.x)
            return false;
        int crossings = 0;
        for (uint32_t fi : cells_[size_t(cell_y(py)) * grid_ + cell_z(pz)]) {
            const Tri &t = m_.f[fi];
            const Vec3 &a = m_.v[t[0]], &b = m_.v[t[1]], &c = m_.v[t[2]];
            // Barycentric coordinates of (py, pz) in the yz projection.
            const double det = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
            if (det == 0)
                continue;
            const double u = ((py - a.y) * (c.z - a.z) - (pz - a.z) * (c.y - a.y)) / det;
            const double v = ((b.y - a.y) * (pz - a.z) - (b.z - a.z) * (py - a.y)) / det;
            if (u < 0 || v < 0 || u + v > 1)
                continue;
            const double x = a.x + u * (b.x - a.x) + v * (c.x - a.x);
            if (x > p.x)
                ++crossings;
        }
        return (crossings & 1) != 0;
    }

  private:
    int cell_y(double y) const { return std::clamp(int((y - lo_y_) * inv_y_), 0, grid_ - 1); }
    int cell_z(double z) const { return std::clamp(int((z - lo_z_) * inv_z_), 0, grid_ - 1); }
    const Mesh &m_;
    const Shell &shell_;
    double eps_y_, eps_z_, lo_y_, lo_z_, inv_y_, inv_z_;
    int grid_;
    std::vector<std::vector<uint32_t>> cells_;
};

// Vertices of the shell that stand for it in the containment test: its first one and the extremes on every axis.
std::vector<Vec3> probes(const Mesh &m, const Shell &s) {
    std::vector<Vec3> out;
    const Vec3 *ext[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    for (uint32_t fi : s.faces)
        for (uint32_t x : m.f[fi]) {
            const Vec3 &p = m.v[x];
            if (!ext[0] || p.x < ext[0]->x) ext[0] = &p;
            if (!ext[1] || p.x > ext[1]->x) ext[1] = &p;
            if (!ext[2] || p.y < ext[2]->y) ext[2] = &p;
            if (!ext[3] || p.y > ext[3]->y) ext[3] = &p;
            if (!ext[4] || p.z < ext[4]->z) ext[4] = &p;
            if (!ext[5] || p.z > ext[5]->z) ext[5] = &p;
        }
    out.push_back(m.v[m.f[s.faces[0]][0]]);
    for (const Vec3 *p : ext)
        if (p)
            out.push_back(*p);
    return out;
}

void flip_shell(Mesh &m, const Shell &s) {
    for (uint32_t fi : s.faces)
        std::swap(m.f[fi][1], m.f[fi][2]);
}

} // namespace

void process_components(Mesh &m, const Ctx &ctx) {
    if (m.f.empty())
        return;
    std::vector<uint32_t> comp;
    const uint32_t count = label_components(m, comp);
    std::vector<Shell> shells(count);
    for (uint32_t f = 0; f < m.f.size(); ++f) {
        shells[comp[f]].faces.push_back(f);
        for (uint32_t x : m.f[f])
            shells[comp[f]].box.add(m.v[x]);
    }
    {
        const auto recs = build_edges(m);
        for (size_t i = 0; i < recs.size();) {
            size_t j = i + 1;
            while (j < recs.size() && recs[j].key == recs[i].key)
                ++j;
            if (j - i != 2)
                shells[comp[recs[i].id / 3]].boundary_edges += 1;
            i = j;
        }
    }
    for (Shell &s : shells)
        s.volume = signed_volume_of(m, s.faces);

    if (ctx.on(MESHFIX_FIX_COMPONENTS)) {
        for (Shell &s : shells) {
            if (s.faces.size() < ctx.opt.min_component_faces) {
                s.removed = true;
                ++ctx.rep.small_components_removed;
            } else {
                const double d = s.box.diag();
                if (s.closed() && std::fabs(s.volume) <= 1e-9 * d * d * d) {
                    s.removed = true;
                    ++ctx.rep.flat_components_removed;
                }
            }
        }
    }

    if (ctx.on(MESHFIX_FIX_ORIENTATION)) {
        std::vector<uint32_t> alive;
        for (uint32_t i = 0; i < count; ++i)
            if (!shells[i].removed)
                alive.push_back(i);
        // depth[i]: the length of the longest chain of shells containing each other around shell i.
        std::vector<uint32_t> depth(count, 0);
        if (ctx.on(MESHFIX_FIX_NESTED_SHELLS) && alive.size() > 1) {
            std::vector<std::vector<uint32_t>> containers(count);
            std::vector<std::unique_ptr<RayCaster>> casters(count);
            for (uint32_t j : alive) {
                if (!shells[j].closed())
                    continue;
                for (uint32_t i : alive) {
                    if (i == j || !shells[i].box.strictly_inside(shells[j].box))
                        continue;
                    if (!casters[j])
                        casters[j] = std::make_unique<RayCaster>(m, shells[j]);
                    bool all_inside = true;
                    for (const Vec3 &p : probes(m, shells[i]))
                        if (!casters[j]->inside(p)) {
                            all_inside = false;
                            break;
                        }
                    if (all_inside)
                        containers[i].push_back(j);
                }
            }
            // A shell's containers also contain its containers, so they have fewer containers and get their depth first.
            std::vector<uint32_t> order = alive;
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return containers[a].size() < containers[b].size(); });
            for (uint32_t i : order)
                for (uint32_t j : containers[i])
                    depth[i] = std::max(depth[i], depth[j] + 1);
        }
        for (uint32_t i : alive) {
            Shell &s = shells[i];
            const double d = s.box.diag();
            const bool cavity = depth[i] % 2 == 1;
            if (cavity)
                ++ctx.rep.cavity_shells;
            // An open shell has no well-defined volume; trust its sign only when it clearly dominates.
            if (std::fabs(s.volume) <= 1e-12 * d * d * d)
                continue;
            if ((s.volume < 0) != cavity) {
                flip_shell(m, s);
                ctx.rep.faces_flipped += s.faces.size();
            }
        }
    }

    size_t out = 0;
    std::vector<uint8_t> drop(m.f.size(), 0);
    for (const Shell &s : shells)
        if (s.removed)
            for (uint32_t f : s.faces)
                drop[f] = 1;
    for (size_t f = 0; f < m.f.size(); ++f)
        if (!drop[f])
            m.f[out++] = m.f[f];
    m.f.resize(out);
}

} // namespace meshfix
