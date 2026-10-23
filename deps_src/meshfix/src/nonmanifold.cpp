// Non-manifold edge and vertex splitting. SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>

#include "core.hpp"

namespace meshfix {

// Edges with more than two faces are paired radially: around the edge, a face that runs against the
// edge axis pairs with the next face (counter-clockwise) that runs along it, which is the wedge that
// holds the material of consistently outward-oriented shells. Faces left over stay unpaired and
// become boundary. Then every fan of faces around a vertex becomes its own vertex, which also
// separates vertices where several fans touch at a point.
uint64_t split_non_manifold(Mesh &m, uint64_t &split) {
    const auto recs = build_edges(m);
    auto partner = pair_manifold_edges(m, recs);
    uint64_t non_manifold = 0;
    struct Item {
        double angle;
        uint32_t id;
        bool along;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].key == recs[i].key)
            ++j;
        const size_t k = j - i;
        if (k > 2) {
            ++non_manifold;
            const uint32_t a = uint32_t(recs[i].key >> 32), b = uint32_t(recs[i].key & 0xFFFFFFFFu);
            const Vec3 pa = m.v[a];
            const Vec3 axis = m.v[b] - pa;
            const double len = norm(axis);
            if (len > 0) {
                const Vec3 e = axis * (1.0 / len);
                // Reference direction: the axis the edge is least aligned with.
                const Vec3 pick = std::fabs(e.x) <= std::fabs(e.y) && std::fabs(e.x) <= std::fabs(e.z) ? Vec3{1, 0, 0}
                                  : std::fabs(e.y) <= std::fabs(e.z)                                     ? Vec3{0, 1, 0}
                                                                                                         : Vec3{0, 0, 1};
                const Vec3 r1 = cross(e, cross(pick, e));
                const Vec3 r1n = r1 * (1.0 / norm(r1));
                const Vec3 r2 = cross(e, r1n);
                items.clear();
                for (size_t q = i; q < j; ++q) {
                    const uint32_t id = recs[q].id;
                    const Tri &t = m.f[id / 3];
                    const Vec3 c = m.v[t[(id % 3 + 2) % 3]];
                    const Vec3 w = c - pa;
                    items.push_back({std::atan2(dot(w, r2), dot(w, r1n)), id, corner_from(m, id) == a});
                }
                std::stable_sort(items.begin(), items.end(), [](const Item &x, const Item &y) {
                    return x.angle != y.angle ? x.angle < y.angle : x.id < y.id;
                });
                std::vector<uint8_t> taken(k, 0);
                for (size_t q = 0; q < k; ++q) {
                    const size_t n = (q + 1) % k;
                    if (n == q || taken[q] || taken[n])
                        continue;
                    if (!items[q].along && items[n].along && items[q].id / 3 != items[n].id / 3) {
                        taken[q] = taken[n] = 1;
                        partner[items[q].id] = items[n].id;
                        partner[items[n].id] = items[q].id;
                    }
                }
                // Left over: pair what remains with a neighbour that runs the other way (the other wedge of
                // a shell that is inside out here), and detach the rest from the surface altogether, as lone
                // triangles that the later stages dispose of.
                std::vector<size_t> rest;
                for (size_t q = 0; q < k; ++q)
                    if (!taken[q])
                        rest.push_back(q);
                for (size_t q = 0; q < rest.size(); ++q) {
                    const size_t a2 = rest[q], b2 = rest[(q + 1) % rest.size()];
                    if (a2 == b2 || taken[a2] || taken[b2])
                        continue;
                    if (items[a2].along != items[b2].along && items[a2].id / 3 != items[b2].id / 3) {
                        taken[a2] = taken[b2] = 1;
                        partner[items[a2].id] = items[b2].id;
                        partner[items[b2].id] = items[a2].id;
                    }
                }
                for (size_t q = 0; q < k; ++q) {
                    if (taken[q])
                        continue;
                    const uint32_t f = items[q].id / 3;
                    for (uint32_t c = 3 * f; c < 3 * f + 3; ++c) {
                        if (partner[c] != kNone)
                            partner[partner[c]] = kNone;
                        partner[c] = kNone;
                    }
                }
            }
        }
        i = j;
    }
    // Surfaces that touch themselves along an edge can stay connected around the edge's vertices, so the
    // pairs of faces on the edge would still share it. Cut such a pair loose from its neighbours.
    std::vector<uint32_t> cls;
    for (int pass = 0; pass < 4; ++pass) {
        fan_classes(m, partner, cls);
        bool changed = false;
        std::vector<std::array<uint32_t, 3>> sigs; // (class a, class b, record id)
        for (size_t i = 0; i < recs.size();) {
            size_t j = i + 1;
            while (j < recs.size() && recs[j].key == recs[i].key)
                ++j;
            if (j - i > 2) {
                sigs.clear();
                for (size_t q = i; q < j; ++q) {
                    const uint32_t id = recs[q].id;
                    const uint32_t c0 = cls[id], c1 = cls[3 * (id / 3) + (id % 3 + 1) % 3];
                    sigs.push_back({std::min(c0, c1), std::max(c0, c1), id});
                }
                std::sort(sigs.begin(), sigs.end());
                for (size_t q = 0; q < sigs.size();) {
                    size_t r = q + 1;
                    while (r < sigs.size() && sigs[r][0] == sigs[q][0] && sigs[r][1] == sigs[q][1])
                        ++r;
                    // More than two faces on one edge: keep the first and its partner, detach the others.
                    if (r - q > 2) {
                        const uint32_t keep = sigs[q][2], keep_partner = partner[keep];
                        for (size_t s = q; s < r; ++s) {
                            const uint32_t id = sigs[s][2];
                            if (id == keep || id == keep_partner)
                                continue;
                            const uint32_t f = id / 3;
                            for (uint32_t c = 3 * f; c < 3 * f + 3; ++c) {
                                if (c == id)
                                    continue;
                                if (partner[c] != kNone)
                                    partner[partner[c]] = kNone;
                                partner[c] = kNone;
                            }
                            changed = true;
                        }
                    }
                    q = r;
                }
            }
            i = j;
        }
        if (!changed)
            break;
    }
    split += split_fans(m, partner);
    return non_manifold;
}

} // namespace meshfix
