// Edge tables, topology analysis, orientation and volume. SPDX-License-Identifier: MIT
#include <algorithm>
#include <numeric>

#include "core.hpp"

namespace meshfix {

std::vector<EdgeRec> build_edges(const Mesh &m) {
    std::vector<EdgeRec> recs;
    recs.reserve(m.f.size() * 3);
    for (uint32_t f = 0; f < m.f.size(); ++f)
        for (uint32_t i = 0; i < 3; ++i)
            recs.push_back({edge_key(m.f[f][i], m.f[f][(i + 1) % 3]), 3 * f + i});
    std::sort(recs.begin(), recs.end(), [](const EdgeRec &a, const EdgeRec &b) {
        return a.key != b.key ? a.key < b.key : a.id < b.id;
    });
    return recs;
}

namespace {

struct UnionFind {
    std::vector<uint32_t> p;
    explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (p[x] != x) {
            p[x] = p[p[x]];
            x = p[x];
        }
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (a < b)
            p[b] = a; // the smaller index stays the root: deterministic labels
        else
            p[a] = b;
    }
};

bool forward(const Mesh &m, uint32_t id) { return corner_from(m, id) < corner_to(m, id); }

} // namespace

std::vector<uint32_t> pair_manifold_edges(const Mesh &m, const std::vector<EdgeRec> &recs) {
    std::vector<uint32_t> partner(m.f.size() * 3, kNone);
    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].key == recs[i].key)
            ++j;
        if (j - i == 2 && recs[i].id / 3 != recs[i + 1].id / 3) {
            partner[recs[i].id] = recs[i + 1].id;
            partner[recs[i + 1].id] = recs[i].id;
        }
        i = j;
    }
    return partner;
}

// Corner (f, i) and its partner's matching corner share a vertex; corners joined through paired edges
// form one fan. Every fan becomes its own vertex.
uint32_t fan_classes(const Mesh &m, const std::vector<uint32_t> &partner, std::vector<uint32_t> &cls) {
    const size_t n = m.f.size() * 3;
    UnionFind uf(n);
    for (uint32_t c = 0; c < n; ++c) {
        const uint32_t p = partner[c];
        if (p == kNone || p < c)
            continue;
        const uint32_t f = c / 3, i = c % 3, g = p / 3, j = p % 3;
        const uint32_t u = m.f[f][i];
        if (m.f[g][j] == u) {
            uf.unite(c, p);
            uf.unite(3 * f + (i + 1) % 3, 3 * g + (j + 1) % 3);
        } else {
            uf.unite(c, 3 * g + (j + 1) % 3);
            uf.unite(3 * f + (i + 1) % 3, p);
        }
    }
    std::vector<uint32_t> root_class(n, kNone);
    cls.assign(n, 0);
    uint32_t count = 0;
    for (uint32_t c = 0; c < n; ++c) {
        const uint32_t r = uf.find(c);
        if (root_class[r] == kNone)
            root_class[r] = count++;
        cls[c] = root_class[r];
    }
    return count;
}

uint64_t split_fans(Mesh &m, const std::vector<uint32_t> &partner) {
    std::vector<uint32_t> cls;
    const uint32_t count = fan_classes(m, partner, cls);
    std::vector<Vec3> nv(count);
    std::vector<uint8_t> used(m.v.size(), 0);
    uint64_t used_count = 0;
    for (uint32_t c = 0; c < cls.size(); ++c) {
        const uint32_t old = m.f[c / 3][c % 3];
        nv[cls[c]] = m.v[old];
        if (!used[old]) {
            used[old] = 1;
            ++used_count;
        }
        m.f[c / 3][c % 3] = cls[c];
    }
    m.v = std::move(nv);
    return count - used_count;
}

Topology analyze_topology(const Mesh &m) {
    Topology t;
    if (m.f.empty())
        return t;
    const auto recs = build_edges(m);
    auto partner = pair_manifold_edges(m, recs);
    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].key == recs[i].key)
            ++j;
        const size_t k = j - i;
        if (k == 1)
            ++t.boundary_edges;
        else if (k > 2)
            ++t.non_manifold_edges;
        else if (forward(m, recs[i].id) == forward(m, recs[i + 1].id))
            ++t.inconsistent_edges;
        i = j;
    }
    std::vector<uint32_t> cls;
    const uint32_t fans = fan_classes(m, partner, cls);
    std::vector<uint8_t> used(m.v.size(), 0);
    uint64_t used_count = 0;
    for (const Tri &f : m.f)
        for (uint32_t x : f)
            if (!used[x]) {
                used[x] = 1;
                ++used_count;
            }
    t.non_manifold_vertices = fans - used_count;
    std::vector<uint32_t> comp;
    t.components = label_components(m, comp);
    t.manifold = t.non_manifold_edges == 0 && t.non_manifold_vertices == 0;
    t.watertight = t.boundary_edges == 0 && t.non_manifold_edges == 0 && t.inconsistent_edges == 0;
    return t;
}

