// Vertex welding, degenerate / duplicate face removal and T-junction stitching.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core.hpp"

namespace meshfix {

namespace {

struct Cell {
    int64_t x, y, z;
    bool operator==(const Cell &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    size_t operator()(const Cell &c) const {
        // Unsigned so the multiplications wrap instead of overflowing.
        return size_t(uint64_t(c.x) * 73856093u ^ uint64_t(c.y) * 19349663u ^ uint64_t(c.z) * 83492791u);
    }
};
Cell cell_of(const Vec3 &p, double h) {
    return {int64_t(std::floor(p.x / h)), int64_t(std::floor(p.y / h)), int64_t(std::floor(p.z / h))};
}

} // namespace

uint64_t weld_vertices(Mesh &m, double tol) {
    const size_t nv = m.v.size();
    std::vector<uint8_t> used(nv, 0);
    for (const Tri &f : m.f)
        for (uint32_t x : f)
            used[x] = 1;
    std::vector<uint32_t> idx;
    idx.reserve(nv);
    for (uint32_t i = 0; i < nv; ++i)
        if (used[i])
            idx.push_back(i);
    const uint64_t used_count = idx.size();

    // 1. Exactly equal coordinates (-0 == +0): the smallest index of each run represents it.
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        const Vec3 &p = m.v[a], &q = m.v[b];
        if (p.x != q.x) return p.x < q.x;
        if (p.y != q.y) return p.y < q.y;
        if (p.z != q.z) return p.z < q.z;
        return a < b;
    });
    std::vector<uint32_t> rep(nv, kNone);
    for (size_t i = 0; i < idx.size();) {
        size_t j = i + 1;
        const Vec3 &p = m.v[idx[i]];
        while (j < idx.size() && m.v[idx[j]].x == p.x && m.v[idx[j]].y == p.y && m.v[idx[j]].z == p.z)
            ++j;
        for (size_t k = i; k < j; ++k)
            rep[idx[k]] = idx[i];
        i = j;
    }

