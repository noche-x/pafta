// SPDX-License-Identifier: GPL-2.0-only
#include "formats.hpp"

#include <bit>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pafta {
namespace {

// -----------------------------------------------------------------------------
// Basic binary reading and writing
// -----------------------------------------------------------------------------

constexpr std::uint32_t sbb_payload_class_mask = 0xe0000000U;
// BuildBuildingBlockRenderBuffers_1400799d0.c:128: marks a solid or buildblock cell.
constexpr std::uint32_t sbb_solid_payload_class = 0x40000000U;
// BuildBuildingBlockRenderBuffers_1400799d0.c:212: marks a single mesh.
constexpr std::uint32_t sbb_piece_payload_class = 0x80000000U;
constexpr std::uint32_t sbb_empty_interior = 0xffffU;

class BinaryReader {
public:
    explicit BinaryReader(const std::vector<std::uint8_t>& bytes) : bytes_{bytes} {}

    [[nodiscard]] std::uint32_t u32(std::size_t offset) const {
        require(offset, 4);
        return static_cast<std::uint32_t>(bytes_[offset]) |
               (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(bytes_[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bytes_[offset + 3]) << 24U);
    }

    [[nodiscard]] std::int32_t i32(std::size_t offset) const {
        return static_cast<std::int32_t>(u32(offset));
    }

    [[nodiscard]] std::uint16_t u16(std::size_t offset) const {
        require(offset, 2);
        return static_cast<std::uint16_t>(bytes_[offset]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset + 1]) << 8U);
    }

    [[nodiscard]] float f32(std::size_t offset) const {
        return std::bit_cast<float>(u32(offset));
    }

    [[nodiscard]] std::string ascii(std::size_t offset, std::size_t length) const {
        require(offset, length);
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto end = begin + static_cast<std::ptrdiff_t>(length);
        const auto nul = std::find(begin, end, 0);
        std::string value(begin, nul);
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }
        return value;
    }

    [[nodiscard]] std::size_t size() const {
        return bytes_.size();
    }

    void records(std::size_t offset, std::size_t count, std::size_t stride, std::string_view section) const {
        if (offset > bytes_.size() || count > (bytes_.size() - offset) / stride) {
            throw std::runtime_error("Truncated " + std::string{section});
        }
    }

private:
    void require(std::size_t offset, std::size_t length) const {
        if (offset > bytes_.size() || length > bytes_.size() - offset) {
            throw std::runtime_error("Unexpected end of binary file at byte " + std::to_string(offset));
        }
    }

    const std::vector<std::uint8_t>& bytes_;
};

class BinaryWriter {
public:
    explicit BinaryWriter(std::vector<std::uint8_t> bytes) : bytes_{std::move(bytes)} {}
    explicit BinaryWriter(std::size_t size) : bytes_(size) {}

    void u32(std::size_t offset, std::uint32_t value) {
        require(offset, 4);
        bytes_[offset] = static_cast<std::uint8_t>(value & 0xffU);
        bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        bytes_[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        bytes_[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    }

    void i32(std::size_t offset, std::int32_t value) {
        u32(offset, static_cast<std::uint32_t>(value));
    }

    void u16(std::size_t offset, std::uint16_t value) {
        require(offset, 2);
        bytes_[offset] = static_cast<std::uint8_t>(value & 0xffU);
        bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    }

    void f32(std::size_t offset, float value) {
        u32(offset, std::bit_cast<std::uint32_t>(value));
    }

    void ascii(std::size_t offset, std::size_t length, const std::string& value) {
        require(offset, length);
        std::fill(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length), std::uint8_t{0});
        const auto size = std::min(length, value.size());
        std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(size), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    [[nodiscard]] std::vector<std::uint8_t> bytes() && {
        return std::move(bytes_);
    }

    std::vector<std::uint8_t>& raw() {
        return bytes_;
    }

private:
    void require(std::size_t offset, std::size_t length) const {
        if (offset > bytes_.size() || length > bytes_.size() - offset) {
            throw std::runtime_error("write offset is outside the buffer");
        }
    }

    std::vector<std::uint8_t> bytes_;
};

// -----------------------------------------------------------------------------
// SBB, SBC, and SBL parsing helpers
// -----------------------------------------------------------------------------

std::vector<std::string> read_fixed_ascii_chunks(
    const BinaryReader& reader,
    std::size_t offset,
    std::size_t chunk_size,
    std::uint32_t count
) {
    std::vector<std::string> strings;
    reader.records(offset, count, chunk_size, "SBB name table");
    strings.reserve(count);

    for (std::uint32_t index = 0; index < count; ++index) {
        strings.push_back(reader.ascii(offset, chunk_size));
        offset += chunk_size;
    }

    return strings;
}

std::vector<std::string> read_counted_names(const BinaryReader& reader, std::size_t offset) {
    const auto count = reader.u32(offset);
    if (count > 1024U) throw std::runtime_error("SBB name table exceeds 1024 entries");
    return read_fixed_ascii_chunks(reader, offset + 4, 0x40, count);
}

SbbCellPayloads read_cell_payloads(const std::array<std::uint32_t, 6>& words) {
    SbbCellPayloads payloads;
    const auto exterior = words[SbbCell::exterior_block_word];
    const auto interior = words[SbbCell::interior_block_word];

    // SaveBuilding_SBB_1400548d0.decomp.txt:76-99 writes the exterior and interior
    // block words first, even though BuildingCell keeps cellType at +0x00 in memory.
    if ((exterior & sbb_payload_class_mask) == sbb_solid_payload_class) {
        payloads.push_back({PayloadKind::Block, exterior, exterior & 0xffffU});
    }

    for (const auto word : {exterior, interior}) {
        if (word == 0 || word == sbb_empty_interior) {
            continue;
        }
        const auto payload_class = word & sbb_payload_class_mask;
        if (payload_class == sbb_solid_payload_class) {
            continue;
        }
        payloads.push_back({
            payload_class == sbb_piece_payload_class ? PayloadKind::Piece : PayloadKind::Other,
            word,
            payload_class == sbb_piece_payload_class ? word & 0x3ffU : word & 0xffffU,
        });
    }

    return payloads;
}

std::vector<SbbCell> read_sbb_cells(
    const BinaryReader& reader,
    std::size_t offset,
    std::uint32_t size_x,
    std::uint32_t size_y,
    std::uint32_t size_z
) {
    std::vector<SbbCell> cells;
    const auto width = size_x + 1;
    const auto depth = size_y + 1;
    const auto height = size_z;
    const auto count = static_cast<std::size_t>(width) * depth * height;
    reader.records(offset, count, 0x3c, "SBB cell volume");
    cells.reserve(count);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::array<std::uint32_t, 6> words{};
                for (std::size_t index = 0; index < words.size(); ++index) {
                    words[index] = reader.u32(offset + index * 4);
                }
                std::array<std::uint16_t, 16> shorts{};
                for (std::size_t index = 0; index < shorts.size(); ++index) {
                    shorts[index] = reader.u16(offset + 24 + index * 2);
                }
                const auto item_set_index = reader.u32(offset + 56);
                const auto occupied = std::ranges::any_of(words, [](std::uint32_t word) {
                    return word != 0;
                });

                cells.push_back({
                    cells.size(),
                    x,
                    y,
                    z,
                    words,
                    item_set_index,
                    occupied,
                    read_cell_payloads(words),
                    shorts,
                });
                offset += 0x3c;
            }
        }
    }

    return cells;
}

bool sbc_has_city_blocks(std::uint32_t version) {
    return version >= 0x0b;
}

bool sbb_version_supported(std::uint32_t version) {
    return version >= 0x0c && version <= 0x0e;
}

bool sbc_version_supported(std::uint32_t version) {
    return version >= 7 && version <= 0x0c;
}

bool sbl_block_version_supported(std::uint32_t version) {
    return version >= 1 && version <= 2;
}

std::size_t sbb_table_offset(std::uint32_t version) {
    return version > 0x0c ? 0x60U : 0x54U;
}

std::size_t sbb_cell_count(const SbbBuilding& building) {
    if (!sbb_dimensions_supported(building.size_x, building.size_y, building.size_z)) {
        throw std::runtime_error("SBB dimensions exceed the supported 64-cell grid");
    }
    const auto width = building.size_x + 1;
    const auto depth = building.size_y + 1;
    const auto height = building.size_z;
    return static_cast<std::size_t>(width) * depth * height;
}

constexpr std::size_t max_csx_payload_bytes = 512U * 1024U * 1024U;
constexpr std::uint32_t max_sbl_buildblock_doors = 2;
constexpr std::uint32_t max_sbl_buildblock_rects = 16;

// -----------------------------------------------------------------------------
// Rebuilding SBL block meshes
// -----------------------------------------------------------------------------

std::uint32_t exterior_face_count(std::uint32_t width, std::uint32_t height, std::uint32_t depth, const std::array<std::uint32_t, 6>& flags) {
    auto count = 0U;
    if (flags[0] != 0) {
        count += depth * width;
    }
    if (flags[1] != 0) {
        count += depth * width;
    }
    if (flags[2] != 0) {
        count += height * width;
    }
    if (flags[3] != 0) {
        count += height * depth;
    }
    if (flags[4] != 0) {
        count += height * width;
    }
    if (flags[5] != 0) {
        count += height * depth;
    }
    return count;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_i32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value));
}

