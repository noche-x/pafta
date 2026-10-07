#include "formats.hpp"

#include <bit>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace map_editor {
namespace {

// -----------------------------------------------------------------------------
// Binary Reader/Writer Primitives
// -----------------------------------------------------------------------------

constexpr std::size_t sbb_exterior_block_word = SbbCell::exterior_block_word;
constexpr std::size_t sbb_interior_block_word = SbbCell::interior_block_word;
constexpr std::uint32_t sbb_payload_class_mask = 0xe0000000U;
// BuildBuildingBlockRenderBuffers_1400799d0.c:128: solid/buildblock cell tag.
constexpr std::uint32_t sbb_solid_payload_class = 0x40000000U;
// BuildBuildingBlockRenderBuffers_1400799d0.c:212: standalone mesh-instance tag;
// this port exposes those parallel cell payloads as PayloadKind::Piece.
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
        const auto word = u32(offset);
        float value{};
        std::memcpy(&value, &word, sizeof(value));
        return value;
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

private:
    void require(std::size_t offset, std::size_t length) const {
        if (offset > bytes_.size() || length > bytes_.size() - offset) {
            throw std::runtime_error("Unexpected end of file while reading SBB");
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
        std::uint32_t word{};
        std::memcpy(&word, &value, sizeof(value));
        u32(offset, word);
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
// SBB/SBC/SBL Parse Helpers
// -----------------------------------------------------------------------------

std::vector<std::string> read_fixed_ascii_chunks(
    const BinaryReader& reader,
    std::size_t offset,
    std::size_t chunk_size,
    std::uint32_t count
) {
    std::vector<std::string> strings;
    strings.reserve(count);

    for (std::uint32_t index = 0; index < count && offset + chunk_size <= reader.size(); ++index) {
        auto value = reader.ascii(offset, chunk_size);
        if (value.empty()) {
            break;
        }
        strings.push_back(std::move(value));
        offset += chunk_size;
    }

    return strings;
}

std::vector<std::string> read_counted_names(const BinaryReader& reader, std::size_t offset) {
    const auto count = std::min<std::uint32_t>(reader.u32(offset), 1024U);
    return read_fixed_ascii_chunks(reader, offset + 4, 0x40, count);
}

SbbCellPayloads read_cell_payloads(const std::array<std::uint32_t, 6>& words) {
    SbbCellPayloads payloads;
    const auto exterior = words[sbb_exterior_block_word];
    const auto interior = words[sbb_interior_block_word];

    // SaveBuilding_SBB_1400548d0.decomp.txt:76-99 serializes exterior/interior
    // block payloads first, even though BuildingCell memory keeps cellType at +0x00.
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
    const auto width = std::min<std::uint32_t>(0x40, size_x + 1);
    const auto depth = std::min<std::uint32_t>(0x40, size_y + 1);
    const auto height = std::min<std::uint32_t>(0x40, size_z);
    cells.reserve(static_cast<std::size_t>(width) * depth * height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t z = 0; z < depth; ++z) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (offset + 0x3c > reader.size()) {
                    return cells;
                }

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

std::uint32_t safe_record_count(std::uint32_t count, std::uint32_t limit) {
    return std::min(count, limit);
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
    const auto width = std::min<std::uint32_t>(0x40, building.size_x + 1);
    const auto depth = std::min<std::uint32_t>(0x40, building.size_y + 1);
    const auto height = std::min<std::uint32_t>(0x40, building.size_z);
    return static_cast<std::size_t>(width) * depth * height;
}

constexpr std::uint32_t max_sbl_buildblock_doors = 2;
constexpr std::uint32_t max_sbl_buildblock_rects = 16;

// -----------------------------------------------------------------------------
// SBL Block Mesh Reconstruction
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

Vec2f native_oriented_uv(const Vec3f& position, const Vec2f& fallback, std::uint32_t orientation);

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

std::uint32_t clamp_bezier_order(std::uint32_t order) {
    return std::clamp(order, 1U, 3U);
}

std::uint32_t clamp_bezier_tess(std::uint32_t tess) {
    return std::clamp(tess, 1U, 128U);
}

float bezier1d(std::uint32_t order, float t, const std::array<float, 4>& values) {
    const auto clamped_order = clamp_bezier_order(order);
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

Vec2f native_oriented_uv(const Vec3f& position, const Vec2f& fallback, std::uint32_t orientation) {
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
            faces.push_back({
                material & 0xffffU,
                {
                    bezier_patch_sample(control_points, column_order, row_order, a0, b0),
                    bezier_patch_sample(control_points, column_order, row_order, a1, b0),
                    bezier_patch_sample(control_points, column_order, row_order, a1, b1),
                    bezier_patch_sample(control_points, column_order, row_order, a0, b1),
                },
                {
                    native_oriented_uv(
                        bezier_patch_sample(control_points, column_order, row_order, a0, b0),
                        bezier_patch_uv_sample(control_uvs, column_order, row_order, a0, b0),
                        material),
                    native_oriented_uv(
                        bezier_patch_sample(control_points, column_order, row_order, a1, b0),
                        bezier_patch_uv_sample(control_uvs, column_order, row_order, a1, b0),
                        material),
                    native_oriented_uv(
                        bezier_patch_sample(control_points, column_order, row_order, a1, b1),
                        bezier_patch_uv_sample(control_uvs, column_order, row_order, a1, b1),
                        material),
                    native_oriented_uv(
                        bezier_patch_sample(control_points, column_order, row_order, a0, b1),
                        bezier_patch_uv_sample(control_uvs, column_order, row_order, a0, b1),
                        material),
                },
                true,
                material & 0xf0000U,
            });
        }
    }
}

void emit_sbl_light_faces(
    std::vector<SblBlockFace>& faces,
    const std::array<Vec3f, 8>& points,
    const std::array<std::uint32_t, 6>& scalars,
    std::uint32_t final_value) {
    constexpr std::array<std::array<std::size_t, 4>, 6> light_face_corners{{
        {{3, 2, 1, 0}},
        {{7, 6, 2, 3}},
        {{4, 7, 3, 0}},
        {{5, 4, 0, 1}},
        {{6, 5, 1, 2}},
        {{4, 5, 6, 7}},
    }};

    for (std::size_t face = 0; face < light_face_corners.size(); ++face) {
        if ((final_value & (1U << face)) == 0U) {
            continue;
        }
        const auto& corners = light_face_corners[face];
        // LoadAndInitBlockTemplates_SBL calculates the orientation from the
        // emitted light-face triangle normal. It does not use the face index:
        // light points can describe a rotated or non-cubic surface. A fixed
        // cube orientation flips UVs on authored light faces.
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
    const auto authored_face_count = safe_record_count(reader.u32(40), 0x400);
    std::vector<SblAuthoredPatch> authored_patches;
    authored_patches.reserve(authored_face_count);
    auto offset = std::size_t{44};
    for (std::uint32_t face = 0; face < authored_face_count && offset + 344 <= reader.size(); ++face) {
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
    if (reader.u32(0) > 1 && offset + 4 <= reader.size()) {
        light_count = safe_record_count(reader.u32(offset), 0x100);
        offset += 4;
        lights.reserve(light_count);
        for (std::uint32_t light = 0; light < light_count && offset + 0x7c <= reader.size(); ++light) {
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
            emit_sbl_light_faces(faces, points, scalars, final_value);
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
    doors.reserve(count);
    for (std::uint32_t index = 0; index < count && offset + 12 <= reader.size(); ++index) {
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
    rects.reserve(count);
    for (std::uint32_t index = 0; index < count && offset + 20 <= reader.size(); ++index) {
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

    for (std::size_t index = 0; index < 16 && offset + 0x18 <= reader.size(); ++index) {
        const auto layer_offset = offset;
        const auto portal_type = reader.u32(offset);
        const std::array<float, 4> bounds{
            reader.f32(offset + 4),
            reader.f32(offset + 8),
            reader.f32(offset + 12),
            reader.f32(offset + 16),
        };
        const auto door_count = safe_record_count(reader.u32(offset + 20), max_sbl_buildblock_doors);
        offset += 0x18;

        auto doors = read_sbl_doors(reader, offset, door_count);
        offset += doors.size() * 12;

        const auto window_count = offset + 4 <= reader.size() ? safe_record_count(reader.u32(offset), max_sbl_buildblock_rects) : 0U;
        offset += 4;
        auto windows = read_sbl_rects(reader, offset, window_count);
        offset += windows.size() * 20;

        const auto beam_count = offset + 4 <= reader.size() ? safe_record_count(reader.u32(offset), max_sbl_buildblock_rects) : 0U;
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
            // Block/buildblock .sbl (parse_sbl). CSX packs these with container
            // version sentinel 1; load_project_csx_geometry normalizes 1 -> 2.
            return CsxEntryKind::Sbl;
        case 0x585a0002:
            // Building .sbb. CSX packs it with container version sentinel 1;
            // load_project_csx_geometry normalizes 1 -> 14 before parse_sbb.
            // (parse_sbl is lenient enough to misread these bytes as layers, but
            // parse_sbb confirms they are real buildings, e.g. station2 12x12x4.)
            return CsxEntryKind::Sbb;
        case 0x585a0004:
            return CsxEntryKind::Texture;
        default:
            return CsxEntryKind::Unknown;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Public Parse Entry Points
// -----------------------------------------------------------------------------

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Could not open " + path.string());
    }

    // Bulk read: size the buffer from the end-seek and pull the whole file in one
    // read() call. The previous istreambuf_iterator construction copied the file a
    // byte at a time through the stream's locale/sentry machinery, which dominated
    // load-time profiles (CSX/SBC/SBB/SBL all funnel through here).
    const auto end = file.tellg();
    if (end < 0) {
        throw std::runtime_error("Could not size " + path.string());
    }
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            bytes.resize(static_cast<std::size_t>(file.gcount()));
        }
    }
    return bytes;
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

    if (building.version >= 0x0e && offset + 8 <= reader.size()) {
        building.lod_tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
        building.lod_marker = reader.u32(offset);
        if (building.lod_marker == 1) {
            building.lod_group_count = reader.u32(offset + 4);
        }
    }

    return building;
}

SbbBuilding parse_sbb_file(const std::filesystem::path& path) {
    return parse_sbb(read_file_bytes(path));
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
            map.header_records.push_back(record); offset += 64;
        }
    }

    const auto intersection_count = reader.u32(offset);
    offset += 4;
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

    if (sbc_has_city_blocks(map.version) && offset + 4 <= reader.size()) {
        const auto city_block_count = reader.u32(offset);
        offset += 4;
        map.city_blocks.reserve(city_block_count);
        for (std::uint32_t index = 0; index < city_block_count && offset + 0x40 <= reader.size(); ++index) {
            const auto name = reader.ascii(offset, 0x40);
            offset += 0x40;

            std::uint32_t layer_count = 1;
            if (map.version >= 0x0c) {
                if (offset + 4 > reader.size()) {
                    break;
                }
                layer_count = reader.u32(offset);
                offset += 4;
            }

            const auto safe_layer_count = std::min<std::uint32_t>(layer_count, 64);
            const auto layer_bytes = static_cast<std::size_t>(safe_layer_count) * 8 * 0x40;
            if (offset + layer_bytes > reader.size()) {
                offset = reader.size();
                break;
            }
            std::vector<std::array<std::string, 8>> layers;
            layers.reserve(safe_layer_count);
            for (std::uint32_t layer = 0; layer < safe_layer_count; ++layer) {
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

    if (map.version > 7 && offset + 4 <= reader.size()) {
        const auto portal_count = reader.u32(offset);
        offset += 4;
        map.portals.reserve(portal_count);
        for (std::uint32_t index = 0; index < portal_count && offset + 16 <= reader.size(); ++index) {
            const auto fields = std::array<std::int32_t, 4>{
                reader.i32(offset),
                reader.i32(offset + 4),
                reader.i32(offset + 8),
                reader.i32(offset + 12),
            };
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
                if (offset + 0x800 > reader.size()) {
                    offset = reader.size();
                    break;
                }
                portal.blocks.reserve(0x200);
                for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                    portal.blocks.push_back(reader.u32(offset + block_index * 4U));
                }
                offset += 0x800;
            }

            if (map.version < 0x0b) {
                if (offset + 0x800 > reader.size()) {
                    offset = reader.size();
                    break;
                }
                portal.legacy_blocks.reserve(0x200);
                for (std::size_t block_index = 0; block_index < 0x200; ++block_index) {
                    portal.legacy_blocks.push_back(reader.u32(offset + block_index * 4U));
                }
                offset += 0x800;

                if (offset + 0x2000 > reader.size()) {
                    offset = reader.size();
                    break;
                }
                portal.textures.reserve(0x1000);
                for (std::size_t texture_index = 0; texture_index < 0x1000; ++texture_index) {
                    portal.textures.push_back(reader.u16(offset + texture_index * 2U));
                }
                offset += 0x2000;
            }

            if (map.version > 9) {
                if (offset + 0x800 > reader.size()) {
                    offset = reader.size();
                    break;
                }
                portal.itemsets.reserve(0x200);
                for (std::size_t itemset_index = 0; itemset_index < 0x200; ++itemset_index) {
                    portal.itemsets.push_back(reader.u32(offset + itemset_index * 4U));
                }
                offset += 0x800;
            }
        }
    }

    if (map.version > 8 && offset + 4 <= reader.size()) {
        const auto connection_count = reader.u32(offset);
        offset += 4;
        map.portal_connections.reserve(connection_count);
        for (std::uint32_t index = 0; index < connection_count && offset + 12 <= reader.size(); ++index) {
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
    std::vector<SblBlockFace> preserved_light_faces;
    const auto light_begin = static_cast<std::size_t>(exterior_count) + static_cast<std::size_t>(authored_count);
    // Box faces come from the box records; a block without records (the
    // native procedural catalog) keeps the box faces it was built with.
    if (block.lights.empty() && light_begin < block.faces.size()) {
        preserved_light_faces.assign(block.faces.begin() + static_cast<std::ptrdiff_t>(light_begin), block.faces.end());
    }

    auto faces = build_exterior_block_faces(block.width, block.height, block.depth, block.exterior_flags);
    append_authored_patch_faces(faces, block.authored_patches);
    for (const auto& box : block.lights) emit_sbl_light_faces(faces, box.points, box.scalars, box.final_value);
    faces.insert(faces.end(), preserved_light_faces.begin(), preserved_light_faces.end());
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
    archive.entries.reserve(archive.entry_count);

    for (std::uint32_t index = 0; index < archive.entry_count; ++index) {
        const auto directory_entry = static_cast<std::size_t>(archive.directory_offset) + static_cast<std::size_t>(index) * 0x40;
        if (directory_entry + 0x40 > reader.size()) {
            break;
        }

        const auto type = reader.u32(directory_entry);
        const auto payload_offset = reader.u32(directory_entry + 4);
        const auto payload_size = reader.u32(directory_entry + 8);
        std::vector<std::uint8_t> payload;
        if (static_cast<std::size_t>(payload_offset) <= bytes.size() &&
            static_cast<std::size_t>(payload_size) <= bytes.size() - static_cast<std::size_t>(payload_offset)) {
            payload.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset + payload_size)
            );
        }

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

CsxArchive parse_csx_file(const std::filesystem::path& path) {
    return parse_csx(read_file_bytes(path));
}

// -----------------------------------------------------------------------------
// Patch and Rebuild Writers
// -----------------------------------------------------------------------------

std::vector<std::uint8_t> patch_sbb_cell(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building, const SbbCell& cell) {
    BinaryWriter writer{bytes};
    const auto cell_volume_offset = sbb_table_offset(building.version)
        + building.materials.size() * 0x40
        + 4 + building.piece_names.size() * 0x40
        + 4 + building.block_names.size() * 0x40
        + 4 + building.item_set_names.size() * 0x40;
    const auto cell_offset = cell_volume_offset + cell.index * 0x3c;
    if (cell_offset + 0x3c > bytes.size()) {
        return std::move(writer).bytes();
    }

    for (std::size_t index = 0; index < cell.words.size(); ++index) {
        writer.u32(cell_offset + index * 4, cell.words[index]);
    }
    for (std::size_t index = 0; index < cell.shorts.size(); ++index) {
        writer.u16(cell_offset + 24 + index * 2, cell.shorts[index]);
    }
    writer.u32(cell_offset + 56, cell.item_set_index);
    return std::move(writer).bytes();
}

std::vector<std::uint8_t> patch_sbb_cells(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building) {
    auto patched = bytes;
    for (const auto& cell : building.cells) {
        patched = patch_sbb_cell(patched, building, cell);
    }
    return patched;
}

std::vector<std::uint8_t> rebuild_sbb(const SbbBuilding& building) {
    auto size_x = building.size_x;
    auto size_y = building.size_y;
    auto size_z = building.size_z;
    for (const auto& cell : building.cells) {
        if (cell.exterior_block() == 0 && cell.interior_block() == 0) {
            continue;
        }
        // X/Z sizes are the highest index (the grid below spans 0..size
        // inclusive); Y is the floor count.
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

    // Index the cells once; a per-slot search made large buildings take seconds.
    // The first cell at a position wins, as the search did.
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

    writer.ascii(offset, 0x40, building.name);
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
        writer.ascii(offset, 0x40, city_block.name);
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
                rebuilt.push_back(static_cast<std::uint8_t>(value & 0xffU));
                rebuilt.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
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
    for (const auto& portal : map.portals)
        if (std::any_of(portal.legacy_blocks.begin(), portal.legacy_blocks.end(), [](auto word) { return word != 0; }))
            throw std::runtime_error("Promote legacy district geometry before writing SBC v12");
    std::vector<std::uint8_t> rebuilt;
    append_u32(rebuilt, 0x0c);

    append_u32(rebuilt, static_cast<std::uint32_t>(map.header_records.size()));
    for (const auto& record : map.header_records) rebuilt.insert(rebuilt.end(), record.begin(), record.end());

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
    BinaryWriter writer{bytes};
    const auto offset = word_index * 4;
    if (offset + 4 > bytes.size()) {
        return std::move(writer).bytes();
    }
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
    // Materialize evaluated quads as linear patches. Used for converting native
    // procedural catalog entries, never to rewrite an imported curved mesh.
    std::vector<std::uint8_t> bytes;
    const auto number = [&](float value) {
        std::uint32_t bits; std::memcpy(&bits, &value, 4); append_u32(bytes, bits);
    };
    append_u32(bytes, 2);
    append_u32(bytes, std::max(1U, block.width));
    append_u32(bytes, std::max(1U, block.height));
    append_u32(bytes, std::max(1U, block.depth));
    for (int i = 0; i < 6; ++i) append_u32(bytes, 0);
    append_u32(bytes, static_cast<std::uint32_t>(block.faces.size()));
    for (const auto& face : block.faces) {
        if (face.vertices.size() != 4 || face.uvs.size() != 4)
            throw std::runtime_error("Legacy conversion requires quad faces with UVs");
        append_u32(bytes, 1); append_u32(bytes, 1);
        append_u32(bytes, 1); append_u32(bytes, 1);
        append_u32(bytes, face.material & 0xffffU); append_u32(bytes, 0);
        for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) {
            const float u = column / 3.0F, v = row / 3.0F;
            const auto interpolate = [&](auto value) {
                return (value(0) * (1-u) + value(1) * u) * (1-v) +
                    (value(3) * (1-u) + value(2) * u) * v;
            };
            number(interpolate([&](int i) { return face.vertices[i].x; }));
            number(interpolate([&](int i) { return face.vertices[i].y; }));
            number(interpolate([&](int i) { return face.vertices[i].z; }));
            number(interpolate([&](int i) { return face.uvs[i].u; }));
            number(interpolate([&](int i) { return face.uvs[i].v; }));
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
    emit_sbl_light_faces(faces, box.points, box.scalars, box.final_value);
    return faces;
}

std::vector<std::uint8_t> write_sbl_block(const SblBlockMesh& block) {
    // BlockSbl_LoadFromFile @e6e50: version, size, six side flags, patches,
    // then (version 2) the box records.
    std::vector<std::uint8_t> bytes;
    const auto number = [&](float value) { append_u32(bytes, std::bit_cast<std::uint32_t>(value)); };
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
            number(point.position.x);
            number(point.position.y);
            number(point.position.z);
            number(point.u);
            number(point.v);
        }
    }
    append_u32(bytes, static_cast<std::uint32_t>(block.lights.size()));
    for (const auto& box : block.lights) {
        for (const auto& corner : box.points) {
            number(corner.x);
            number(corner.y);
            number(corner.z);
        }
        for (const auto scalar : box.scalars) append_u32(bytes, scalar);
        append_u32(bytes, box.final_value);
    }
    return bytes;
}

std::vector<std::uint8_t> rebuild_sbl_buildblock(const SblTemplate& template_file) {
    if (template_file.block || template_file.layers.size() > 16)
        throw std::runtime_error("Expected a buildblock with at most 16 faces");
    std::vector<std::uint8_t> bytes;
    // All shipped buildblock templates use header 1. Block-mesh SBL v2 is a
    // different layout; setting this to 2 is not a buildblock upgrade.
    append_u32(bytes, 1);
    const auto number = [&](float value) {
        const auto offset = bytes.size();
        bytes.resize(offset + 4);
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(bits >> (i * 8));
    };
    for (const auto& layer : template_file.layers) {
        if (layer.doors.size() > 2 || layer.windows.size() > 16 || layer.beams.size() > 16)
            throw std::runtime_error("Buildblock face exceeds its opening limits");
        append_u32(bytes, layer.portal_type);
        for (const auto value : layer.bounds) number(value);
        append_u32(bytes, static_cast<std::uint32_t>(layer.doors.size()));
        for (const auto& door : layer.doors) {
            append_u32(bytes, door.type); number(door.x); number(door.y);
        }
        const auto rectangles = [&](const auto& rects) {
            append_u32(bytes, static_cast<std::uint32_t>(rects.size()));
            for (const auto& rect : rects) {
                append_u32(bytes, rect.type); number(rect.x); number(rect.width); number(rect.y); number(rect.height);
            }
        };
        rectangles(layer.windows); rectangles(layer.beams);
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
// CSX Archive Writer
// -----------------------------------------------------------------------------

std::vector<std::uint8_t> build_csx(const CsxArchive& archive) {
    const auto payload_size = std::ranges::fold_left(archive.entries, std::size_t{0}, [](std::size_t total, const CsxEntry& entry) {
        return total + entry.payload.size();
    });
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

} // namespace map_editor
