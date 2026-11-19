// The equation model, its evaluation and the search for the clock curve: VFCURVE blackbox/vfe.py.
#include "vf_tuner.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mech::nv::vf {
namespace {
constexpr size_t variable_header = 296, variable_info_size = 4356, variable_control_header = 104,
                 variable_record_size = 172;
double f32(std::span<const uint8_t> b, size_t p) {
    float f;
    std::memcpy(&f, b.data() + p, 4);
    return f;
}
int u16(std::span<const uint8_t> b, size_t p) { return b[p] | b[p + 1] << 8; }
std::string text(int i) { return std::to_string(i); }
}

Model::Model(const Bytes& info, const Bytes& records, const Bytes& variable_info, const Bytes& variable_records)
    : stock(records) {
    auto n = equation_count(info);
    if (info.size() < equation_header + n * info_size) throw std::runtime_error("Truncated V/F equation list");
    E.resize(n);
    for (size_t i = 0; i < n; ++i) {
        auto a = std::span(info).subspan(equation_header + i * info_size, info_size);
        auto c = record(records, i);
        auto& e = E[i];
        e.type = int(u32(a, 0));
        e.var = u16(a, 4);
        e.next = u16(a, 6) == 0xffff ? -1 : u16(a, 6);
        e.lo = f32(c, 4);
        e.hi = f32(c, 8);
        if (e.type == 1) {
            e.yes = u16(a, 42);
            e.no = u16(a, 44);
            e.func = c[44];
            e.ref = c[45];
            e.crit = f32(c, 48);
            e.refidx = int(std::min<uint32_t>(u32(c, 48), INT_MAX));
        } else if (e.type == 2) {
            e.a = u16(a, 42);
            e.b = u16(a, 44);
            e.max = c[44];
        } else if (e.type == 3) {
            e.nvars = 1;
            e.vars[0] = e.var;
            e.nk = 3;
            for (int j = 0; j < 3; ++j) e.k[j] = f32(c, 44 + 4 * j);
        } else if (e.type == 6) {
            auto nv = u32(c, 68), nk = u32(c, 152);
            if (nv > 3 || nk > 10)
                throw std::runtime_error("V/F equation " + text(int(i)) +
                                         " has a formula layout MechControl does not know");
            e.nvars = int(nv);
            e.nk = int(nk);
            for (int j = 0; j < e.nvars; ++j) e.vars[j] = int(std::min<uint32_t>(u32(c, 48 + 8 * j), INT_MAX));
            for (int j = 0; j < e.nk; ++j) e.k[j] = f32(c, 76 + 8 * j);
        } else
            throw std::runtime_error("V/F equation " + text(int(i)) + " has a type MechControl does not know (" +
                                     text(e.type) + ")");
    }
    if (variable_info.size() < variable_header || variable_records.size() < variable_control_header)
        throw std::runtime_error("Truncated V/F variable list");
    size_t count = 0;
    for (size_t i = 0; i < 256; ++i)
        if (variable_info[4 + i / 8] & (1u << (i % 8))) {
            if (i != count)
                throw std::runtime_error(
                    "The driver lists its V/F variables with gaps, which MechControl does not know");
            ++count;
        }
    if (variable_info.size() < variable_header + count * variable_info_size ||
        variable_records.size() < variable_control_header + count * variable_record_size)
        throw std::runtime_error("Truncated V/F variable list");
    V.resize(count);
    for (size_t i = 0; i < count; ++i) {
        auto v = std::span(variable_info).subspan(variable_header + i * variable_info_size, variable_info_size);
        auto k = std::span(variable_records)
                     .subspan(variable_control_header + i * variable_record_size, variable_record_size);
        auto& x = V[i];
        x.type = int(u32(v, 0));
        x.lo = f32(k, 4);
        x.hi = f32(k, 8);
        if (x.type == 2 || x.type == 3) {
            x.a = v[268];
            x.b = v[269];
        }
        if (x.type == 8) x.channel = v[276];
        if (x.type == 13) x.value = int32_t(u32(v, 284));
    }
    // What a variable depends on: the point voltage and thermal channels.
    sym.resize(count);
    std::vector<char> state(count); // 1 while being worked out, 2 when done
    auto work = [&](auto& self, size_t i) -> const std::set<int>& {
        if (state[i] == 2) return sym[i];
        if (state[i] == 1) throw std::runtime_error("The V/F variables refer to each other in a loop");
        state[i] = 1;
        auto& x = V[i];
        if (x.type == 9) sym[i] = {voltage};
        if (x.type == 8) sym[i] = {x.channel};
        if (x.type == 2 || x.type == 3) {
            if (size_t(x.a) >= count || size_t(x.b) >= count)
                throw std::runtime_error("V/F variable " + text(int(i)) + " refers to a variable that does not exist");
            auto s = self(self, x.a);
            auto& t = self(self, x.b);
            s.insert(t.begin(), t.end());
            sym[i] = std::move(s);
        }
        state[i] = 2;
        return sym[i];
    };
    for (size_t i = 0; i < count; ++i) work(work, i);
}
const Equation& Model::at(const Equations& list, int i) const {
    if (i < 0 || size_t(i) >= list.size())
        throw std::runtime_error("A V/F equation refers to equation " + text(i) + ", which does not exist");
    return list[i];
}
bool Model::is_switch(const Equation& e) const {
    return e.type == 1 && e.ref && size_t(e.var) < sym.size() && sym[e.var].contains(voltage);
}
std::vector<int> Model::kids(const Equation& e, bool refs) const {
    std::vector<int> k;
    if (e.type == 1) {
        k = {e.yes, e.no};
        if (e.ref && refs) k.push_back(e.refidx);
    }
    if (e.type == 2) k = {e.a, e.b};
    if (e.next >= 0) k.push_back(e.next);
    return k;
}
std::set<int> Model::tree(int root) const {
    std::set<int> seen;
    std::vector<int> stack{root};
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        if (seen.insert(i).second)
            for (int k : kids(at(E, i))) stack.push_back(k);
    }
    return seen;
}