void append_f32(std::vector<std::uint8_t>& bytes, float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_ascii(std::vector<std::uint8_t>& bytes, std::size_t length, const std::string& value) {
    const auto offset = bytes.size();
    bytes.resize(offset + length);
    const auto size = std::min(length, value.size());
    std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(size), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<Vec3f> default_block_face_quad(std::uint32_t face_index) {
    constexpr auto inset = 0.0625F;
    constexpr auto top = 0.875F;
    constexpr auto one = 1.0F;

    if (face_index == 0) {
        return {{0.0F, inset, 0.0F}, {one, inset, 0.0F}, {one, inset, one}, {0.0F, inset, one}};
    }
    if (face_index == 5) {
        return {{0.0F, top, one}, {one, top, one}, {one, top, 0.0F}, {0.0F, top, 0.0F}};
    }

    auto quad = std::vector<Vec3f>{{0.0F, one, inset}, {one, one, inset}, {one, 0.0F, inset}, {0.0F, 0.0F, inset}};
    for (std::uint32_t index = 1; index < face_index; ++index) {
        for (auto& vertex : quad) {
            const auto next_x = one - vertex.z;
            const auto next_z = vertex.x;
            vertex.x = next_x;
            vertex.z = next_z;
        }
    }
    return quad;
}

std::uint32_t native_default_face_orientation(std::uint32_t face_dir) {
    switch (face_dir) {
        case 0:
            return 0x40000U;
        case 1:
            return 0x60000U;
        case 2:
            return 0x10000U;
        case 3:
            return 0x50000U;
        case 4:
            return 0x20000U;
        case 5:
            return 0x30000U;
        default:
            return 0U;
    }
}

void emit_exterior_plane(std::vector<SblBlockFace>& faces, std::uint32_t face_dir, const std::vector<std::array<std::uint32_t, 3>>& cells) {
    const auto base = default_block_face_quad(face_dir);
    const auto orientation = native_default_face_orientation(face_dir);
    for (const auto& cell : cells) {
        std::vector<Vec3f> vertices;
        std::vector<Vec2f> uvs;
        vertices.reserve(base.size());
        uvs.reserve(base.size());
        for (const auto& vertex : base) {
            const auto positioned = Vec3f{
                vertex.x + static_cast<float>(cell[0]),
                vertex.y + static_cast<float>(cell[1]),
                vertex.z + static_cast<float>(cell[2]),
            };
            vertices.push_back(positioned);
            uvs.push_back(native_oriented_uv(positioned, {}, orientation));
        }
        faces.push_back({face_dir, std::move(vertices), std::move(uvs), false, orientation});
    }
}

std::vector<std::array<std::uint32_t, 3>> grid_cells(std::uint32_t count, auto make_cell) {
    std::vector<std::array<std::uint32_t, 3>> cells;
    cells.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        cells.push_back(make_cell(index));
    }
    return cells;
}

std::vector<SblBlockFace> build_exterior_block_faces(std::uint32_t width, std::uint32_t height, std::uint32_t depth, const std::array<std::uint32_t, 6>& flags) {
    constexpr std::array<std::uint32_t, 6> face_dirs{0, 5, 3, 4, 1, 2};
    std::vector<SblBlockFace> faces;
    faces.reserve(exterior_face_count(width, height, depth, flags));

    if (flags[0] != 0) {
        emit_exterior_plane(faces, face_dirs[0], grid_cells(depth * width, [width](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{index % width, 0, index / width};
        }));
    }
    if (flags[1] != 0) {
        emit_exterior_plane(faces, face_dirs[1], grid_cells(depth * width, [width, height](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{index % width, height - 1, index / width};
        }));
    }
    if (flags[2] != 0) {
        emit_exterior_plane(faces, face_dirs[2], grid_cells(height * width, [width, depth](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{index % width, index / width, depth - 1};
        }));
    }
    if (flags[3] != 0) {
        emit_exterior_plane(faces, face_dirs[3], grid_cells(height * depth, [depth](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{0, index / depth, index % depth};
        }));
    }
    if (flags[4] != 0) {
        emit_exterior_plane(faces, face_dirs[4], grid_cells(height * width, [width](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{index % width, index / width, 0};
        }));
    }
    if (flags[5] != 0) {
        emit_exterior_plane(faces, face_dirs[5], grid_cells(height * depth, [depth, width](std::uint32_t index) {
            return std::array<std::uint32_t, 3>{width - 1, index / depth, index % depth};
        }));
    }

    return faces;
}

std::uint32_t clamp_bezier_tess(std::uint32_t tess) {
    return std::clamp(tess, 1U, 128U);
}

float bezier1d(std::uint32_t order, float t, const std::array<float, 4>& values) {
    // Only four control points are stored, so preview curves are at most cubic.
    const auto clamped_order = std::clamp(order, 1U, 3U);
    const auto inverse = 1.0F - t;
    if (clamped_order == 1U) {
        return values[0] * inverse + values[3] * t;
    }
    if (clamped_order == 2U) {
        return values[0] * inverse * inverse + values[1] * 2.0F * inverse * t + values[3] * t * t;
    }
    return values[0] * inverse * inverse * inverse +
        values[1] * 3.0F * inverse * inverse * t +
        values[2] * 3.0F * inverse * t * t +
        values[3] * t * t * t;
}

Vec3f bezier_patch_sample(
    const std::array<Vec3f, 16>& control_points,
    std::uint32_t column_order,
    std::uint32_t row_order,
    float a,
    float b) {
    auto component = [&](auto getter) {
        std::array<float, 4> intermediate{};
        for (std::size_t column = 0; column < 4; ++column) {
            const std::array<float, 4> row_values{
                getter(control_points[column]),
                getter(control_points[4 + column]),
                getter(control_points[8 + column]),
                getter(control_points[12 + column]),
            };
            intermediate[column] = bezier1d(row_order, b, row_values);
        }
        return bezier1d(column_order, a, intermediate);
    };

    return {
        component([](Vec3f value) { return value.x; }),
        component([](Vec3f value) { return value.y; }),
        component([](Vec3f value) { return value.z; }),
    };
}

Vec2f bezier_patch_uv_sample(
    const std::array<Vec2f, 16>& control_uvs,
    std::uint32_t column_order,
    std::uint32_t row_order,
    float a,
    float b) {
    auto component = [&](auto getter) {
        std::array<float, 4> intermediate{};
        for (std::size_t column = 0; column < 4; ++column) {
            const std::array<float, 4> row_values{
                getter(control_uvs[column]),
                getter(control_uvs[4 + column]),
                getter(control_uvs[8 + column]),
                getter(control_uvs[12 + column]),
            };
            intermediate[column] = bezier1d(row_order, b, row_values);
        }
        return bezier1d(column_order, a, intermediate);
    };

    return {
        component([](Vec2f value) { return value.u; }),
        component([](Vec2f value) { return value.v; }),
    };
}


Vec3f subtract(Vec3f left, Vec3f right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3f cross(Vec3f left, Vec3f right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

Vec3f normalize(Vec3f value) {
    const auto length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.000001F) {
        return {};
    }
    return {value.x / length, value.y / length, value.z / length};
}

std::uint32_t native_orientation_from_normal(Vec3f normal) {
    constexpr auto threshold = 0.5F;
    if (normal.y > threshold) {
        return 0x40000U;
    }
    if (normal.y < -threshold) {
        return 0x30000U;
    }
    if (normal.x > threshold) {
        return 0x20000U;
    }
    if (normal.x < -threshold) {
        return 0x10000U;
    }
    if (normal.z > threshold) {
        return 0x60000U;
    }
    return 0x50000U;
}

std::uint32_t native_patch_material_with_auto_orientation(std::uint32_t material, const std::array<Vec3f, 16>& control_points) {
    const auto normal = cross(
        subtract(control_points[15], control_points[0]),
        subtract(control_points[1], control_points[0]));
    return material | native_orientation_from_normal(normalize(normal));
}

void emit_bezier_patch_faces(
    std::vector<SblBlockFace>& faces,
    std::uint32_t material,
    std::uint32_t column_order,
    std::uint32_t column_tess,
    std::uint32_t row_order,
    std::uint32_t row_tess,
    const std::array<Vec3f, 16>& control_points,
    const std::array<Vec2f, 16>& control_uvs) {
    const auto columns = clamp_bezier_tess(column_tess);
    const auto rows = clamp_bezier_tess(row_tess);
    for (std::uint32_t row = 0; row < rows; ++row) {
        const auto b0 = static_cast<float>(row) / static_cast<float>(rows);
        const auto b1 = static_cast<float>(row + 1U) / static_cast<float>(rows);
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto a0 = static_cast<float>(column) / static_cast<float>(columns);
            const auto a1 = static_cast<float>(column + 1U) / static_cast<float>(columns);
            const std::array<Vec2f, 4> samples{{{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}}};
            std::vector<Vec3f> vertices;
            std::vector<Vec2f> uvs;
            vertices.reserve(samples.size());
            uvs.reserve(samples.size());
            for (const auto& sample : samples) {
                const auto position = bezier_patch_sample(control_points, column_order, row_order, sample.u, sample.v);
                const auto uv = bezier_patch_uv_sample(control_uvs, column_order, row_order, sample.u, sample.v);
                vertices.push_back(position);
                uvs.push_back(native_oriented_uv(position, uv, material));
            }
            faces.push_back({
                material & 0xffffU,
                std::move(vertices),
                std::move(uvs),
                true,
                material & 0xf0000U,
            });
        }
    }
}

void emit_sbl_box_faces(
    std::vector<SblBlockFace>& faces,
    const std::array<Vec3f, 8>& points,
    const std::array<std::uint32_t, 6>& scalars,
    std::uint32_t final_value) {
    constexpr std::array<std::array<std::size_t, 4>, 6> box_face_corners{{
        {{3, 2, 1, 0}},
        {{7, 6, 2, 3}},
        {{4, 7, 3, 0}},
        {{5, 4, 0, 1}},
        {{6, 5, 1, 2}},
        {{4, 5, 6, 7}},
    }};

    for (std::size_t face = 0; face < box_face_corners.size(); ++face) {
        if ((final_value & (1U << face)) == 0U) {
            continue;
        }
        const auto& corners = box_face_corners[face];
        // Boxes can be turned or stretched, so UV projection follows the normal
        // of each face made, not a fixed side of a cube.
        const auto edge_a = subtract(points[corners[1]], points[corners[0]]);
        const auto edge_b = subtract(points[corners[2]], points[corners[0]]);
        const auto orientation = native_orientation_from_normal(normalize(cross(edge_a, edge_b)));
        std::vector<Vec3f> vertices{points[corners[0]], points[corners[1]], points[corners[2]], points[corners[3]]};
        std::vector<Vec2f> uvs;
        uvs.reserve(vertices.size());
        for (const auto& vertex : vertices) {
            uvs.push_back(native_oriented_uv(vertex, {}, orientation));
        }
        faces.push_back({
            scalars[0] & 0xffffU,
            std::move(vertices),
            std::move(uvs),
            false,
            orientation,
        });
    }
}

std::uint32_t authored_render_face_count(const std::vector<SblAuthoredPatch>& patches) {
    auto count = 0U;
    for (const auto& patch : patches) {
        count += clamp_bezier_tess(patch.column_tess) * clamp_bezier_tess(patch.row_tess);
    }
    return count;
}

void append_authored_patch_faces(std::vector<SblBlockFace>& faces, const std::vector<SblAuthoredPatch>& patches) {
    for (const auto& patch : patches) {
        if (patch.control_points.size() < 16) {
            continue;
        }
        std::array<Vec3f, 16> control_points{};
        std::array<Vec2f, 16> control_uvs{};
        for (std::size_t index = 0; index < control_points.size(); ++index) {
            control_points[index] = patch.control_points[index].position;
            control_uvs[index] = {patch.control_points[index].u, patch.control_points[index].v};
        }
        emit_bezier_patch_faces(
            faces,
            patch.normal_flag_b == 0U ? patch.normal_flag_a : native_patch_material_with_auto_orientation(patch.normal_flag_a, control_points),
            patch.column_order,
            patch.column_tess,
            patch.row_order,
            patch.row_tess,
            control_points,
            control_uvs);
    }
}

std::optional<SblBlockMesh> read_sbl_block_mesh(const BinaryReader& reader) {
    if (reader.size() < 44) {
        return std::nullopt;
    }
    if (!sbl_block_version_supported(reader.u32(0))) {
        return std::nullopt;
    }

    const auto width = reader.u32(4);
    const auto height = reader.u32(8);
    const auto depth = reader.u32(12);
    if (width == 0 || height == 0 || depth == 0 || width > 0x40 || height > 0x40 || depth > 0x40) {
        return std::nullopt;
    }

    const std::array<std::uint32_t, 6> flags{
        reader.u32(16),
        reader.u32(20),
        reader.u32(24),
        reader.u32(28),
        reader.u32(32),
        reader.u32(36),
    };

    auto faces = build_exterior_block_faces(width, height, depth, flags);
    const auto authored_face_count = reader.u32(40);
    if (authored_face_count > 0x400U) throw std::runtime_error("SBL exceeds 1024 authored patches");
    reader.records(44, authored_face_count, 344, "SBL authored patches");
    std::vector<SblAuthoredPatch> authored_patches;
    authored_patches.reserve(authored_face_count);
    auto offset = std::size_t{44};
    for (std::uint32_t face = 0; face < authored_face_count; ++face) {
        const auto face_offset = offset;
        const auto column_order = reader.u32(offset);
        const auto column_tess = reader.u32(offset + 4);
        const auto row_order = reader.u32(offset + 8);
        const auto row_tess = reader.u32(offset + 12);
        const auto normal_flag_a = reader.u32(offset + 16);
        const auto normal_flag_b = reader.u32(offset + 20);
        offset += 24;

        std::array<Vec3f, 16> control_points{};
        std::array<Vec2f, 16> control_uvs{};
        std::vector<SblControlPoint> source_points;
        source_points.reserve(control_points.size());
        for (std::size_t point_index = 0; point_index < control_points.size(); ++point_index) {
            auto& point = control_points[point_index];
            point = {
                reader.f32(offset),
                reader.f32(offset + 4),
                reader.f32(offset + 8),
            };
            control_uvs[point_index] = {
                reader.f32(offset + 12),
                reader.f32(offset + 16),
            };
            source_points.push_back({
                point,
                control_uvs[point_index].u,
                control_uvs[point_index].v,
            });
            offset += 20;
        }
        authored_patches.push_back({
            face,
            face_offset,
            column_order,
            column_tess,
            row_order,
            row_tess,
            normal_flag_a,
            normal_flag_b,
            std::move(source_points),
        });
        emit_bezier_patch_faces(
            faces,
            normal_flag_b == 0U ? normal_flag_a : native_patch_material_with_auto_orientation(normal_flag_a, control_points),
            column_order,
            column_tess,
            row_order,
            row_tess,
            control_points,
            control_uvs);
    }

    auto light_count = 0U;
    std::vector<SblBlockLight> lights;
    if (reader.u32(0) > 1) {
        light_count = reader.u32(offset);
        if (light_count > 0x100U) throw std::runtime_error("SBL exceeds 256 boxes");
        offset += 4;
        reader.records(offset, light_count, 0x7c, "SBL boxes");
        lights.reserve(light_count);
        for (std::uint32_t light = 0; light < light_count; ++light) {
            std::array<Vec3f, 8> points{};
            for (auto& point : points) {
                point = {
                    reader.f32(offset),
                    reader.f32(offset + 4),
                    reader.f32(offset + 8),
                };
                offset += 12;
            }
            std::array<std::uint32_t, 6> scalars{};
            for (auto& scalar : scalars) {
                scalar = reader.u32(offset);
                offset += 4;
            }
            const auto final_value = reader.u32(offset);
            offset += 4;
            lights.push_back({points, scalars, final_value});
            emit_sbl_box_faces(faces, points, scalars, final_value);
        }
    }

    return SblBlockMesh{
        width,
        height,
        depth,
        flags,
        static_cast<std::uint32_t>(faces.size()),
        light_count,
        std::move(faces),
        std::move(authored_patches),
        std::move(lights),
    };
}

std::vector<SblDoor> read_sbl_doors(const BinaryReader& reader, std::size_t offset, std::uint32_t count) {
    std::vector<SblDoor> doors;
    reader.records(offset, count, 12, "SBL doors");
    doors.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        doors.push_back({
            reader.u32(offset),
            reader.f32(offset + 4),
            reader.f32(offset + 8),
        });
        offset += 12;
    }
    return doors;
}

