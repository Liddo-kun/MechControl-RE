#pragma once
// A fake driver for the V/F curve: synthetic equations and the NVAPI packets that read and write them. Used by
// vf-curve-check and by ui-pages-check, which opens the V/F Curve tab on it.
#include "vf_tuner.h"
#include <cmath>
#include <cstring>

namespace vf_fixture {
using namespace mech::nv;
using namespace mech::nv::vf;
inline void put_f32(Bytes& b, size_t p, float v) { std::memcpy(b.data() + p, &v, 4); }
inline void put_u16(Bytes& b, size_t p, int v) {
    b[p] = uint8_t(v);
    b[p + 1] = uint8_t(v >> 8);
}
constexpr float fmax = 3.4028234663852886e38f;

// A small equation list shaped like the RTX 5090 Laptop GPU's (VFCURVE notes.md): a switch to a slow curve below a
// switch voltage; the fast curve is the lowest of a steep foot and, by voltage segment, a low line (with a second line
// above it) or a high line. The low line carries a channel 3 correction, the high line a cold correction.
struct Synthetic {
    Bytes info = packet(0x98444, 0xd8444), records = packet(0x158444, 0x158444);
    Bytes variable_info = packet(0x10f424, 0x15f424), variable_records = packet(0xabbc, 0x3abbc);
    int equations = 0, variables = 0;
    Synthetic() {
        var(9, -fmax, fmax);                          // 0: point voltage
        var(8, -100, 200, 0, 0, 4);                   // 1: channel 4
        var(13, -fmax, fmax, 0, 0, 0, 2000);          // 2: chip value
        var(8, -100, 200, 0, 0, 3);                   // 3: channel 3
        var(13, -1, -1, 0, 0, 0, -1);                 // 4: -1
        var(2, -fmax, fmax, 1, 4);                    // 5: -channel 4
        var(3, 0, fmax, 3, 5);                        // 6: channel 3 above channel 4, never below 0
        var(15, 0, 1);                                // 7: the flag
        compare(0, 1, 1, 1, 2, 3, 180, 3000);         // 0: root, the switch at equation 1
        formula(3, 2, {0, 0, 0}, 700000, 700000);     // 1: the switch voltage
        choice(4, 5);                                 // 2: the fast curve
        formula(3, 0, {200, 0.0018f, 0});             // 3: the slow curve
        line({-18000, 0, 0, 0.03f});                  // 4: the foot
        compare(0, 1, 900000, 0, 7, 16);              // 5: the voltage segments
        line({300, 0, 0, 0.002f, 0, 0, -1}, 9);       // 6: low
        line({750, 0, 0, 0.0015f, 0, 0, -0.5f}, 13);  // 7: high
        line({365, 0, 0, 0.002f, 0, 0, -1});          // 8: a second line above low
        compare(6, 0, 0, 0, 10, 11, -fmax, fmax, 12); // 9: +30 while channel 3 is not above channel 4
        formula(3, 2, {0, 0, 0}, 30, 30);             // 10
        formula(3, 2, {0, 0, 0}, 0, 0);               // 11
        formula(3, 2, {0, 0, 0}, -30, -30);           // 12: the fixed -30 after it
        compare(1, 1, 30, 0, 14, 15);                 // 13: a correction below 30 C
        formula(3, 2, {0, 0, 0}, 0, 0);               // 14
        formula(3, 0, {-40, 0, 0}, -100, 0);          // 15
        choice(6, 8);                                 // 16
    }
    void var(int type, float lo, float hi, int a = 0, int b = 0, int channel = 0, int value = 0) {
        int i = variables++;
        variable_info[4 + i / 8] |= uint8_t(1u << (i % 8));
        size_t v = 296 + i * 4356, k = 104 + i * 172;
        put(variable_info, v, uint32_t(type));
        variable_info[v + 268] = uint8_t(a);
        variable_info[v + 269] = uint8_t(b);
        variable_info[v + 276] = uint8_t(channel);
        put(variable_info, v + 284, uint32_t(value));
        put_f32(variable_records, k + 4, lo);
        put_f32(variable_records, k + 8, hi);
    }
    size_t add(int type, int var, float lo, float hi, int next) {
        int i = equations++;
        info[4 + i / 8] |= uint8_t(1u << (i % 8));
        records[4 + i / 8] |= uint8_t(1u << (i % 8));
        size_t a = 1092 + i * 76;
        put(info, a, uint32_t(type));
        put_u16(info, a + 4, var);
        put_u16(info, a + 6, next < 0 ? 0xffff : next);
        put_f32(records, 1092 + i * 172 + 4, lo);
        put_f32(records, 1092 + i * 172 + 8, hi);
        return a;
    }
    size_t control(int i) { return 1092 + i * 172; }
    void compare(int var, int func, float crit, int ref, int yes, int no, float lo = -fmax, float hi = fmax,
                 int next = -1) {
        auto a = add(1, var, lo, hi, next);
        int i = equations - 1;
        put_u16(info, a + 42, yes);
        put_u16(info, a + 44, no);
        records[control(i) + 44] = uint8_t(func);
        records[control(i) + 45] = uint8_t(ref);
        if (ref)
            put(records, control(i) + 48, uint32_t(crit));
        else
            put_f32(records, control(i) + 48, crit);
    }
    void choice(int a, int b) {
        auto p = add(2, 0, -fmax, fmax, -1);
        put_u16(info, p + 42, a);
        put_u16(info, p + 44, b);
    }
    void formula(int type, int var, std::array<float, 3> k, float lo = -fmax, float hi = fmax, int next = -1) {
        add(type, var, lo, hi, next);
        for (int j = 0; j < 3; ++j) put_f32(records, control(equations - 1) + 44 + 4 * j, k[j]);
    }
    // A main formula in chip value, voltage and temperature with ten coefficients.
    void line(std::vector<float> k, int next = -1) {
        add(6, 2, -fmax, fmax, next);
        auto c = control(equations - 1);
        put(records, c + 68, 3);
        for (int j = 0; j < 3; ++j) put(records, c + 48 + 8 * j, uint32_t(std::array{2, 0, 1}[j]));
        put(records, c + 152, 10);
        k.resize(10);
        for (int j = 0; j < 10; ++j) put_f32(records, c + 76 + 8 * j, k[j]);
    }
    Bytes info_trimmed() const { return Bytes(info.begin(), info.begin() + 1092 + equations * 76); }
    Bytes records_trimmed() const { return Bytes(records.begin(), records.begin() + 1092 + equations * 172); }
};

// The driver as the V/F packets see it: the synthetic equations and a client curve of 96 graphics points (grid 498.75
// mV + 7.5 mV, listed rounded up to 5 mV, clock step 15 MHz, at 45 C) plus four memory points.
struct Driver {
    Synthetic s;
    Bytes live = s.records;
    std::vector<uint32_t> base_khz, uv;
    std::array<int32_t, 100> offsets{};
    std::vector<int> sets; // equation SetControl calls: the equation each wrote
    int offset_sets = 0;
    bool refuse_records = false;
    bool follow = false; // the curve follows written equations (ui-pages-check); otherwise it stays stock
    Driver() {
        for (int p = 0; p < 96; ++p) uv.push_back(uint32_t(std::ceil((498750 + 7500.0 * p) / 5000 - 1e-9) * 5000));
        curve(s.records_trimmed());
    }
    void curve(const Bytes& records) {
        Model m(s.info_trimmed(), records, s.variable_info, s.variable_records);
        std::vector<double> raw;
        for (int p = 0; p < 96; ++p) raw.push_back(Env(m, m.E, 498750 + 7500.0 * p, 45).equ(0));
        base_khz.clear();
        for (double r : hold(raw)) base_khz.push_back(uint32_t(to_step(r, 15) * 1000));
    }
    void call(uint32_t id, Bytes& b) {
        switch (id) {
        case 0x8d49471c:
            if (b.size() != s.info.size() || u32(b, 0) != 0xd8444)
                throw std::runtime_error("NVIDIA packet call failed (-9)");
            b = s.info;
            break;
        case 0x4c75c9fe: {
            if (refuse_records || u32(b, 0) != 0x158444) throw std::runtime_error("NVIDIA packet call failed (-9)");
            Bytes mask(b.begin() + 4, b.begin() + 1028);
            b = live;
            std::copy(mask.begin(), mask.end(), b.begin() + 4);
            for (int i = 0; i < s.equations; ++i)
                if (!(mask[i / 8] & (1u << (i % 8)))) std::fill_n(b.begin() + 1092 + i * 172, 172, uint8_t(0));
            break;
        }
        case 0x68b798c4: {
            int bits = 0, which = -1;
            for (int i = 0; i < 8192; ++i)
                if (b[4 + i / 8] & (1u << (i % 8))) {
                    ++bits;
                    which = i;
                }
            if (bits != 1) throw std::runtime_error("fixture: one equation per SetControl");
            std::copy_n(b.begin() + 1092 + which * 172, 172, live.begin() + 1092 + which * 172);
            sets.push_back(which);
            break;
        }
        case 0xb9da41d6: b = s.variable_info; break;
        case 0x5d387298: b = s.variable_records; break;
        case 0x64b43a6a: { // clock domains: graphics points 0-95, memory 96-99
            put(b, 4, 3);
            put(b, 0x28 + 0x28, uint32_t(1000000));
            put(b, 0x28 + 0x2c, uint32_t(-1000000));
            b[0x28 + 0x30] = 0;
            b[0x28 + 0x31] = 95;
            put(b, 0x28 + 0x48 + 4, 4);
            b[0x28 + 0x48 + 0x30] = 96;
            b[0x28 + 0x48 + 0x31] = 99;
            break;
        }
        case 0x507b4b59:
            for (int i = 0; i < 100; ++i) b[4 + i / 8] |= uint8_t(1u << (i % 8));
            put(b, 0x44 + 96 * 0x18, 1); // fixed memory points
            put(b, 0x44 + 97 * 0x18, 1);
            break;
        case 0x21537ad4:
            if (u32(b, 0) >> 16 != 3) throw std::runtime_error("NVIDIA packet call failed (-9)");
            if (follow) curve(Bytes(live.begin(), live.begin() + 1092 + s.equations * 172));
            b[0x24] = 1;
            for (int i = 0; i < 96; ++i) {
                size_t p = 0x68 + i * 0x15c;
                put(b, p + 4, uint32_t(base_khz[i] + offsets[i]));
                put(b, p + 8, uv[i]);
                put(b, p + 0xc, base_khz[i]);
            }
            break;
        case 0x23f1b133:
            for (int i = 0; i < 100; ++i) put(b, 0x44 + i * 0x24 + 0x14, uint32_t(offsets[i]));
            break;
        case 0x0733e009:
            ++offset_sets;
            for (int i = 0; i < 100; ++i) offsets[i] = int32_t(u32(b, 0x44 + i * 0x24 + 0x14));
            break;
        default: throw std::runtime_error("fixture: unexpected NVAPI call");
        }
    }
};
}