Env::Env(const Model& model, const Equations& list, double uv, double celsius, int flag,
         const std::map<int, double>& hotter)
    : m(model), E(list), uv_(uv), celsius_(celsius), flag_(flag), hotter_(hotter), value_(model.V.size()),
      known_(model.V.size()) {}
double Env::var(int i) {
    if (i < 0 || size_t(i) >= m.V.size())
        throw std::runtime_error("A V/F equation refers to variable " + text(i) + ", which does not exist");
    if (known_[i]) return value_[i];
    auto& x = m.V[i];
    double r;
    switch (x.type) {
    case 13: r = x.value; break;
    case 8: {
        auto h = hotter_.find(x.channel);
        r = h != hotter_.end() ? celsius_ + h->second : celsius_;
        break;
    }
    case 9: r = uv_; break;
    case 15: r = flag_; break;
    case 2: r = var(x.a) * var(x.b); break;
    case 3: r = var(x.a) + var(x.b); break;
    default:
        throw std::runtime_error("V/F variable " + text(i) + " has a type MechControl does not know (" + text(x.type) +
                                 ")");
    }
    known_[i] = 1;
    return value_[i] = std::max(x.lo, std::min(r, x.hi));
}
bool Env::test(const Equation& e, double crit) {
    double x = var(e.var);
    return e.func == 0 ? x == crit : e.func == 1 ? x >= crit : x > crit;
}
double Env::formula(const Equation& e) {
    std::array<double, 3> x{};
    for (int j = 0; j < e.nvars; ++j) x[j] = var(e.vars[j]);
    auto& k = e.k;
    if (e.type == 3) return k[0] + k[1] * x[0] + k[2] * x[0] * x[0];
    if (e.nk < 6 || e.nvars < 2 || (e.nk == 10 && e.nvars < 3))
        throw std::runtime_error("A V/F formula has a layout MechControl does not know");
    double r = k[0] + k[1] * x[0] + k[2] * x[0] * x[0] + k[3] * x[1] + k[4] * x[1] * x[1] + k[5] * x[0] * x[1];
    if (e.nk == 10) r += k[6] * x[2] + k[7] * x[2] * x[2] + k[8] * x[0] * x[2] + k[9] * x[1] * x[2];
    return r;
}
double Env::equ(int i) {
    auto& e = m.at(E, i);
    if (++depth_ > 10000) throw std::runtime_error("The V/F equations refer to each other in a loop");
    double r;
    if (e.type == 1)
        r = equ(test(e, e.ref ? equ(e.refidx) : e.crit) ? e.yes : e.no);
    else if (e.type == 2) {
        double a = equ(e.a), b = equ(e.b);
        r = e.max ? std::max(a, b) : std::min(a, b);
    } else
        r = formula(e);
    r = std::max(e.lo, std::min(r, e.hi));
    if (e.next >= 0) r += equ(e.next);
    --depth_;
    return r;
}