std::vector<SblPortalRect> read_sbl_rects(const BinaryReader& reader, std::size_t offset, std::uint32_t count) {
    std::vector<SblPortalRect> rects;
    reader.records(offset, count, 20, "SBL rectangles");
    rects.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        rects.push_back({
            reader.u32(offset),
            reader.f32(offset + 4),
            reader.f32(offset + 8),
            reader.f32(offset + 12),
            reader.f32(offset + 16),
        });
        offset += 20;
    }
    return rects;
}

std::vector<SblLayer> read_sbl_layers(const BinaryReader& reader) {
    std::vector<SblLayer> layers;
    auto offset = std::size_t{4};

    for (std::size_t index = 0; offset < reader.size(); ++index) {
        if (index >= 16) throw std::runtime_error("SBL buildblock exceeds 16 layers");
        reader.records(offset, 1, 0x18, "SBL buildblock layer");
        const auto layer_offset = offset;
        const auto portal_type = reader.u32(offset);
        const std::array<float, 4> bounds{
            reader.f32(offset + 4),
            reader.f32(offset + 8),
            reader.f32(offset + 12),
            reader.f32(offset + 16),
        };
        const auto door_count = reader.u32(offset + 20);
        if (door_count > max_sbl_buildblock_doors) throw std::runtime_error("SBL layer exceeds 2 doors");
        offset += 0x18;

        auto doors = read_sbl_doors(reader, offset, door_count);
        offset += doors.size() * 12;

        const auto window_count = reader.u32(offset);
        if (window_count > max_sbl_buildblock_rects) throw std::runtime_error("SBL layer exceeds 16 windows");
        offset += 4;
        auto windows = read_sbl_rects(reader, offset, window_count);
        offset += windows.size() * 20;

        const auto beam_count = reader.u32(offset);
        if (beam_count > max_sbl_buildblock_rects) throw std::runtime_error("SBL layer exceeds 16 beams");
        offset += 4;
        auto beams = read_sbl_rects(reader, offset, beam_count);
        offset += beams.size() * 20;

        layers.push_back({
            index,
            layer_offset,
            portal_type,
            bounds,
            door_count,
            std::move(doors),
            window_count,
            std::move(windows),
            beam_count,
            std::move(beams),
        });
    }

    return layers;
}

CsxEntryKind classify_csx_entry(std::uint32_t type) {
    switch (type) {
        case 0x585a0001:
            // Block or buildblock .sbl (parse_sbl). CSX stores these with container
            // version 1; load_project_csx_geometry changes 1 to 2.
            return CsxEntryKind::Sbl;
        case 0x585a0002:
            // Building .sbb. CSX stores it with container version 1;
            // load_project_csx_geometry changes 1 to 14 before parse_sbb.
            return CsxEntryKind::Sbb;
        case 0x585a0004:
            return CsxEntryKind::Texture;
        default:
            return CsxEntryKind::Unknown;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Public parse functions
// -----------------------------------------------------------------------------

Vec2f native_oriented_uv(Vec3f position, Vec2f fallback, std::uint32_t orientation) {
    switch (orientation & 0xf0000U) {
        case 0x10000U:
            return {position.z, position.y};
        case 0x20000U:
            return {1.0F - position.z, position.y};
        case 0x30000U:
            return {position.x, position.z};
        case 0x40000U:
            return {position.x, 1.0F - position.z};
        case 0x50000U:
            return {1.0F - position.x, position.y};
        case 0x60000U:
            return {position.x, position.y};
        default:
            return fallback;
    }
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Could not open " + path.string());
    }

    // One big read avoids slow byte-by-byte stream work on large map archives.
    const auto end = file.tellg();
    if (end < 0) {
        throw std::runtime_error("Could not size " + path.string());
    }
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw std::runtime_error("Could not read complete file: " + path.string());
    }
    return bytes;
}

