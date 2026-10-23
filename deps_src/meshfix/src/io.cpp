// STL (binary + ASCII) and OBJ reading, STL / OBJ writing. SPDX-License-Identifier: MIT
#include "io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace meshfix {

namespace {

struct File {
    FILE *f;
    explicit File(FILE *p) : f(p) {}
    ~File() { close(); }
    // fclose() flushes: a full disk shows up here, not in the fwrite calls.
    bool close() {
        const bool ok = !f || std::fclose(f) == 0;
        f = nullptr;
        return ok;
    }
};

std::string lower_ext(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

bool read_all(const std::string &path, std::vector<char> &data) {
    File file(std::fopen(path.c_str(), "rb"));
    if (!file.f)
        return false;
    std::fseek(file.f, 0, SEEK_END);
    const long size = std::ftell(file.f);
    std::fseek(file.f, 0, SEEK_SET);
    if (size < 0)
        return false;
    data.resize(size_t(size));
    return data.empty() || std::fread(data.data(), 1, data.size(), file.f) == data.size();
}

uint32_t le32(const char *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v; // little-endian hosts only: every platform this library targets
}

float lef(const char *p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

IoStatus parse_stl(const std::vector<char> &data, Mesh &m) {
    const bool binary_size = data.size() >= 84 && 84 + size_t(le32(data.data() + 80)) * 50 == data.size();
    if (binary_size) {
        const size_t n = le32(data.data() + 80);
        m.v.reserve(n * 3);
        m.f.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const char *rec = data.data() + 84 + i * 50 + 12;
            for (int k = 0; k < 3; ++k) {
                m.v.push_back({double(lef(rec + k * 12)), double(lef(rec + k * 12 + 4)), double(lef(rec + k * 12 + 8))});
            }
            const uint32_t base = uint32_t(i * 3);
            m.f.push_back({base, base + 1, base + 2});
        }
        return IoStatus::Ok;
    }
    // ASCII: every "vertex x y z" line is a corner, three make a facet.
    std::string text(data.begin(), data.end());
    if (text.compare(0, 5, "solid") != 0 && text.find("facet") == std::string::npos)
        return IoStatus::Format;
    size_t pos = 0;
    while ((pos = text.find("vertex", pos)) != std::string::npos) {
        pos += 6;
        char *end = nullptr;
        const char *p = text.c_str() + pos;
        const double x = std::strtod(p, &end);
        if (end == p)
            return IoStatus::Format;
        p = end;
        const double y = std::strtod(p, &end);
        if (end == p)
            return IoStatus::Format;
        p = end;
        const double z = std::strtod(p, &end);
        if (end == p)
            return IoStatus::Format;
        m.v.push_back({x, y, z});
    }
    if (m.v.empty() || m.v.size() % 3 != 0)
        return IoStatus::Format;
    for (uint32_t i = 0; i < m.v.size(); i += 3)
        m.f.push_back({i, i + 1, i + 2});
    return IoStatus::Ok;
}

IoStatus parse_obj(const std::vector<char> &data, Mesh &m) {
    const char *p = data.data(), *end = p + data.size();
    std::vector<int64_t> idx;
    while (p < end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', size_t(end - p)));
        if (!eol)
            eol = end;
        std::string line(p, eol);
        p = eol < end ? eol + 1 : end;
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            double x, y, z;
            if (std::sscanf(line.c_str() + 2, "%lf %lf %lf", &x, &y, &z) != 3)
                return IoStatus::Format;
            m.v.push_back({x, y, z});
        } else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            idx.clear();
            const char *q = line.c_str() + 2;
            while (*q) {
                while (*q == ' ' || *q == '\t' || *q == '\r')
                    ++q;
                if (!*q || *q == '#')
                    break;
                char *e = nullptr;
                const long long v = std::strtoll(q, &e, 10);
                if (e == q)
                    return IoStatus::Format;
                while (*e && *e != ' ' && *e != '\t' && *e != '\r')
                    ++e; // skip /vt/vn
                q = e;
                const int64_t i = v < 0 ? int64_t(m.v.size()) + v : v - 1;
                if (i < 0 || i >= int64_t(m.v.size()))
                    return IoStatus::Format;
                idx.push_back(i);
            }
            for (size_t k = 1; k + 1 < idx.size(); ++k)
                m.f.push_back({uint32_t(idx[0]), uint32_t(idx[k]), uint32_t(idx[k + 1])});
        }
    }
    return IoStatus::Ok;
}

} // namespace

IoStatus read_mesh_file(const std::string &path, Mesh &m) {
    std::vector<char> data;
    if (!read_all(path, data))
        return IoStatus::Io;
    m = Mesh{};
    const std::string ext = lower_ext(path);
    if (ext == ".stl")
        return parse_stl(data, m);
    if (ext == ".obj")
        return parse_obj(data, m);
    return IoStatus::Format;
}

IoStatus write_mesh_file(const Mesh &m, const std::string &path) {
    const std::string ext = lower_ext(path);
    if (ext != ".obj" && m.f.size() > 0xFFFFFFFFu)
        return IoStatus::Format; // binary STL counts its faces in 32 bits
    File file(std::fopen(path.c_str(), "wb"));
    if (!file.f)
        return IoStatus::Io;
    if (ext == ".obj") {
        for (const Vec3 &p : m.v)
            std::fprintf(file.f, "v %.15g %.15g %.15g\n", p.x, p.y, p.z);
        for (const Tri &t : m.f)
            std::fprintf(file.f, "f %u %u %u\n", t[0] + 1, t[1] + 1, t[2] + 1);
        const bool failed = std::ferror(file.f) != 0;
        return file.close() && !failed ? IoStatus::Ok : IoStatus::Io;
    }
    char header[80] = {};
    std::snprintf(header, sizeof header, "meshfix binary STL");
    std::fwrite(header, 1, 80, file.f);
    const uint32_t n = uint32_t(m.f.size());
    std::fwrite(&n, 4, 1, file.f);
    for (const Tri &t : m.f) {
        const Vec3 &a = m.v[t[0]], &b = m.v[t[1]], &c = m.v[t[2]];
        Vec3 nrm = cross(b - a, c - a);
        const double l = norm(nrm);
        if (l > 0)
            nrm = nrm * (1.0 / l);
        float rec[12] = {float(nrm.x), float(nrm.y), float(nrm.z), float(a.x), float(a.y), float(a.z),
                         float(b.x),   float(b.y),   float(b.z),   float(c.x), float(c.y), float(c.z)};
        const uint16_t attr = 0;
        std::fwrite(rec, 4, 12, file.f);
        std::fwrite(&attr, 2, 1, file.f);
    }
    const bool failed = std::ferror(file.f) != 0;
    return file.close() && !failed ? IoStatus::Ok : IoStatus::Io;
}

} // namespace meshfix