    // 2. Points closer than tol, in ascending index order: the first point of a cluster represents it.
    std::vector<uint32_t> uniq;
    for (uint32_t i = 0; i < nv; ++i)
        if (used[i] && rep[i] == i)
            uniq.push_back(i);
    if (tol > 0 && !uniq.empty()) {
        double extent = 0;
        for (uint32_t i : uniq) {
            const Vec3 &p = m.v[i];
            extent = std::max({extent, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
        }
        // Grid cells are int64 coordinates: skip where coord / tol would overflow them.
        if (std::isfinite(extent) && extent / tol < 1e15) {
            std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
            grid.reserve(uniq.size() * 2);
            const double tol_sq = tol * tol;
            for (uint32_t i : uniq) {
                const Vec3 &p = m.v[i];
                const Cell c = cell_of(p, tol);
                uint32_t found = kNone;
                for (int64_t dx = -1; dx <= 1 && found == kNone; ++dx)
                    for (int64_t dy = -1; dy <= 1 && found == kNone; ++dy)
                        for (int64_t dz = -1; dz <= 1 && found == kNone; ++dz) {
                            const auto it = grid.find({c.x + dx, c.y + dy, c.z + dz});
                            if (it == grid.end())
                                continue;
                            for (uint32_t j : it->second) {
                                const Vec3 d = m.v[j] - p;
                                if (dot(d, d) <= tol_sq) {
                                    found = j;
                                    break;
                                }
                            }
                        }
                if (found == kNone)
                    grid[c].push_back(i);
                else
                    rep[i] = found;
            }
        }
    }
    // Points whose exact-merge representative joined a cluster follow it.
    for (uint32_t i = 0; i < nv; ++i)
        if (used[i] && rep[i] != i)
            rep[i] = rep[rep[i]];

    // 3. Renumber the representatives in ascending order.
    std::vector<uint32_t> newid(nv, kNone);
    std::vector<Vec3> nvv;
    for (uint32_t i = 0; i < nv; ++i)
        if (used[i] && rep[i] == i) {
            newid[i] = uint32_t(nvv.size());
            nvv.push_back(m.v[i]);
        }
    for (Tri &f : m.f)
        for (uint32_t &x : f)
            x = newid[rep[x]];
    const uint64_t removed = used_count - nvv.size();
    m.v = std::move(nvv);
    return removed;
}

uint64_t remove_degenerate_faces(Mesh &m) {
    size_t out = 0;
    for (size_t i = 0; i < m.f.size(); ++i) {
        const Tri t = m.f[i];
        bool degenerate = t[0] == t[1] || t[1] == t[2] || t[0] == t[2];
        if (!degenerate) {
            const Vec3 &a = m.v[t[0]], &b = m.v[t[1]], &c = m.v[t[2]];
            const Vec3 ab = b - a, ac = c - a, bc = c - b;
            const double longest_sq = std::max({dot(ab, ab), dot(ac, ac), dot(bc, bc)});
            // Twice the area against the longest edge squared: a sliver thinner than 1e-9 of its length.
            degenerate = norm(cross(ab, ac)) <= 1e-9 * longest_sq;
        }
        if (!degenerate)
            m.f[out++] = t;
    }
    const uint64_t removed = m.f.size() - out;
    m.f.resize(out);
    return removed;
}

uint64_t remove_duplicate_faces(Mesh &m) {
    struct Key {
        uint32_t a, b, c, face;
        bool positive;
    };
    std::vector<Key> keys;
    keys.reserve(m.f.size());
    for (uint32_t i = 0; i < m.f.size(); ++i) {
        Tri s = m.f[i];
        std::sort(s.begin(), s.end());
        const Tri &t = m.f[i];
        const bool positive = (t[0] == s[0] && t[1] == s[1]) || (t[0] == s[1] && t[1] == s[2]) || (t[0] == s[2] && t[1] == s[0]);
        keys.push_back({s[0], s[1], s[2], i, positive});
    }
    std::sort(keys.begin(), keys.end(), [](const Key &x, const Key &y) {
        if (x.a != y.a) return x.a < y.a;
        if (x.b != y.b) return x.b < y.b;
        if (x.c != y.c) return x.c < y.c;
        return x.face < y.face;
    });
    std::vector<uint8_t> keep(m.f.size(), 1);
    for (size_t i = 0; i < keys.size();) {
        size_t j = i + 1;
        while (j < keys.size() && keys[j].a == keys[i].a && keys[j].b == keys[i].b && keys[j].c == keys[i].c)
            ++j;
        if (j - i > 1) {
            int net = 0;
            uint32_t first_pos = kNone, first_neg = kNone;
            for (size_t k = i; k < j; ++k) {
                keep[keys[k].face] = 0;
                if (keys[k].positive) {
                    ++net;
                    if (first_pos == kNone) first_pos = keys[k].face;
                } else {
                    --net;
                    if (first_neg == kNone) first_neg = keys[k].face;
                }
            }
            // The same face twice keeps one; back-to-back pairs (a zero-thickness sheet) cancel.
            if (net > 0) keep[first_pos] = 1;
            else if (net < 0) keep[first_neg] = 1;
        }
        i = j;
    }
    size_t out = 0;
    for (size_t i = 0; i < m.f.size(); ++i)
        if (keep[i])
            m.f[out++] = m.f[i];
    const uint64_t removed = m.f.size() - out;
    m.f.resize(out);
    return removed;
}

uint64_t separate_coincident_vertices(Mesh &m, double distance) {
    if (distance <= 0)
        return 0;
    std::vector<uint32_t> face_count(m.v.size(), 0);
    std::vector<Vec3> normal(m.v.size());
    for (const Tri &t : m.f) {
        const Vec3 n = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]); // twice the area times the unit normal
        for (uint32_t x : t) {
            ++face_count[x];
            normal[x] = normal[x] + n;
        }
    }
    std::vector<uint32_t> idx;
    for (uint32_t i = 0; i < m.v.size(); ++i)
        if (face_count[i])
            idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        const Vec3 &p = m.v[a], &q = m.v[b];
        if (p.x != q.x) return p.x < q.x;
        if (p.y != q.y) return p.y < q.y;
        if (p.z != q.z) return p.z < q.z;
        return a < b;
    });
    uint64_t moved = 0;
    std::vector<std::pair<uint32_t, Vec3>> moves;
    for (size_t i = 0; i < idx.size();) {
        size_t j = i + 1;
        const Vec3 &p = m.v[idx[i]];
        while (j < idx.size() && m.v[idx[j]].x == p.x && m.v[idx[j]].y == p.y && m.v[idx[j]].z == p.z)
            ++j;
        if (j - i > 1) {
            size_t keep = i;
            for (size_t k = i + 1; k < j; ++k)
                if (face_count[idx[k]] > face_count[idx[keep]])
                    keep = k;
            for (size_t k = i; k < j; ++k) {
                if (k == keep)
                    continue;
                const double len = norm(normal[idx[k]]);
                if (len > 0)
                    moves.emplace_back(idx[k], m.v[idx[k]] - normal[idx[k]] * (distance / len));
            }
        }
        i = j;
    }
    for (const auto &mv : moves)
        m.v[mv.first] = mv.second;
    moved = moves.size();
    return moved;
}