void write_files_transaction(std::span<const FileWrite> files) {
    struct StagedFile {
        std::filesystem::path target;
        std::filesystem::path directory;
        bool backed_up{};
        bool installed{};
    };
    std::vector<StagedFile> staged;
    staged.reserve(files.size());
    try {
        for (const auto& output : files) {
            const auto target = std::filesystem::absolute(output.path).lexically_normal();
            const auto duplicate = std::ranges::any_of(staged, [&](const auto& entry) { return entry.target == target; });
            if (duplicate) {
                throw std::runtime_error("Duplicate output file: " + target.string());
            }
            const auto status = std::filesystem::symlink_status(target);
            if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
                throw std::runtime_error("Output is not a regular file: " + target.string());
            }
            std::filesystem::create_directories(target.parent_path());
            // A folder next to the target that only this save creates keeps temporary
            // names from clashing with another save and keeps each rename on one volume.
            std::filesystem::path directory;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                directory = target.parent_path() / ("." + target.filename().string() + ".pafta-save-" + std::to_string(stamp));
                if (std::filesystem::create_directory(directory)) {
                    break;
                }
                directory.clear();
            }
            if (directory.empty()) {
                throw std::runtime_error("Could not create staging directory for " + target.string());
            }
            staged.push_back({target, directory});
            std::ofstream file{directory / "new", std::ios::binary | std::ios::trunc};
            if (!file) {
                throw std::runtime_error("Could not open staged output: " + target.string());
            }
            file.write(reinterpret_cast<const char*>(output.bytes.data()), static_cast<std::streamsize>(output.bytes.size()));
            if (!file) {
                throw std::runtime_error("Could not write staged output: " + target.string());
            }
            file.flush();
            if (!file) {
                throw std::runtime_error("Could not flush staged output: " + target.string());
            }
            file.close();
            if (!file) {
                throw std::runtime_error("Could not close staged output: " + target.string());
            }
        }
        for (auto& entry : staged) {
            if (std::filesystem::exists(entry.target)) {
                std::filesystem::rename(entry.target, entry.directory / "old");
                entry.backed_up = true;
            }
            std::filesystem::rename(entry.directory / "new", entry.target);
            entry.installed = true;
        }
    } catch (const std::exception& error) {
        std::string recovery_error;
        for (auto entry = staged.rbegin(); entry != staged.rend(); ++entry) {
            std::error_code failure;
            if (entry->installed) {
                std::filesystem::remove(entry->target, failure);
                if (failure) {
                    recovery_error += "; could not remove " + entry->target.string() + ": " + failure.message();
                }
            }
            if (entry->backed_up) {
                std::filesystem::rename(entry->directory / "old", entry->target, failure);
                if (failure) {
                    recovery_error += "; restore " + (entry->directory / "old").string() + " to " + entry->target.string() + ": " + failure.message();
                    continue; // Preserve the recovery copy.
                }
            }
            std::filesystem::remove_all(entry->directory, failure);
            if (failure) {
                recovery_error += "; could not remove staging directory " + entry->directory.string() + ": " + failure.message();
            }
        }
        throw std::runtime_error(std::string{error.what()} + recovery_error);
    }
    // Every replacement worked. Cleanup errors must not start a rollback after another
    // file's recovery copy has already been removed.
    for (const auto& entry : staged) {
        std::error_code failure;
        std::filesystem::remove_all(entry.directory, failure);
        if (failure) {
            throw std::runtime_error("Files were saved, but could not remove recovery directory " + entry.directory.string() + ": " + failure.message());
        }
    }
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    const FileWrite output{path, bytes};
    write_files_transaction(std::span{&output, 1});
}

SbbBuilding parse_sbb(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    if (reader.size() < 0x50) {
        throw std::runtime_error("SBB is too small");
    }

    SbbBuilding building;
    building.version = reader.u32(0);
    if (!sbb_version_supported(building.version)) {
        throw std::runtime_error(
            "Unsupported SBB version " + std::to_string(building.version) + " (supported 12-14)");
    }
    building.name = reader.ascii(4, 0x40);
    building.size_x = reader.u32(0x44);
    building.size_y = reader.u32(0x48);
    building.size_z = reader.u32(0x4c);
    if (!sbb_dimensions_supported(building.size_x, building.size_y, building.size_z)) {
        throw std::runtime_error("SBB dimensions exceed the supported 64-cell grid");
    }
    building.anchor_x = building.version > 0x0c ? reader.u32(0x50) : 4;
    building.anchor_y = building.version > 0x0c ? reader.u32(0x54) : 4;
    building.anchor_z = building.version > 0x0c ? reader.u32(0x58) : 4;
    building.extent_x = std::max(1U, building.size_x - std::min(building.size_x, building.anchor_x));
    building.extent_y = std::max(1U, building.size_z - std::min(building.size_z, building.anchor_y));
    building.extent_z = std::max(1U, building.size_y - std::min(building.size_y, building.anchor_z));

    const auto table_offset = sbb_table_offset(building.version);
    const auto material_count = reader.u32(table_offset - 4U);
    building.materials = read_fixed_ascii_chunks(reader, table_offset, 0x40, material_count);

    auto offset = table_offset + static_cast<std::size_t>(material_count) * 0x40;
    building.piece_names = read_counted_names(reader, offset);
    offset += 4 + building.piece_names.size() * 0x40;
    building.block_names = read_counted_names(reader, offset);
    offset += 4 + building.block_names.size() * 0x40;
    building.item_set_names = read_counted_names(reader, offset);
    offset += 4 + building.item_set_names.size() * 0x40;
    building.cells = read_sbb_cells(reader, offset, building.size_x, building.size_y, building.size_z);
    offset += sbb_cell_count(building) * 0x3c;

    if (building.version >= 0x0e && offset < reader.size()) {
        building.lod_tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
        if (offset + 4 <= reader.size()) building.lod_marker = reader.u32(offset);
        if (building.lod_marker == 1 && offset + 8 <= reader.size()) {
            building.lod_group_count = reader.u32(offset + 4);
        }
    }

    return building;
}

SbbBuilding parse_sbb_file(const std::filesystem::path& path) {
    return parse_sbb(read_file_bytes(path));
}

std::int32_t sbc_portal_cell_base(std::int32_t sector) {
    const auto base = static_cast<std::int64_t>(sector) * 8;
    if (base < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) + 1 ||
        base > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) - 8) {
        throw std::runtime_error("SBC portal coordinate exceeds the safe cell range: " + std::to_string(sector));
    }
    return static_cast<std::int32_t>(base);
}

SbcMap parse_sbc(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    auto offset = std::size_t{0};

    SbcMap map;
    map.version = reader.u32(offset);
    if (!sbc_version_supported(map.version)) {
        throw std::runtime_error("Unsupported SBC version");
    }
    offset += 4;

    if (map.version > 9) {
        const auto count = reader.u32(offset);
        offset += 4;
        if (count > (bytes.size() - offset) / 64) throw std::runtime_error("Truncated SBC header table");
        for (std::uint32_t i = 0; i < count; ++i) {
            std::array<std::uint8_t, 64> record{};
            std::copy_n(bytes.begin() + offset, 64, record.begin());
            map.header_records.push_back(record);
            offset += 64;
        }
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4;
    reader.records(offset, intersection_count, 12, "SBC intersections");
    map.intersections.reserve(intersection_count);
    for (std::uint32_t index = 0; index < intersection_count; ++index) {
        map.intersections.push_back({
            index,
            reader.i32(offset),
            reader.i32(offset + 4),
            reader.i32(offset + 8),
        });
        offset += 12;
    }

    const auto street_count = reader.u32(offset);
    offset += 4;
    reader.records(offset, street_count, 0x34, "SBC streets");
    map.streets.reserve(street_count);
    for (std::uint32_t index = 0; index < street_count; ++index) {
        map.streets.push_back({
            index,
            reader.i32(offset),
            reader.i32(offset + 4),
            reader.i32(offset + 8),
            reader.i32(offset + 12),
            reader.i32(offset + 16),
            reader.ascii(offset + 20, 0x20),
        });
        offset += 0x34;
    }

    const auto building_count = reader.u32(offset);
    offset += 4;
    reader.records(offset, building_count, 0x50, "SBC buildings");
    map.buildings.reserve(building_count);
    for (std::uint32_t index = 0; index < building_count; ++index) {
        map.buildings.push_back({
            index,
            reader.ascii(offset, 0x40),
            reader.i32(offset + 0x40),
            reader.i32(offset + 0x44),
            reader.i32(offset + 0x48),
            reader.i32(offset + 0x4c),
        });
        offset += 0x50;
    }

    if (sbc_has_city_blocks(map.version)) {
        const auto city_block_count = reader.u32(offset);
        offset += 4;
        reader.records(offset, city_block_count, map.version >= 0x0c ? 0x44 : 0x240, "SBC city blocks");
        map.city_blocks.reserve(city_block_count);
        for (std::uint32_t index = 0; index < city_block_count; ++index) {
            const auto name = reader.ascii(offset, 0x40);
            offset += 0x40;

            std::uint32_t layer_count = 1;
            if (map.version >= 0x0c) {
                layer_count = reader.u32(offset);
                offset += 4;
            }

            if (layer_count > 64) throw std::runtime_error("SBC city block exceeds 64 layers");
            reader.records(offset, layer_count, 8 * 0x40, "SBC city block layers");
            const auto layer_bytes = static_cast<std::size_t>(layer_count) * 8 * 0x40;
            std::vector<std::array<std::string, 8>> layers;
            layers.reserve(layer_count);
            for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
                std::array<std::string, 8> slots{};
                for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                    slots[slot] = reader.ascii(offset + static_cast<std::size_t>(layer) * 8 * 0x40 + slot * 0x40, 0x40);
                }
                layers.push_back(std::move(slots));
            }
            offset += layer_bytes;
            map.city_blocks.push_back({index, name, std::move(layers)});
        }
    }

    if (map.version > 7) {
        const auto portal_count = reader.u32(offset);
        offset += 4;
        const auto portal_stride = map.version > 0x0a ? 0x1010U : map.version > 9 ? 0x3010U : 0x2810U;
        reader.records(offset, portal_count, portal_stride, "SBC portals");
        map.portals.reserve(portal_count);
        for (std::uint32_t index = 0; index < portal_count; ++index) {
            const auto fields = std::array<std::int32_t, 4>{
                reader.i32(offset),
                reader.i32(offset + 4),
                reader.i32(offset + 8),
                reader.i32(offset + 12),
            };
            for (std::size_t axis = 1; axis < fields.size(); ++axis) sbc_portal_cell_base(fields[axis]);
            offset += 16;

            map.portals.push_back({
                index,
                fields,
                {},
                {},
                {},
                {},
            });

            auto& portal = map.portals.back();
            if (map.version > 0x0a) {
                portal.blocks.reserve(0x200);
                for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                    portal.blocks.push_back(reader.u32(offset + block_index * 4U));
                }
                offset += 0x800;
            }

            if (map.version < 0x0b) {
                portal.legacy_blocks.reserve(0x200);
                for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                    portal.legacy_blocks.push_back(reader.u32(offset + block_index * 4U));
                }
                offset += 0x800;

                portal.textures.reserve(0x1000);
                for (std::size_t texture_index = 0; texture_index < 0x1000; ++texture_index) {
                    portal.textures.push_back(reader.u16(offset + texture_index * 2U));
                }
                offset += 0x2000;
            }

            if (map.version > 9) {
                portal.itemsets.reserve(0x200);
                for (std::size_t itemset_index = 0; itemset_index < 0x200; ++itemset_index) {
                    portal.itemsets.push_back(reader.u32(offset + itemset_index * 4U));
                }
                offset += 0x800;
            }
        }
    }

    if (map.version > 8) {
        const auto connection_count = reader.u32(offset);
        offset += 4;
        reader.records(offset, connection_count, 12, "SBC portal connections");
        map.portal_connections.reserve(connection_count);
        for (std::uint32_t index = 0; index < connection_count; ++index) {
            map.portal_connections.push_back({
                index,
                {reader.i32(offset), reader.i32(offset + 4), reader.i32(offset + 8)},
            });
            offset += 12;
        }
    }

    return map;
}