double to_step(double x, double step) { return (std::ceil((std::floor(x + 1e-9) + 1) / step - 1e-9) - 1) * step; }
std::vector<double> hold(std::vector<double> c) {
    for (size_t i = 1; i < c.size(); ++i) c[i] = std::max(c[i], c[i - 1]);
    return c;
}
std::vector<double> with_offsets(const std::vector<double>& base, const std::vector<double>& off, double st) {
    std::vector<double> out(base.size());
    double top = -INFINITY;
    for (size_t p = 0; p < base.size(); ++p) {
        double o = off[p], a = std::abs(o), k = std::floor(a / (2 * st) + 1e-9),
               q = 2 * st * k + (a - 2 * st * k > 1e-6 ? std::floor(st) : 0),
               x = std::floor(base[p]) + (o > 0 ? q : -q);
        top = std::max(top, to_step(x, st));
        out[p] = top;
    }
    return out;
}
double snap(double v, double step) {
    double c = std::ceil(v / step - 1e-9) * step;
    return c - v < 1 ? c : v;
}
std::vector<double> driver_stock(const Points& points) {
    std::vector<double> out;
    for (auto& p : points.graphics) {
        if (!p.base_khz && p.offset_khz)
            throw std::runtime_error(
                "This driver does not report the stock clock and offsets are set, so the stock curve is not known. Set every offset to 0 first.");
        out.push_back((p.base_khz ? double(*p.base_khz) : std::max(0.0, double(p.final_khz) - p.offset_khz)) / 1000);
    }
    return out;
}

config::Json to_json(const Meta& m) {
    config::Json grid = {{"npoints", m.uv.size()}};
    if (m.v0 && m.vstep) {
        grid["v0"] = *m.v0;
        grid["vstep"] = *m.vstep;
    } else
        grid["uv"] = m.uv;
    return {
        {"root", m.root},
        {"grid", grid},
        {"step", m.step},
        {"fit",
         {{"points_equal", m.equal}, {"of", m.uv.size()}, {"temperature", m.temperature}, {"voltages", m.voltages}}},
        {"candidates", m.candidates},
        {"recorded", m.recorded}};
}
Meta meta_from_json(const config::Json& j) {
    Meta m;
    m.root = j.at("root").get<int>();
    auto& g = j.at("grid");
    if (g.contains("uv"))
        m.uv = g.at("uv").get<std::vector<double>>();
    else {
        m.v0 = g.at("v0").get<double>();
        m.vstep = g.at("vstep").get<double>();
        for (int i = 0; i < g.at("npoints").get<int>(); ++i) m.uv.push_back(*m.v0 + *m.vstep * i);
    }
    if (m.uv.size() < 2) throw std::runtime_error("Malformed V/F curve points");
    m.step = j.at("step").get<double>();
    auto& f = j.at("fit");
    m.equal = f.at("points_equal").get<int>();
    m.temperature = f.at("temperature").get<double>();
    m.voltages = f.at("voltages").get<std::string>();
    if (j.contains("candidates")) m.candidates = j.at("candidates").get<std::vector<int>>();
    if (j.contains("recorded")) m.recorded = j.at("recorded").get<std::string>();
    return m;
}

