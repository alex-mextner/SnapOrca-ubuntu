// Self-intersection diagnostic: BVH over the faces, proper edge-through-face piercings only.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>

#include "core.hpp"

namespace meshfix {

namespace {

struct Aabb {
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    void add(const Vec3 &p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void add(const Aabb &o) {
        add(o.lo);
        add(o.hi);
    }
    bool overlaps(const Aabb &o) const {
        return lo.x <= o.hi.x && hi.x >= o.lo.x && lo.y <= o.hi.y && hi.y >= o.lo.y && lo.z <= o.hi.z && hi.z >= o.lo.z;
    }
    double size_sq() const { return dot(hi - lo, hi - lo); }
};

struct Node {
    Aabb box;
    int32_t left = -1, right = -1; // children, or -1 for a leaf
    uint32_t begin = 0, end = 0;   // leaf: range in the face order
};

class Bvh {
  public:
    Bvh(const Mesh &m, double eps) : m_(m) {
        order_.resize(m.f.size());
        boxes_.resize(m.f.size());
        for (uint32_t i = 0; i < m.f.size(); ++i) {
            order_[i] = i;
            Aabb b;
            for (uint32_t x : m.f[i])
                b.add(m.v[x]);
            b.lo = b.lo - Vec3{eps, eps, eps};
            b.hi = b.hi + Vec3{eps, eps, eps};
            boxes_[i] = b;
        }
        nodes_.reserve(m.f.size() / 2 + 2);
        build(0, uint32_t(m.f.size()));
    }
    uint64_t count_pairs(double eps, uint64_t cap) {
        eps_ = eps;
        cap_ = cap;
        found_ = 0;
        if (!nodes_.empty())
            self(0);
        return found_;
    }

  private:
    int32_t build(uint32_t begin, uint32_t end) {
        const int32_t idx = int32_t(nodes_.size());
        nodes_.emplace_back();
        Aabb box, centres;
        for (uint32_t i = begin; i < end; ++i) {
            box.add(boxes_[order_[i]]);
            const Aabb &b = boxes_[order_[i]];
            centres.add((b.lo + b.hi) * 0.5);
        }
        nodes_[idx].box = box;
        if (end - begin <= 4) {
            nodes_[idx].begin = begin;
            nodes_[idx].end = end;
            return idx;
        }
        const Vec3 ext = centres.hi - centres.lo;
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
        const auto key = [&](uint32_t f) {
            const Aabb &b = boxes_[f];
            return axis == 0 ? b.lo.x + b.hi.x : axis == 1 ? b.lo.y + b.hi.y : b.lo.z + b.hi.z;
        };
        const uint32_t mid = begin + (end - begin) / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end, [&](uint32_t a, uint32_t b) {
            const double ka = key(a), kb = key(b);
            return ka != kb ? ka < kb : a < b;
        });
        const int32_t l = build(begin, mid);
        const int32_t r = build(mid, end);
        nodes_[idx].left = l;
        nodes_[idx].right = r;
        return idx;
    }

    bool leaf(int32_t n) const { return nodes_[n].left < 0; }

    void self(int32_t n) {
        if (found_ >= cap_)
            return;
        if (leaf(n)) {
            for (uint32_t i = nodes_[n].begin; i < nodes_[n].end; ++i)
                for (uint32_t j = i + 1; j < nodes_[n].end; ++j)
                    test(order_[i], order_[j]);
            return;
        }
        self(nodes_[n].left);
        self(nodes_[n].right);
        pair(nodes_[n].left, nodes_[n].right);
    }

    void pair(int32_t a, int32_t b) {
        if (found_ >= cap_ || !nodes_[a].box.overlaps(nodes_[b].box))
            return;
        if (leaf(a) && leaf(b)) {
            for (uint32_t i = nodes_[a].begin; i < nodes_[a].end; ++i)
                for (uint32_t j = nodes_[b].begin; j < nodes_[b].end; ++j)
                    test(order_[i], order_[j]);
            return;
        }
        if (leaf(b) || (!leaf(a) && nodes_[a].box.size_sq() >= nodes_[b].box.size_sq())) {
            pair(nodes_[a].left, b);
            pair(nodes_[a].right, b);
        } else {
            pair(a, nodes_[b].left);
            pair(a, nodes_[b].right);
        }
    }

    // Does an edge of one triangle pierce the interior of the other, clear of the borders by eps?
    bool pierces(const Tri &ta, const Tri &tb) const {
        const Vec3 &p0 = m_.v[tb[0]], &p1 = m_.v[tb[1]], &p2 = m_.v[tb[2]];
        Vec3 n = cross(p1 - p0, p2 - p0);
        const double nl = norm(n);
        if (nl == 0)
            return false;
        n = n * (1.0 / nl);
        const Vec3 *corners[3] = {&p0, &p1, &p2};
        for (int e = 0; e < 3; ++e) {
            const Vec3 &a = m_.v[ta[e]], &b = m_.v[ta[(e + 1) % 3]];
            const double da = dot(a - p0, n), db = dot(b - p0, n);
            if (!((da > eps_ && db < -eps_) || (da < -eps_ && db > eps_)))
                continue;
            const Vec3 h = a + (b - a) * (da / (da - db));
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
                const Vec3 &u = *corners[k], &v = *corners[(k + 1) % 3];
                const Vec3 side = v - u;
                const double sl = norm(side);
                inside = sl > 0 && dot(cross(side, h - u), n) / sl > eps_;
            }
            if (inside)
                return true;
        }
        return false;
    }

    void test(uint32_t a, uint32_t b) {
        const Tri &ta = m_.f[a], &tb = m_.f[b];
        for (uint32_t x : ta)
            if (x == tb[0] || x == tb[1] || x == tb[2])
                return;
        if (!boxes_[a].overlaps(boxes_[b]))
            return;
        if (pierces(ta, tb) || pierces(tb, ta))
            ++found_;
    }

    const Mesh &m_;
    std::vector<uint32_t> order_;
    std::vector<Aabb> boxes_;
    std::vector<Node> nodes_;
    double eps_ = 0;
    uint64_t cap_ = 0, found_ = 0;
};

} // namespace

uint64_t count_self_intersections(const Mesh &m, double eps, uint64_t cap) {
    if (m.f.size() < 2)
        return 0;
    Bvh bvh(m, eps);
    return bvh.count_pairs(eps, cap);
}

} // namespace meshfix
