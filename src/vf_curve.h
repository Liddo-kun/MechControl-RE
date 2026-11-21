#pragma once
#include "nvidia.h"
#include <map>

// The driver's V/F curve as a profile applies it, without the tuner (vf_tuner.*): the control records of the VFE
// equations, which compute the stock clock of each curve point, and the client offset the driver adds per point.
// The packet sizes are those of driver 621.24. A driver that refuses them, or lists its equations in another way,
// is refused before anything is written.
namespace mech::nv::vf {
constexpr size_t equation_header = 1092, info_size = 76, record_size = 172;
// What a curve was made for. A curve applies only where all four are the same.
struct Gpu {
    uint16_t device = 0; // PCI device ID
    std::string vbios, driver;
    uint64_t equations = 0; // Hash of the driver's equation list (its info records)
    bool operator==(const Gpu&) const = default;
};
// A curve ready to apply: the control records that differ from stock, by equation, one offset per graphics point
// from the lowest, and the offset of every programmable memory point; without it the memory offset stays as it is.
struct Curve {
    Gpu gpu;
    std::map<int, Bytes> records;
    std::vector<int> offsets_khz;
    std::optional<int> memory_khz;
    bool operator==(const Curve&) const = default;
};
config::Json to_json(const Curve&);
Curve curve_from_json(const config::Json&); // Throws on a malformed curve.

// The driver's equation list: a header with the list's mask, then one info record per equation.
Bytes equation_info(Api&);
size_t equation_count(const Bytes& info_or_records);
Gpu gpu(const Identity&, const Bytes& info);
// The control records as the driver has them now: the same header, then one record per equation.
Bytes records(Api&, const Bytes& info);
std::span<const uint8_t> record(const Bytes& records, size_t equation);
// The variables the equations read (the tuner's model needs them): their list, then their control records.
Bytes variable_info(Api&);
Bytes variable_records(Api&, const Bytes& variable_info);

struct Point {
    uint32_t uv = 0;       // the voltage the driver lists
    uint32_t base_khz = 0; // the clock from the equations
    uint32_t final_khz = 0; // with the offset
    int32_t offset_khz = 0;
    bool programmable = false; // takes an offset
};
// The graphics and memory points of the driver's client curve, from the lowest; a GPU without a programmable memory
// clock has no memory points.
struct Points {
    std::vector<Point> graphics;
    int32_t offset_min_khz = 0, offset_max_khz = 0;
    std::vector<Point> memory;
    int32_t memory_min_khz = 0, memory_max_khz = 0;
};
Points points(Api&);

struct Applied {
    int equations = 0, offsets = 0; // how many were written
    bool memory = false;            // the memory offset was written
    bool replaced = false;          // an offset that was not 0 before was changed
};
// Makes the driver's equations equal its stock records with the curve's records and its offsets equal the curve's
// (those that go down before the equations, the others and the memory offset after), and reads both back. The stock
// records come from `directory` (store_directory()). Refuses a curve made for another GPU, VBIOS, driver or equation
// list, one without recorded stock, before any write.
Applied apply(Api&, const Curve&, const std::filesystem::path& directory);
// Stock equations and every offset 0, memory included.
Applied restore(Api&, const std::filesystem::path& directory);
// The curve on the GPU now, as apply() would write it: the records that differ from stock, every offset and the memory
// offset. Needs the recorded stock.
Curve current(Api&, const std::filesystem::path& directory);

// Stock records per GPU, at %ProgramData%/MechControl/VfCurve beside NvidiaOverrides. The tuner records them on its
// first use on a GPU; profiles only read them.
std::filesystem::path store_directory();
std::filesystem::path stock_path(const std::filesystem::path& directory, const Gpu&);
std::optional<Bytes> load_stock(const std::filesystem::path& directory, const Gpu&, const Bytes& info);
void save_stock(const std::filesystem::path& directory, const Gpu&, const Bytes& records);
}