SbcMap parse_sbc_file(const std::filesystem::path& path) {
    return parse_sbc(read_file_bytes(path));
}

SblTemplate parse_sbl(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    const auto block = read_sbl_block_mesh(reader);
    return {
        reader.u32(0),
        block,
        block.has_value() ? std::vector<SblLayer>{} : read_sbl_layers(reader),
    };
}

SblTemplate parse_sbl_file(const std::filesystem::path& path) {
    return parse_sbl(read_file_bytes(path));
}

void refresh_sbl_block_faces(SblBlockMesh& block) {
    const auto exterior_count = exterior_face_count(block.width, block.height, block.depth, block.exterior_flags);
    const auto authored_count = authored_render_face_count(block.authored_patches);
    std::vector<SblBlockFace> preserved_box_faces;
    const auto box_begin = static_cast<std::size_t>(exterior_count) + static_cast<std::size_t>(authored_count);
    // Box faces come from the box records. A block with no records (from the game's
    // built-in catalog) keeps the box faces it was built with.
    if (block.lights.empty() && box_begin < block.faces.size()) {
        preserved_box_faces.assign(block.faces.begin() + static_cast<std::ptrdiff_t>(box_begin), block.faces.end());
    }

    auto faces = build_exterior_block_faces(block.width, block.height, block.depth, block.exterior_flags);
    append_authored_patch_faces(faces, block.authored_patches);
    for (const auto& box : block.lights) {
        emit_sbl_box_faces(faces, box.points, box.scalars, box.final_value);
    }
    faces.insert(faces.end(), preserved_box_faces.begin(), preserved_box_faces.end());
    block.face_count = static_cast<std::uint32_t>(faces.size());
    block.faces = std::move(faces);
}

CsxArchive parse_csx(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    CsxArchive archive{
        reader.u32(0),
        reader.u32(4),
        reader.u32(8),
        {},
    };
    if (archive.directory_offset < 12) throw std::runtime_error("CSX directory overlaps its header");
    reader.records(archive.directory_offset, archive.entry_count, 0x40, "CSX directory");
    // Bound all copies before allocating any payload. Repeated references must not
    // multiply a small input into a much larger in-memory archive.
    const auto budget = std::min(bytes.size(), max_csx_payload_bytes);
    std::size_t payload_total = 0;
    for (std::uint32_t index = 0; index < archive.entry_count; ++index) {
        const auto entry = static_cast<std::size_t>(archive.directory_offset) + static_cast<std::size_t>(index) * 0x40;
        const auto offset = reader.u32(entry + 4);
        const auto size = reader.u32(entry + 8);
        reader.records(offset, size, 1, "CSX entry payload");
        if (size > budget - payload_total) {
            throw std::runtime_error("CSX payload copies exceed the file size or the 512 MiB allocation limit");
        }
        payload_total += size;
    }
    archive.entries.reserve(archive.entry_count);

    for (std::uint32_t index = 0; index < archive.entry_count; ++index) {
        const auto directory_entry = static_cast<std::size_t>(archive.directory_offset) + static_cast<std::size_t>(index) * 0x40;
        const auto type = reader.u32(directory_entry);
        const auto payload_offset = reader.u32(directory_entry + 4);
        const auto payload_size = reader.u32(directory_entry + 8);
        std::vector<std::uint8_t> payload;
        reader.records(payload_offset, payload_size, 1, "CSX entry payload");
        payload.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(payload_offset) + payload_size)
        );

        archive.entries.push_back({
            index,
            classify_csx_entry(type),
            type,
            payload_offset,
            payload_size,
            reader.ascii(directory_entry + 12, 0x34),
            std::move(payload),
        });
    }

    return archive;
}

std::vector<SisItem> parse_sis(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    const auto read_vector = [&](std::size_t offset) {
        return Vec3f{reader.f32(offset), reader.f32(offset + 4), reader.f32(offset + 8)};
    };
    // Version (1), count, then for each item a 64-byte type name, position, and 3
    // rotation rows.
    const auto count = reader.u32(4);
    if (count > 4096U) throw std::runtime_error("SIS exceeds 4096 items");
    reader.records(8, count, 112, "SIS items");
    std::vector<SisItem> items(count);
    auto offset = std::size_t{8};
    for (auto& item : items) {
        item.type = reader.ascii(offset, 64);
        item.pos = read_vector(offset + 64);
        item.rot = {read_vector(offset + 76), read_vector(offset + 88), read_vector(offset + 100)};
        offset += 112;
    }
    return items;
}

It3Mesh parse_it3(const std::vector<std::uint8_t>& bytes) {
    const BinaryReader reader{bytes};
    It3Mesh mesh;
    // Version 3, chunk id 0, render sub-version, then vertices and faces.
    if (reader.i32(0) != 3 || reader.i32(4) != 0) {
        return mesh;
    }
    auto offset = std::size_t{16};
    const auto vertex_count = reader.u32(12);
    if (vertex_count > 65536U) throw std::runtime_error("IT3 exceeds 65536 vertices");
    reader.records(offset, vertex_count, 12, "IT3 vertices");
    mesh.vertices.resize(vertex_count);
    for (auto& vertex : mesh.vertices) {
        vertex = {reader.f32(offset), reader.f32(offset + 4), reader.f32(offset + 8)};
        offset += 12;
    }
    const auto face_count = reader.u32(offset);
    if (face_count > 65536U) throw std::runtime_error("IT3 exceeds 65536 faces");
    offset += 4;
    reader.records(offset, face_count, 4, "IT3 face headers");
    mesh.faces.resize(face_count);
    for (auto& face : mesh.faces) {
        const auto corner_count = reader.u32(offset);
        if (corner_count > 64U) throw std::runtime_error("IT3 face exceeds 64 corners");
        offset += 4;
        reader.records(offset, corner_count, 12, "IT3 face corners");
        face.resize(corner_count);
        // Each corner: the vertex index, then its UV.
        for (auto& index : face) {
            index = reader.u32(offset);
            if (index >= mesh.vertices.size()) throw std::runtime_error("IT3 face references a missing vertex");
            offset += 12;
        }
    }
    return mesh;
}