Meta find_meta(const Model& m, const Points& points) {
    auto stock = driver_stock(points);
    size_t n = stock.size();
    if (n < 8) throw std::runtime_error("The driver's curve has fewer than 8 points");
    std::set<int> par;
    for (auto& e : m.E)
        for (int k : m.kids(e)) par.insert(k);
    auto vdep = [&](int i) {
        for (int j : m.tree(i)) {
            auto& e = m.E[j];
            auto depends = [&](int v) { return size_t(v) < m.sym.size() && m.sym[v].contains(voltage); };
            for (int k = 0; k < e.nvars; ++k)
                if (depends(e.vars[k])) return true;
            if (e.type == 1 && depends(e.var)) return true;
        }
        return false;
    };
    std::vector<int> roots;
    for (int i = 0; i < int(m.E.size()); ++i)
        if (!par.contains(i) && vdep(i)) roots.push_back(i);
    if (roots.empty()) throw std::runtime_error("No V/F equation depends on the voltage");
    auto score = [&](int root, const std::vector<double>& uv, double step, double celsius) {
        std::vector<double> raw;
        for (double u : uv) raw.push_back(Env(m, m.E, u, celsius).equ(root));
        raw = hold(std::move(raw));
        int equal = 0;
        for (size_t i = 0; i < n; ++i) equal += to_step(raw[i], step) == snap(stock[i], step);
        return equal;
    };
    // Point voltage candidates: straight grids whose values, rounded up to 5 mV, give the listed voltages, then the
    // listed voltages themselves.
    struct Candidate {
        bool grid;
        double v0, st;
        std::vector<double> uv;
    };
    std::vector<Candidate> cands;
    std::vector<long long> listed;
    for (auto& p : points.graphics) listed.push_back(p.uv);
    double est = double(listed.back() - listed.front()) / double(n - 1);
    std::vector<long long> steps;
    for (int d = -1250; d <= 1250; d += 625) steps.push_back((long long)std::nearbyint(est / 625.0) * 625 + d);
    std::stable_sort(steps.begin(), steps.end(),
                     [&](long long a, long long b) { return std::abs(a - est) < std::abs(b - est); });
    for (long long st : steps) {
        if (st <= 0) continue;
        for (long long v0 = listed[0]; v0 > listed[0] - 5000; v0 -= 625) {
            bool fits = true;
            for (size_t i = 0; i < n && fits; ++i)
                fits = std::ceil(double(v0 + st * (long long)i) / 5000.0 - 1e-9) * 5000 == double(listed[i]);
            if (!fits) continue;
            Candidate c{true, double(v0), double(st), {}};
            for (size_t i = 0; i < n; ++i) c.uv.push_back(double(v0 + st * (long long)i));
            cands.push_back(std::move(c));
        }
    }
    cands.push_back({false, 0, 0, {}});
    for (auto v : listed) cands.back().uv.push_back(double(v));
    int best = -1, root = 0;
    double step = 0, t0 = 0;
    for (int r : roots)
        for (auto* c : {&cands.front(), &cands.back()})
            for (double st : {7.5, 15.0})
                for (int t = -5; t < 106; t += 10)
                    if (int s = score(r, c->uv, st, t); s > best)
                        std::tie(best, root, step, t0) = std::tuple(s, r, st, double(t));
    best = -1;
    double celsius = 0;
    const Candidate* found = nullptr;
    for (auto& c : cands)
        for (int t2 = int(t0 * 2) - 20; t2 < int(t0 * 2) + 21; ++t2)
            if (int s = score(root, c.uv, step, t2 / 2.0); s > best)
                std::tie(best, celsius, found) = std::tuple(s, t2 / 2.0, &c);
    Meta meta;
    meta.root = root;
    meta.uv = found->uv;
    if (found->grid) {
        meta.v0 = found->v0;
        meta.vstep = found->st;
    }
    meta.step = step;
    meta.equal = best;
    meta.temperature = celsius;
    meta.voltages = found->grid ? "grid" : "listed";
    meta.candidates = roots;
    return meta;
}
}
