// Boundary loop tracing and hole filling. SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

#include "core.hpp"

namespace meshfix {

namespace {

constexpr size_t kDpMaxEdges = 16;    // minimum-area triangulation up to here
constexpr size_t kEarMaxEdges = 2048; // ear clipping up to here, a centroid fan beyond

struct EdgeSet {
    std::vector<uint64_t> sorted; // edges of the mesh
    std::unordered_set<uint64_t> added; // edges created by earlier fills
    bool has(uint32_t a, uint32_t b) const {
        const uint64_t k = edge_key(a, b);
        return std::binary_search(sorted.begin(), sorted.end(), k) || added.count(k) != 0;
    }
};

bool same_point(const Mesh &m, uint32_t a, uint32_t b) {
    return m.v[a].x == m.v[b].x && m.v[a].y == m.v[b].y && m.v[a].z == m.v[b].z;
}

double area2(const Vec3 &a, const Vec3 &b, const Vec3 &c) { return norm(cross(b - a, c - a)); }

// Minimum total area triangulation of the polygon, every diagonal a new edge. Empty when none exists.
std::vector<Tri> triangulate_dp(const Mesh &m, const std::vector<uint32_t> &q, const EdgeSet &edges) {
    const size_t n = q.size();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> cost(n, std::vector<double>(n, inf));
    std::vector<std::vector<uint32_t>> pick(n, std::vector<uint32_t>(n, kNone));
    const auto allowed = [&](size_t i, size_t j) {
        if (j == i + 1 || (i == 0 && j == n - 1))
            return true;
        return !edges.has(q[i], q[j]);
    };
    for (size_t i = 0; i + 1 < n; ++i)
        cost[i][i + 1] = 0;
    for (size_t len = 2; len < n; ++len)
        for (size_t i = 0; i + len < n; ++i) {
            const size_t j = i + len;
            if (!allowed(i, j))
                continue;
            for (size_t k = i + 1; k < j; ++k) {
                if (cost[i][k] == inf || cost[k][j] == inf)
                    continue;
                const double a = area2(m.v[q[i]], m.v[q[k]], m.v[q[j]]);
                // A triangle without area is acceptable only where two corners are copies of one point (the
                // closing side of a pinched loop).
                if (a <= 0 && !(same_point(m, q[i], q[k]) || same_point(m, q[k], q[j]) || same_point(m, q[i], q[j])))
                    continue;
                const double c = cost[i][k] + cost[k][j] + a;
                if (c < cost[i][j]) {
                    cost[i][j] = c;
                    pick[i][j] = uint32_t(k);
                }
            }
        }
    std::vector<Tri> out;
    if (cost[0][n - 1] == inf)
        return out;
    std::vector<std::pair<size_t, size_t>> stack{{0, n - 1}};
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        if (j <= i + 1)
            continue;
        const size_t k = pick[i][j];
        out.push_back({q[i], q[k], q[j]});
        stack.push_back({i, k});
        stack.push_back({k, j});
    }
    return out;
}

// Ear clipping in the plane the polygon projects best onto. Empty when it gets stuck.
std::vector<Tri> triangulate_ears(const Mesh &m, const std::vector<uint32_t> &q, const EdgeSet &edges) {
    const size_t n = q.size();
    Vec3 normal{0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        const Vec3 &a = m.v[q[i]], &b = m.v[q[(i + 1) % n]];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    const double ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
    std::vector<std::array<double, 2>> p(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 &v = m.v[q[i]];
        if (ax >= ay && ax >= az)
            p[i] = {normal.x >= 0 ? v.y : -v.y, v.z};
        else if (ay >= az)
            p[i] = {normal.y >= 0 ? v.z : -v.z, v.x};
        else
            p[i] = {normal.z >= 0 ? v.x : -v.x, v.y};
    }
    const auto turn = [&](size_t a, size_t b, size_t c) {
        return (p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[c][0] - p[a][0]);
    };
    std::vector<uint32_t> prev(n), next(n);
    for (size_t i = 0; i < n; ++i) {
        prev[i] = uint32_t((i + n - 1) % n);
        next[i] = uint32_t((i + 1) % n);
    }
    std::vector<Tri> out;
    size_t remaining = n, cur = 0, since_clip = 0;
    while (remaining > 3) {
        const size_t a = prev[cur], b = cur, c = next[cur];
        bool ear = turn(a, b, c) > 0 && !edges.has(q[a], q[c]);
        if (ear)
            for (size_t k = next[c]; k != a && ear; k = next[k]) {
                // A vertex inside (or on) the candidate ear blocks it.
                const double d1 = turn(a, b, k), d2 = turn(b, c, k), d3 = turn(c, a, k);
                if (d1 >= 0 && d2 >= 0 && d3 >= 0)
                    ear = false;
            }
        if (ear) {
            out.push_back({q[a], q[b], q[c]});
            next[a] = uint32_t(c);
            prev[c] = uint32_t(a);
            --remaining;
            cur = c;
            since_clip = 0;
        } else {
            cur = c;
            if (++since_clip > remaining)
                return {};
        }
    }
    const size_t a = prev[cur], b = cur, c = next[cur];
    out.push_back({q[a], q[b], q[c]});
    return out;
}

// Always works: a new vertex at the centroid joined to every boundary vertex.
std::vector<Tri> triangulate_fan(Mesh &m, const std::vector<uint32_t> &q) {
    Vec3 centre{0, 0, 0};
    for (uint32_t x : q)
        centre = centre + m.v[x];
    centre = centre * (1.0 / double(q.size()));
    const uint32_t c = uint32_t(m.v.size());
    m.v.push_back(centre);
    std::vector<Tri> out;
    for (size_t i = 0; i < q.size(); ++i)
        out.push_back({q[i], q[(i + 1) % q.size()], c});
    return out;
}

// Splits a loop that runs through the same point twice into simple loops: the copies of a pinched vertex
// have equal coordinates. Each loop closes with a side between the two copies (p' -> p, and p -> p' in the loop
// that continues), which has no length yet; the two sides are one edge once both loops are filled, so the
// result stays manifold with the copies kept apart.
std::vector<std::vector<uint32_t>> split_at_repeated_points(const Mesh &m, const std::vector<uint32_t> &loop) {
    std::map<std::array<double, 3>, uint32_t> classes;
    std::vector<uint32_t> cls;
    for (uint32_t v : loop) {
        const Vec3 &p = m.v[v];
        const auto it = classes.emplace(std::array<double, 3>{p.x + 0.0, p.y + 0.0, p.z + 0.0}, uint32_t(classes.size())).first;
        cls.push_back(it->second);
    }
    if (classes.size() == loop.size())
        return {loop};
    std::vector<std::vector<uint32_t>> loops;
    std::vector<uint32_t> stack, stack_cls;
    std::map<uint32_t, size_t> position; // the latest stack position of each point
    for (size_t i = 0; i < loop.size(); ++i) {
        const auto it = position.find(cls[i]);
        if (it == position.end()) {
            position.emplace(cls[i], stack.size());
            stack.push_back(loop[i]);
            stack_cls.push_back(cls[i]);
            continue;
        }
        const size_t start = it->second;
        std::vector<uint32_t> sub(stack.begin() + start, stack.end());
        sub.push_back(loop[i]);
        loops.push_back(std::move(sub));
        for (size_t k = start + 1; k < stack.size(); ++k)
            position.erase(stack_cls[k]);
        stack.resize(start + 1);
        stack_cls.resize(start + 1);
        position[cls[i]] = stack.size();
        stack.push_back(loop[i]);
        stack_cls.push_back(cls[i]);
    }
    if (stack.size() >= 3)
        loops.push_back(std::move(stack));
    return loops;
}

} // namespace

HoleStats fill_holes(Mesh &m, uint32_t max_hole_edges, const Ctx &) {
    HoleStats st;
    const auto recs = build_edges(m);
    std::vector<uint32_t> bids;
    EdgeSet edges;
    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].key == recs[i].key)
            ++j;
        edges.sorted.push_back(recs[i].key);
        if (j - i == 1)
            bids.push_back(recs[i].id);
        i = j;
    }
    if (bids.empty())
        return st;

    // Outgoing boundary half-edges per vertex.
    std::vector<std::pair<uint32_t, uint32_t>> out_edges; // (from vertex, index into bids)
    for (uint32_t i = 0; i < bids.size(); ++i)
        out_edges.emplace_back(corner_from(m, bids[i]), i);
    std::sort(out_edges.begin(), out_edges.end());
    std::vector<uint8_t> used(bids.size(), 0);

    std::vector<std::vector<uint32_t>> loops;
    for (uint32_t s = 0; s < bids.size(); ++s) {
        if (used[s])
            continue;
        std::vector<uint32_t> loop;
        const uint32_t start = corner_from(m, bids[s]);
        uint32_t cur = s;
        bool closed = false;
        while (true) {
            used[cur] = 1;
            loop.push_back(corner_from(m, bids[cur]));
            const uint32_t v = corner_to(m, bids[cur]);
            if (v == start) {
                closed = true;
                break;
            }
            auto it = std::lower_bound(out_edges.begin(), out_edges.end(), std::make_pair(v, uint32_t(0)));
            uint32_t nxt = kNone;
            for (; it != out_edges.end() && it->first == v; ++it)
                if (!used[it->second]) {
                    nxt = it->second;
                    break;
                }
            if (nxt == kNone)
                break;
            cur = nxt;
        }
        ++st.found;
        if (closed && loop.size() >= 3)
            loops.push_back(std::move(loop));
        else
            ++st.skipped;
    }

    std::vector<Tri> added;
    for (const auto &p : loops) {
        if (max_hole_edges && p.size() > max_hole_edges) {
            ++st.skipped;
            continue;
        }
        // The filling runs against the direction of the existing boundary edges.
        std::vector<uint32_t> q(p.rbegin(), p.rend());
        const auto parts = split_at_repeated_points(m, q);
        // One boundary cycle that pinches into several simple loops counts as one hole per loop.
        if (parts.size() > 1)
            st.found += parts.size() - 1;
        for (const auto &part : parts) {
            std::vector<Tri> tris;
            if (part.size() == 3)
                tris.push_back({part[0], part[1], part[2]});
            else if (part.size() <= kDpMaxEdges)
                tris = triangulate_dp(m, part, edges);
            else if (part.size() <= kEarMaxEdges)
                tris = triangulate_ears(m, part, edges);
            if (tris.empty())
                tris = triangulate_fan(m, part);
            for (const Tri &t : tris) {
                for (int i = 0; i < 3; ++i)
                    edges.added.insert(edge_key(t[i], t[(i + 1) % 3]));
                added.push_back(t);
            }
            ++st.filled;
        }
    }
    st.faces_added = added.size();
    m.f.insert(m.f.end(), added.begin(), added.end());
    return st;
}

} // namespace meshfix