std::optional<FileRepair> repair_sbb(const std::vector<std::uint8_t>& bytes) {
    try {
        const BinaryReader reader{bytes};
        if (reader.size() < 0x50) return std::nullopt;
        const auto version = reader.u32(0);
        if (!sbb_version_supported(version)) return std::nullopt;
        SbbBuilding sizes;
        sizes.size_x = reader.u32(0x44);
        sizes.size_y = reader.u32(0x48);
        sizes.size_z = reader.u32(0x4c);
        if (!sbb_dimensions_supported(sizes.size_x, sizes.size_y, sizes.size_z)) return std::nullopt;
        // Walk the tables the way parse_sbb does; they must be complete.
        const auto table_offset = sbb_table_offset(version);
        const auto material_count = reader.u32(table_offset - 4U);
        if (material_count > 1024U) return std::nullopt;
        auto offset = table_offset + static_cast<std::size_t>(material_count) * 0x40;
        for (int table = 0; table < 3; ++table) {
            const auto count = reader.u32(offset);
            if (count > 1024U) return std::nullopt;
            offset += 4 + static_cast<std::size_t>(count) * 0x40;
        }
        if (offset > reader.size()) return std::nullopt;
        const auto cells_end = offset + sbb_cell_count(sizes) * 0x3c;
        if (reader.size() >= cells_end) return std::nullopt;
        const auto missing_bytes = cells_end - reader.size();
        // Read it the way editor.exe does (the missing cells come out empty), then
        // save it the way editor.exe does: version 14, every cell, and the end marker
        // (1, then a group count of 0).
        auto padded = bytes;
        padded.resize(cells_end, 0);
        const auto missing_cells = (missing_bytes + 0x3b) / 0x3c;
        FileRepair repair{rebuild_sbb(parse_sbb(padded)), {}};
        repair.description = "resaved like editor.exe does: " + std::to_string(missing_cells) +
            (missing_cells == 1 ? " empty cell" : " empty cells") + " added where the file stopped early, version 14, and the end marker";
        return repair;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

CsxArchive parse_csx_file(const std::filesystem::path& path) {
    return parse_csx(read_file_bytes(path));
}

// -----------------------------------------------------------------------------
// Patch and rebuild writers
// -----------------------------------------------------------------------------

static void write_sbb_cell(BinaryWriter& writer, const SbbBuilding& building, const SbbCell& cell) {
    const auto cell_volume_offset = sbb_table_offset(building.version)
        + building.materials.size() * 0x40
        + 4 + building.piece_names.size() * 0x40
        + 4 + building.block_names.size() * 0x40
        + 4 + building.item_set_names.size() * 0x40;
    if (cell.index >= sbb_cell_count(building) || cell_volume_offset > writer.raw().size() ||
        cell.index >= (writer.raw().size() - cell_volume_offset) / 0x3c)
        throw std::runtime_error("SBB cell index exceeds file data");
    const auto cell_offset = cell_volume_offset + cell.index * 0x3c;

    for (std::size_t index = 0; index < cell.words.size(); ++index) {
        writer.u32(cell_offset + index * 4, cell.words[index]);
    }
    for (std::size_t index = 0; index < cell.shorts.size(); ++index) {
        writer.u16(cell_offset + 24 + index * 2, cell.shorts[index]);
    }
    writer.u32(cell_offset + 56, cell.item_set_index);
}

std::vector<std::uint8_t> patch_sbb_cell(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building, const SbbCell& cell) {
    BinaryWriter writer{bytes};
    write_sbb_cell(writer, building, cell);
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbb_cells(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building) {
    BinaryWriter writer{bytes};
    for (const auto& cell : building.cells) {
        write_sbb_cell(writer, building, cell);
    }
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> rebuild_sbb(const SbbBuilding& building) {
    if (!sbb_dimensions_supported(building.size_x, building.size_y, building.size_z)) {
        throw std::runtime_error("SBB dimensions exceed the supported 64-cell grid");
    }
    auto size_x = building.size_x;
    auto size_y = building.size_y;
    auto size_z = building.size_z;
    for (const auto& cell : building.cells) {
        if (cell.exterior_block() == 0 && cell.interior_block() == 0) {
            continue;
        }
        if (cell.x >= supported_sbb_grid_side || cell.y >= supported_sbb_grid_side || cell.z >= supported_sbb_grid_side) {
            throw std::runtime_error("SBB occupied cell lies outside the supported 64-cell grid");
        }
        // X and Z sizes are the highest index (the grid below runs from 0 to size,
        // both included); Y is the number of floors.
        size_x = std::max(size_x, cell.x);
        size_y = std::max(size_y, cell.z);
        size_z = std::max(size_z, cell.y + 1U);
    }

    std::vector<std::uint8_t> rebuilt;
    rebuilt.reserve(
        0x60
        + (building.materials.size() + building.piece_names.size() + building.block_names.size() + building.item_set_names.size()) * 0x40
        + static_cast<std::size_t>(size_x + 1U) * static_cast<std::size_t>(size_y + 1U) * static_cast<std::size_t>(size_z) * 0x3c
        + building.lod_tail.size()
        + 8U);

    append_u32(rebuilt, 0x0e);
    append_ascii(rebuilt, 0x40, building.name);
    append_u32(rebuilt, size_x);
    append_u32(rebuilt, size_y);
    append_u32(rebuilt, size_z);
    append_u32(rebuilt, building.anchor_x);
    append_u32(rebuilt, building.anchor_y);
    append_u32(rebuilt, building.anchor_z);

    auto append_name_table = [&rebuilt](const std::vector<std::string>& names) {
        append_u32(rebuilt, static_cast<std::uint32_t>(names.size()));
        for (const auto& name : names) {
            append_ascii(rebuilt, 0x40, name);
        }
    };
    append_name_table(building.materials);
    append_name_table(building.piece_names);
    append_name_table(building.block_names);
    append_name_table(building.item_set_names);

    // Each position in the file has one cell. When a position repeats, the first model
    // cell wins. Keep that order when building the lookup grid.
    const auto stride_x = static_cast<std::size_t>(size_x) + 1U;
    const auto stride_z = static_cast<std::size_t>(size_y) + 1U;
    std::vector<const SbbCell*> grid(stride_x * stride_z * size_z, nullptr);
    for (const auto& cell : building.cells) {
        if (cell.x > size_x || cell.z > size_y || cell.y >= size_z) continue;
        auto& slot = grid[(static_cast<std::size_t>(cell.y) * stride_z + cell.z) * stride_x + cell.x];
        if (slot == nullptr) slot = &cell;
    }
    for (std::uint32_t y = 0; y < size_z; ++y) {
        for (std::uint32_t z = 0; z <= size_y; ++z) {
            for (std::uint32_t x = 0; x <= size_x; ++x) {
                const auto* match = grid[(static_cast<std::size_t>(y) * stride_z + z) * stride_x + x];
                if (match == nullptr) {
                    rebuilt.resize(rebuilt.size() + 0x3c);
                    continue;
                }
                for (const auto word : match->words) {
                    append_u32(rebuilt, word);
                }
                for (const auto value : match->shorts) {
                    append_u16(rebuilt, value);
                }
                append_u32(rebuilt, match->item_set_index);
            }
        }
    }

    if (!building.lod_tail.empty()) {
        rebuilt.insert(rebuilt.end(), building.lod_tail.begin(), building.lod_tail.end());
    } else {
        append_u32(rebuilt, 1);
        append_u32(rebuilt, 0);
    }
    return rebuilt;
}

std::vector<std::uint8_t> patch_sbc_building(const std::vector<std::uint8_t>& bytes, const SbcBuilding& building, std::uint32_t version) {
    BinaryReader reader{bytes};
    BinaryWriter writer{bytes};
    auto offset = std::size_t{4};

    if (version > 9) {
        const auto record_count = reader.u32(offset);
        offset += 4 + static_cast<std::size_t>(record_count) * 0x40;
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(intersection_count) * 12;

    const auto street_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(street_count) * 0x34;

    offset += 4 + building.index * 0x50;
    if (offset + 0x50 > bytes.size()) {
        return std::move(writer).bytes();
    }

    if (reader.ascii(offset, 0x40) != building.name) writer.ascii(offset, 0x40, building.name);
    writer.i32(offset + 0x40, building.x);
    writer.i32(offset + 0x44, building.y);
    writer.i32(offset + 0x48, building.z);
    writer.i32(offset + 0x4c, building.rotation);
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbc_streets(const std::vector<std::uint8_t>& bytes, const SbcMap& map) {
    BinaryReader reader{bytes};
    BinaryWriter writer{bytes};
    auto offset = std::size_t{4};

    if (map.version > 9) {
        const auto record_count = reader.u32(offset);
        offset += 4 + static_cast<std::size_t>(record_count) * 0x40;
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(intersection_count) * 12;

    const auto street_count = reader.u32(offset);
    offset += 4;
    const auto count = std::min<std::size_t>(street_count, map.streets.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& street = map.streets[index];
        const auto street_offset = offset + street.index * 0x34;
        if (street_offset + 0x34 > bytes.size()) {
            break;
        }

        writer.i32(street_offset, street.from_intersection);
        writer.i32(street_offset + 4, street.to_intersection);
        writer.i32(street_offset + 8, street.district);
        writer.i32(street_offset + 12, street.left_width);
        writer.i32(street_offset + 16, street.right_width);
        if (reader.ascii(street_offset + 20, 0x20) != street.name)
            writer.ascii(street_offset + 20, 0x20, street.name);
    }

    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbc_buildings(const std::vector<std::uint8_t>& bytes, const SbcMap& map) {
    auto patched = bytes;
    for (const auto& building : map.buildings) {
        patched = patch_sbc_building(patched, building, map.version);
    }
    return patched;
}

std::vector<std::uint8_t> patch_sbc_city_blocks(const std::vector<std::uint8_t>& bytes, const SbcMap& map) {
    BinaryReader reader{bytes};
    BinaryWriter writer{bytes};
    auto offset = std::size_t{4};

    if (map.version > 9) {
        const auto record_count = reader.u32(offset);
        offset += 4 + static_cast<std::size_t>(record_count) * 0x40;
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(intersection_count) * 12;

    const auto street_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(street_count) * 0x34;

    const auto building_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(building_count) * 0x50;

    if (!sbc_has_city_blocks(map.version) || offset + 4 > bytes.size()) {
        return std::move(writer).bytes();
    }

    const auto city_block_count = reader.u32(offset);
    offset += 4;
    const auto count = std::min<std::size_t>(city_block_count, map.city_blocks.size());
    for (std::size_t index = 0; index < count && offset + 0x40 <= bytes.size(); ++index) {
        const auto& city_block = map.city_blocks[index];
        if (reader.ascii(offset, 0x40) != city_block.name) writer.ascii(offset, 0x40, city_block.name);
        offset += 0x40;

        std::uint32_t layer_count = 1;
        if (map.version >= 0x0c) {
            if (offset + 4 > bytes.size()) {
                break;
            }
            layer_count = reader.u32(offset);
            offset += 4;
        }

        const auto safe_layer_count = std::min<std::uint32_t>(layer_count, 64);
        for (std::uint32_t layer = 0; layer < safe_layer_count; ++layer) {
            for (std::size_t slot = 0; slot < 8; ++slot) {
                const auto slot_offset = offset + static_cast<std::size_t>(layer) * 8 * 0x40 + slot * 0x40;
                if (slot_offset + 0x40 > bytes.size()) {
                    return std::move(writer).bytes();
                }
                if (layer < city_block.layers.size()) {
                    if (reader.ascii(slot_offset, 0x40) != city_block.layers[layer][slot])
                        writer.ascii(slot_offset, 0x40, city_block.layers[layer][slot]);
                }
            }
        }
        offset += static_cast<std::size_t>(safe_layer_count) * 8 * 0x40;
    }

    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbc_portals(const std::vector<std::uint8_t>& bytes, const SbcMap& map) {
    BinaryReader reader{bytes};
    BinaryWriter writer{bytes};
    auto offset = std::size_t{4};

    if (map.version > 9) {
        const auto record_count = reader.u32(offset);
        offset += 4 + static_cast<std::size_t>(record_count) * 0x40;
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(intersection_count) * 12;

    const auto street_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(street_count) * 0x34;

    const auto building_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(building_count) * 0x50;

    if (sbc_has_city_blocks(map.version)) {
        if (offset + 4 > bytes.size()) {
            return std::move(writer).bytes();
        }

        const auto city_block_count = reader.u32(offset);
        offset += 4;
        for (std::uint32_t index = 0; index < city_block_count && offset + 0x40 <= bytes.size(); ++index) {
            offset += 0x40;
            std::uint32_t layer_count = 1;
            if (map.version >= 0x0c) {
                if (offset + 4 > bytes.size()) {
                    return std::move(writer).bytes();
                }
                layer_count = reader.u32(offset);
                offset += 4;
            }
            offset += static_cast<std::size_t>(std::min<std::uint32_t>(layer_count, 64)) * 8U * 0x40U;
        }
    }

    if (map.version <= 7 || offset + 4 > bytes.size()) {
        return std::move(writer).bytes();
    }

    const auto portal_section_offset = offset;
    const auto old_portal_count = reader.u32(offset);
    offset += 4;
    for (std::uint32_t index = 0; index < old_portal_count && offset + 16 <= bytes.size(); ++index) {
        offset += 16;
        if (map.version > 0x0a) {
            if (offset + 0x800 > bytes.size()) {
                return std::move(writer).bytes();
            }
            offset += 0x800;
        }
        if (map.version < 0x0b) {
            if (offset + 0x800 > bytes.size()) {
                return std::move(writer).bytes();
            }
            offset += 0x800;
            if (offset + 0x2000 > bytes.size()) {
                return std::move(writer).bytes();
            }
            offset += 0x2000;
        }
        if (map.version > 9) {
            if (offset + 0x800 > bytes.size()) {
                return std::move(writer).bytes();
            }
            offset += 0x800;
        }
    }
    const auto connection_tail_offset = offset;

    std::vector<std::uint8_t> rebuilt;
    rebuilt.reserve(bytes.size() + (map.portals.size() > old_portal_count ? (map.portals.size() - old_portal_count) * 0x1010U : 0U));
    rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(portal_section_offset));
    append_u32(rebuilt, static_cast<std::uint32_t>(map.portals.size()));
    for (const auto& portal : map.portals) {
        append_i32(rebuilt, portal.fields[0]);
        append_i32(rebuilt, portal.fields[1]);
        append_i32(rebuilt, portal.fields[2]);
        append_i32(rebuilt, portal.fields[3]);
        if (map.version > 0x0a) {
            for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                const auto value = block_index < portal.blocks.size() ? portal.blocks[block_index] : 0U;
                append_u32(rebuilt, value);
            }
        }
        if (map.version < 0x0b) {
            for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                const auto value = block_index < portal.legacy_blocks.size() ? portal.legacy_blocks[block_index] : 0U;
                append_u32(rebuilt, value);
            }
            for (std::size_t texture_index = 0; texture_index < 0x1000; ++texture_index) {
                const auto value = texture_index < portal.textures.size() ? portal.textures[texture_index] : 0U;
                append_u16(rebuilt, static_cast<std::uint16_t>(value));
            }
        }
        if (map.version > 9) {
            for (std::size_t itemset_index = 0; itemset_index < 0x200; ++itemset_index) {
                const auto value = itemset_index < portal.itemsets.size() ? portal.itemsets[itemset_index] : 0U;
                append_u32(rebuilt, value);
            }
        }
    }
    if (connection_tail_offset < bytes.size()) {
        rebuilt.insert(rebuilt.end(), bytes.begin() + static_cast<std::ptrdiff_t>(connection_tail_offset), bytes.end());
    }

    return rebuilt;
}

std::vector<std::uint8_t> rebuild_sbc_layout(const std::vector<std::uint8_t>& bytes, const SbcMap& map) {
    BinaryReader reader{bytes};
    auto offset = std::size_t{4};

    std::vector<std::uint8_t> rebuilt;
    append_u32(rebuilt, map.version);

    if (map.version > 9) {
        const auto record_count = reader.u32(offset);
        const auto header_tail_size = 4 + static_cast<std::size_t>(record_count) * 0x40;
        rebuilt.insert(
            rebuilt.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + header_tail_size));
        offset += header_tail_size;
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(intersection_count) * 12;

    const auto old_street_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(old_street_count) * 0x34;

    const auto old_building_count = reader.u32(offset);
    offset += 4 + static_cast<std::size_t>(old_building_count) * 0x50;
    const auto tail_offset = offset;

    rebuilt.reserve(bytes.size() + (map.streets.size() * 0x34) + (map.buildings.size() * 0x50));
    append_u32(rebuilt, static_cast<std::uint32_t>(map.intersections.size()));
    for (const auto& intersection : map.intersections) {
        append_i32(rebuilt, intersection.x);
        append_i32(rebuilt, intersection.y);
        append_i32(rebuilt, intersection.z);
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.streets.size()));
    for (std::size_t index = 0; index < map.streets.size(); ++index) {
        const auto& street = map.streets[index];
        append_i32(rebuilt, street.from_intersection);
        append_i32(rebuilt, street.to_intersection);
        append_i32(rebuilt, street.district);
        append_i32(rebuilt, street.left_width);
        append_i32(rebuilt, street.right_width);
        append_ascii(rebuilt, 0x20, street.name);
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.buildings.size()));
    for (const auto& building : map.buildings) {
        append_ascii(rebuilt, 0x40, building.name);
        append_i32(rebuilt, building.x);
        append_i32(rebuilt, building.y);
        append_i32(rebuilt, building.z);
        append_i32(rebuilt, building.rotation);
    }

    if (tail_offset < bytes.size()) {
        rebuilt.insert(rebuilt.end(), bytes.begin() + static_cast<std::ptrdiff_t>(tail_offset), bytes.end());
    }
    return rebuilt;
}

std::vector<std::uint8_t> rebuild_sbc(const SbcMap& map) {
    for (const auto& portal : map.portals) {
        const auto has_legacy_cells = std::ranges::any_of(portal.legacy_blocks, [](auto word) { return word != 0; });
        if (has_legacy_cells) {
            throw std::runtime_error("Promote legacy district geometry before writing SBC v12");
        }
    }
    std::vector<std::uint8_t> rebuilt;
    append_u32(rebuilt, 0x0c);

    append_u32(rebuilt, static_cast<std::uint32_t>(map.header_records.size()));
    for (const auto& record : map.header_records) {
        rebuilt.insert(rebuilt.end(), record.begin(), record.end());
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.intersections.size()));
    for (const auto& intersection : map.intersections) {
        append_i32(rebuilt, intersection.x);
        append_i32(rebuilt, intersection.y);
        append_i32(rebuilt, intersection.z);
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.streets.size()));
    for (const auto& street : map.streets) {
        append_i32(rebuilt, street.from_intersection);
        append_i32(rebuilt, street.to_intersection);
        append_i32(rebuilt, street.district);
        append_i32(rebuilt, street.left_width);
        append_i32(rebuilt, street.right_width);
        append_ascii(rebuilt, 0x20, street.name);
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.buildings.size()));
    for (const auto& building : map.buildings) {
        append_ascii(rebuilt, 0x40, building.name);
        append_i32(rebuilt, building.x);
        append_i32(rebuilt, building.y);
        append_i32(rebuilt, building.z);
        append_i32(rebuilt, building.rotation);
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.city_blocks.size()));
    for (const auto& city_block : map.city_blocks) {
        append_ascii(rebuilt, 0x40, city_block.name);
        append_u32(rebuilt, static_cast<std::uint32_t>(city_block.layers.size()));
        for (const auto& layer : city_block.layers) {
            for (const auto& slot : layer) {
                append_ascii(rebuilt, 0x40, slot);
            }
        }
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.portals.size()));
    for (const auto& portal : map.portals) {
        append_i32(rebuilt, portal.fields[0]);
        append_i32(rebuilt, portal.fields[1]);
        append_i32(rebuilt, portal.fields[2]);
        append_i32(rebuilt, portal.fields[3]);
        for (std::size_t index = 0; index < 0x200; ++index) {
            const auto value = index < portal.blocks.size() ? portal.blocks[index] : 0U;
            append_u32(rebuilt, value);
        }
        for (std::size_t index = 0; index < 0x200; ++index) {
            const auto value = index < portal.itemsets.size() ? portal.itemsets[index] : 0U;
            append_u32(rebuilt, value);
        }
    }

    append_u32(rebuilt, static_cast<std::uint32_t>(map.portal_connections.size()));
    for (const auto& connection : map.portal_connections) {
        append_i32(rebuilt, connection.fields[0]);
        append_i32(rebuilt, connection.fields[1]);
        append_i32(rebuilt, connection.fields[2]);
    }

    return rebuilt;
}

std::vector<std::uint8_t> patch_sbl_word(const std::vector<std::uint8_t>& bytes, std::size_t word_index, std::uint32_t value) {
    if (word_index >= bytes.size() / 4U) {
        throw std::runtime_error("SBL word index out of range");
    }
    BinaryWriter writer{bytes};
    const auto offset = word_index * 4U;
    writer.u32(offset, value);
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbl_block_header(const std::vector<std::uint8_t>& bytes, const SblBlockMesh& block) {
    BinaryWriter writer{bytes};
    if (bytes.size() < 44) {
        return std::move(writer).bytes();
    }

    writer.u32(4, block.width);
    writer.u32(8, block.height);
    writer.u32(12, block.depth);
    for (std::size_t index = 0; index < block.exterior_flags.size(); ++index) {
        writer.u32(16 + index * 4, block.exterior_flags[index]);
    }
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbl_block_patches(const std::vector<std::uint8_t>& bytes, const SblBlockMesh& block) {
    BinaryWriter writer{bytes};
    for (const auto& patch : block.authored_patches) {
        auto offset = patch.offset;
        if (offset + 344 > bytes.size()) {
            continue;
        }
        writer.u32(offset, patch.column_order);
        writer.u32(offset + 4, patch.column_tess);
        writer.u32(offset + 8, patch.row_order);
        writer.u32(offset + 12, patch.row_tess);
        writer.u32(offset + 16, patch.normal_flag_a);
        writer.u32(offset + 20, patch.normal_flag_b);
        offset += 24;
        for (std::size_t index = 0; index < std::min<std::size_t>(patch.control_points.size(), 16); ++index) {
            const auto& point = patch.control_points[index];
            writer.f32(offset, point.position.x);
            writer.f32(offset + 4, point.position.y);
            writer.f32(offset + 8, point.position.z);
            writer.f32(offset + 12, point.u);
            writer.f32(offset + 16, point.v);
            offset += 20;
        }
    }
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbl_layer_portal_type(
    const std::vector<std::uint8_t>& bytes,
    const SblTemplate& template_file,
    std::size_t layer_index,
    std::uint32_t portal_type) {
    BinaryWriter writer{bytes};
    if (layer_index >= template_file.layers.size()) {
        return std::move(writer).bytes();
    }
    const auto offset = template_file.layers[layer_index].offset;
    if (offset + 4 > bytes.size()) {
        return std::move(writer).bytes();
    }
    writer.u32(offset, portal_type);
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> rebuild_sbl_block(const SblBlockMesh& block) {
    // Turn the worked-out quads into flat patches. Only used to convert entries from the
    // game's built-in catalog, never to rewrite an imported curved mesh.
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 2);
    append_u32(bytes, std::max(1U, block.width));
    append_u32(bytes, std::max(1U, block.height));
    append_u32(bytes, std::max(1U, block.depth));
    for (int i = 0; i < 6; ++i) {
        append_u32(bytes, 0);
    }
    append_u32(bytes, static_cast<std::uint32_t>(block.faces.size()));
    for (const auto& face : block.faces) {
        if (face.vertices.size() != 4 || face.uvs.size() != 4) {
            throw std::runtime_error("Legacy conversion requires quad faces with UVs");
        }
        append_u32(bytes, 1); // Column order and tessellation.
        append_u32(bytes, 1);
        append_u32(bytes, 1); // Row order and tessellation.
        append_u32(bytes, 1);
        append_u32(bytes, face.material & 0xffffU);
        append_u32(bytes, 0);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                const auto u = column / 3.0F;
                const auto v = row / 3.0F;
                const auto interpolate = [&](auto value) {
                    const auto first = value(0) * (1 - u) + value(1) * u;
                    const auto second = value(3) * (1 - u) + value(2) * u;
                    return first * (1 - v) + second * v;
                };
                append_f32(bytes, interpolate([&](int i) { return face.vertices[i].x; }));
                append_f32(bytes, interpolate([&](int i) { return face.vertices[i].y; }));
                append_f32(bytes, interpolate([&](int i) { return face.vertices[i].z; }));
                append_f32(bytes, interpolate([&](int i) { return face.uvs[i].u; }));
                append_f32(bytes, interpolate([&](int i) { return face.uvs[i].v; }));
            }
        }
    }
    append_u32(bytes, 0); // No supplemental v2 records.
    return bytes;
}

void refresh_sbb_cell_payloads(SbbCell& cell) {
    cell.payloads = read_cell_payloads(cell.words);
    cell.occupied = std::ranges::any_of(cell.words, [](std::uint32_t word) { return word != 0; });
}

std::vector<SblBlockFace> sbl_patch_faces(const SblAuthoredPatch& patch) {
    std::vector<SblBlockFace> faces;
    append_authored_patch_faces(faces, {patch});
    return faces;
}

std::vector<SblBlockFace> sbl_box_faces(const SblBlockLight& box) {
    std::vector<SblBlockFace> faces;
    emit_sbl_box_faces(faces, box.points, box.scalars, box.final_value);
    return faces;
}

std::vector<std::uint8_t> write_sbl_block(const SblBlockMesh& block) {
    // BlockSbl_LoadFromFile @e6e50: version, size, six side flags, patches, and then
    // (in version 2) the box records.
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 2);
    append_u32(bytes, block.width);
    append_u32(bytes, block.height);
    append_u32(bytes, block.depth);
    for (const auto flag : block.exterior_flags) append_u32(bytes, flag);
    append_u32(bytes, static_cast<std::uint32_t>(block.authored_patches.size()));
    for (const auto& patch : block.authored_patches) {
        if (patch.control_points.size() != 16) throw std::runtime_error("A block patch needs 16 control points");
        append_u32(bytes, patch.column_order);
        append_u32(bytes, patch.column_tess);
        append_u32(bytes, patch.row_order);
        append_u32(bytes, patch.row_tess);
        append_u32(bytes, patch.normal_flag_a);
        append_u32(bytes, patch.normal_flag_b);
        for (const auto& point : patch.control_points) {
            append_f32(bytes, point.position.x);
            append_f32(bytes, point.position.y);
            append_f32(bytes, point.position.z);
            append_f32(bytes, point.u);
            append_f32(bytes, point.v);
        }
    }
    append_u32(bytes, static_cast<std::uint32_t>(block.lights.size()));
    for (const auto& box : block.lights) {
        for (const auto& corner : box.points) {
            append_f32(bytes, corner.x);
            append_f32(bytes, corner.y);
            append_f32(bytes, corner.z);
        }
        for (const auto scalar : box.scalars) append_u32(bytes, scalar);
        append_u32(bytes, box.final_value);
    }
    return bytes;
}

std::vector<std::uint8_t> rebuild_sbl_buildblock(const SblTemplate& template_file) {
    if (template_file.block || template_file.layers.size() > 16) {
        throw std::runtime_error("Expected a buildblock with at most 16 faces");
    }
    std::vector<std::uint8_t> bytes;
    // Unknown buildblock headers are preserved; changing one is not a format upgrade.
    append_u32(bytes, template_file.version);
    for (const auto& layer : template_file.layers) {
        if (layer.doors.size() > 2 || layer.windows.size() > 16 || layer.beams.size() > 16) {
            throw std::runtime_error("Buildblock face exceeds its opening limits");
        }
        append_u32(bytes, layer.portal_type);
        for (const auto value : layer.bounds) {
            append_f32(bytes, value);
        }
        append_u32(bytes, static_cast<std::uint32_t>(layer.doors.size()));
        for (const auto& door : layer.doors) {
            append_u32(bytes, door.type);
            append_f32(bytes, door.x);
            append_f32(bytes, door.y);
        }
        const auto append_rectangles = [&](const auto& rects) {
            append_u32(bytes, static_cast<std::uint32_t>(rects.size()));
            for (const auto& rect : rects) {
                append_u32(bytes, rect.type);
                append_f32(bytes, rect.x);
                append_f32(bytes, rect.width);
                append_f32(bytes, rect.y);
                append_f32(bytes, rect.height);
            }
        };
        append_rectangles(layer.windows);
        append_rectangles(layer.beams);
    }
    return bytes;
}

std::vector<std::uint8_t> patch_sbl_layers(const std::vector<std::uint8_t>& bytes, const SblTemplate& template_file) {
    BinaryWriter writer{bytes};
    for (const auto& layer : template_file.layers) {
        auto offset = layer.offset;
        if (offset + 0x18 > bytes.size()) {
            break;
        }

        writer.u32(offset, layer.portal_type);
        writer.f32(offset + 4, layer.bounds[0]);
        writer.f32(offset + 8, layer.bounds[1]);
        writer.f32(offset + 12, layer.bounds[2]);
        writer.f32(offset + 16, layer.bounds[3]);
        const auto door_count = std::min<std::size_t>(layer.doors.size(), max_sbl_buildblock_doors);
        writer.u32(offset + 20, static_cast<std::uint32_t>(door_count));
        offset += 0x18;

        for (std::size_t index = 0; index < door_count; ++index) {
            const auto& door = layer.doors[index];
            if (offset + 12 > bytes.size()) {
                return std::move(writer).bytes();
            }
            writer.u32(offset, door.type);
            writer.f32(offset + 4, door.x);
            writer.f32(offset + 8, door.y);
            offset += 12;
        }

        if (offset + 4 > bytes.size()) {
            return std::move(writer).bytes();
        }
        const auto window_count = std::min<std::size_t>(layer.windows.size(), max_sbl_buildblock_rects);
        writer.u32(offset, static_cast<std::uint32_t>(window_count));
        offset += 4;
        for (std::size_t index = 0; index < window_count; ++index) {
            const auto& window = layer.windows[index];
            if (offset + 20 > bytes.size()) {
                return std::move(writer).bytes();
            }
            writer.u32(offset, window.type);
            writer.f32(offset + 4, window.x);
            writer.f32(offset + 8, window.width);
            writer.f32(offset + 12, window.y);
            writer.f32(offset + 16, window.height);
            offset += 20;
        }

        if (offset + 4 > bytes.size()) {
            return std::move(writer).bytes();
        }
        const auto beam_count = std::min<std::size_t>(layer.beams.size(), max_sbl_buildblock_rects);
        writer.u32(offset, static_cast<std::uint32_t>(beam_count));
        offset += 4;
        for (std::size_t index = 0; index < beam_count; ++index) {
            const auto& beam = layer.beams[index];
            if (offset + 20 > bytes.size()) {
                return std::move(writer).bytes();
            }
            writer.u32(offset, beam.type);
            writer.f32(offset + 4, beam.x);
            writer.f32(offset + 8, beam.width);
            writer.f32(offset + 12, beam.y);
            writer.f32(offset + 16, beam.height);
            offset += 20;
        }
    }
    return std::move(writer).bytes();
}

// -----------------------------------------------------------------------------
// CSX archive writer
// -----------------------------------------------------------------------------

std::vector<std::uint8_t> build_csx(const CsxArchive& archive) {
    auto payload_size = std::size_t{0};
    for (const auto& entry : archive.entries) {
        if (entry.payload.size() > max_csx_payload_bytes - payload_size) {
            throw std::runtime_error("CSX output exceeds the 512 MiB payload allocation limit");
        }
        payload_size += entry.payload.size();
    }
    if (archive.entries.size() > (std::numeric_limits<std::uint32_t>::max() - 12U - payload_size) / 0x40U) {
        throw std::runtime_error("CSX output exceeds its 32-bit directory offsets");
    }
    const auto directory_offset = std::size_t{12} + payload_size;
    BinaryWriter writer{directory_offset + archive.entries.size() * 0x40};
    writer.u32(0, archive.magic);
    writer.u32(4, static_cast<std::uint32_t>(directory_offset));
    writer.u32(8, static_cast<std::uint32_t>(archive.entries.size()));

    auto payload_offset = std::size_t{12};
    for (std::size_t index = 0; index < archive.entries.size(); ++index) {
        const auto& entry = archive.entries[index];
        std::ranges::copy(entry.payload, writer.raw().begin() + static_cast<std::ptrdiff_t>(payload_offset));

        const auto entry_offset = directory_offset + index * 0x40;
        writer.u32(entry_offset, entry.type);
        writer.u32(entry_offset + 4, static_cast<std::uint32_t>(payload_offset));
        writer.u32(entry_offset + 8, static_cast<std::uint32_t>(entry.payload.size()));
        writer.ascii(entry_offset + 12, 0x34, entry.name);
        payload_offset += entry.payload.size();
    }

    return std::move(writer).bytes();
}

} // namespace pafta