uint64_t stitch_t_junctions(Mesh &m, double tol) {
    if (tol <= 0)
        return 0;
    uint64_t total = 0;
    for (int round = 0; round < 16; ++round) {
        const auto recs = build_edges(m);
        std::vector<uint32_t> boundary;
        for (size_t i = 0; i < recs.size();) {
            size_t j = i + 1;
            while (j < recs.size() && recs[j].key == recs[i].key)
                ++j;
            if (j - i == 1)
                boundary.push_back(recs[i].id);
            i = j;
        }
        if (boundary.empty())
            break;
        std::vector<uint32_t> bverts;
        double total_len = 0;
        for (uint32_t id : boundary) {
            bverts.push_back(corner_from(m, id));
            bverts.push_back(corner_to(m, id));
            total_len += norm(m.v[corner_to(m, id)] - m.v[corner_from(m, id)]);
        }
        std::sort(bverts.begin(), bverts.end());
        bverts.erase(std::unique(bverts.begin(), bverts.end()), bverts.end());
        const double h = std::max(total_len / double(boundary.size()), 4 * tol);
        std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
        for (uint32_t v : bverts)
            grid[cell_of(m.v[v], h)].push_back(v);

        struct Split {
            uint32_t face, corner;
            std::vector<std::pair<double, uint32_t>> pts;
        };
        std::vector<Split> splits;
        std::vector<uint8_t> touched(m.f.size(), 0);
        for (uint32_t id : boundary) {
            const uint32_t f = id / 3;
            if (touched[f])
                continue;
            const uint32_t a = corner_from(m, id), b = corner_to(m, id);
            const Vec3 &pa = m.v[a], &pb = m.v[b];
            const Vec3 ab = pb - pa;
            const double len = norm(ab);
            if (len <= 2 * tol)
                continue;
            const Vec3 e = ab * (1.0 / len);
            std::vector<std::pair<double, uint32_t>> pts;
            const auto test = [&](uint32_t v) {
                if (v == a || v == b || v == m.f[f][0] || v == m.f[f][1] || v == m.f[f][2])
                    return;
                const Vec3 d = m.v[v] - pa;
                const double s = dot(d, e);
                if (s <= tol || s >= len - tol)
                    return;
                const Vec3 off = d - e * s;
                if (dot(off, off) <= tol * tol)
                    pts.emplace_back(s, v);
            };
            const Cell lo = cell_of({std::min(pa.x, pb.x) - tol, std::min(pa.y, pb.y) - tol, std::min(pa.z, pb.z) - tol}, h);
            const Cell hi = cell_of({std::max(pa.x, pb.x) + tol, std::max(pa.y, pb.y) + tol, std::max(pa.z, pb.z) + tol}, h);
            const double cells = double(hi.x - lo.x + 1) * double(hi.y - lo.y + 1) * double(hi.z - lo.z + 1);
            if (cells > 20000) {
                for (uint32_t v : bverts)
                    test(v);
            } else {
                for (int64_t x = lo.x; x <= hi.x; ++x)
                    for (int64_t y = lo.y; y <= hi.y; ++y)
                        for (int64_t z = lo.z; z <= hi.z; ++z) {
                            const auto it = grid.find({x, y, z});
                            if (it != grid.end())
                                for (uint32_t v : it->second)
                                    test(v);
                        }
            }
            if (pts.empty())
                continue;
            std::sort(pts.begin(), pts.end());
            touched[f] = 1;
            splits.push_back({f, id % 3, std::move(pts)});
        }
        if (splits.empty())
            break;
        for (const Split &s : splits) {
            const Tri t = m.f[s.face];
            const uint32_t a = t[s.corner], b = t[(s.corner + 1) % 3], c = t[(s.corner + 2) % 3];
            std::vector<uint32_t> seq{a};
            for (const auto &p : s.pts)
                seq.push_back(p.second);
            seq.push_back(b);
            m.f[s.face] = {seq[0], seq[1], c};
            for (size_t k = 1; k + 1 < seq.size(); ++k)
                m.f.push_back({seq[k], seq[k + 1], c});
            total += s.pts.size();
        }
    }
    return total;
}

} // namespace meshfix