uint32_t label_components(const Mesh &m, std::vector<uint32_t> &comp) {
    const size_t F = m.f.size();
    UnionFind uf(F);
    const auto recs = build_edges(m);
    for (size_t i = 1; i < recs.size(); ++i)
        if (recs[i].key == recs[i - 1].key)
            uf.unite(recs[i].id / 3, recs[i - 1].id / 3);
    comp.assign(F, 0);
    std::vector<uint32_t> root_label(F, kNone);
    uint32_t count = 0;
    for (uint32_t f = 0; f < F; ++f) {
        const uint32_t r = uf.find(f);
        if (root_label[r] == kNone)
            root_label[r] = count++;
        comp[f] = root_label[r];
    }
    return count;
}

uint64_t orient_consistent(Mesh &m, bool outward) {
    const size_t F = m.f.size();
    const auto recs = build_edges(m);
    const auto partner = pair_manifold_edges(m, recs);
    std::vector<int8_t> flip(F, -1);
    std::vector<uint32_t> stack, members;
    uint64_t flipped = 0;
    for (uint32_t root = 0; root < F; ++root) {
        if (flip[root] >= 0)
            continue;
        flip[root] = 0;
        members.clear();
        stack.assign(1, root);
        while (!stack.empty()) {
            const uint32_t f = stack.back();
            stack.pop_back();
            members.push_back(f);
            for (uint32_t i = 0; i < 3; ++i) {
                const uint32_t p = partner[3 * f + i];
                if (p == kNone)
                    continue;
                const uint32_t g = p / 3;
                if (flip[g] >= 0)
                    continue;
                const bool ff = forward(m, 3 * f + i), fg = forward(m, p);
                flip[g] = int8_t(fg ^ ff ^ (flip[f] != 0) ^ true);
                stack.push_back(g);
            }
        }
        size_t flips = 0;
        for (uint32_t f : members)
            flips += flip[f] != 0;
        const bool invert = flips * 2 > members.size();
        size_t patch_flipped = 0;
        for (uint32_t f : members) {
            const bool doflip = (flip[f] != 0) != invert;
            if (doflip) {
                std::swap(m.f[f][1], m.f[f][2]);
                ++patch_flipped;
            }
        }
        if (outward && signed_volume_of(m, members) < 0) {
            for (uint32_t f : members)
                std::swap(m.f[f][1], m.f[f][2]);
            patch_flipped = members.size() - patch_flipped;
        }
        flipped += patch_flipped;
    }
    return flipped;
}

double signed_volume_of(const Mesh &m, const std::vector<uint32_t> &faces) {
    if (faces.empty())
        return 0;
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (uint32_t fi : faces)
        for (uint32_t x : m.f[fi]) {
            const Vec3 &p = m.v[x];
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    const Vec3 o = (lo + hi) * 0.5;
    double vol = 0;
    for (uint32_t fi : faces) {
        const Tri &t = m.f[fi];
        vol += dot(m.v[t[0]] - o, cross(m.v[t[1]] - o, m.v[t[2]] - o));
    }
    return vol / 6.0;
}

double signed_volume(const Mesh &m) {
    std::vector<uint32_t> all(m.f.size());
    std::iota(all.begin(), all.end(), 0u);
    return signed_volume_of(m, all);
}

void compact_vertices(Mesh &m) {
    std::vector<uint32_t> remap(m.v.size(), kNone);
    for (const Tri &f : m.f)
        for (uint32_t x : f)
            remap[x] = 0;
    std::vector<Vec3> nv;
    nv.reserve(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i)
        if (remap[i] != kNone) {
            remap[i] = uint32_t(nv.size());
            nv.push_back(m.v[i]);
        }
    for (Tri &f : m.f)
        for (uint32_t &x : f)
            x = remap[x];
    m.v = std::move(nv);
}

} // namespace meshfix
