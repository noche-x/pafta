// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pafta {

enum class PayloadKind {
    // 0x40000000: a solid or buildblock cell.
    Block,
    // 0x80000000: a single SblBlockMesh; the low 10 bits pick the piece name and mesh.
    Piece,
    Other,
};

struct SbbCellPayload {
    PayloadKind kind{};
    std::uint32_t word{};
    // Solid cells keep the low 16 bits. Pieces use the low 10 bits to pick a mesh
    // name.
    std::uint32_t index{};

    bool operator==(const SbbCellPayload&) const = default;
};

// A cell's block references, at most three (read_cell_payloads: the solid block
// plus one for each block word that isn't solid), stored inline instead of on the
// heap.
class SbbCellPayloads {
public:
    SbbCellPayloads() = default;
    SbbCellPayloads(std::initializer_list<SbbCellPayload> payloads) {
        for (const auto& payload : payloads) push_back(payload);
    }
    void push_back(const SbbCellPayload& payload) {
        if (count_ < items_.size()) items_[count_++] = payload;
    }
    void clear() { count_ = 0; }
    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    const SbbCellPayload& operator[](std::size_t i) const { return items_[i]; }
    SbbCellPayload& operator[](std::size_t i) { return items_[i]; }
    const SbbCellPayload* begin() const { return items_.data(); }
    const SbbCellPayload* end() const { return items_.data() + count_; }
    SbbCellPayload* begin() { return items_.data(); }
    SbbCellPayload* end() { return items_.data() + count_; }
    bool operator==(const SbbCellPayloads& other) const {
        return count_ == other.count_ && std::equal(begin(), end(), other.begin());
    }

private:
    std::array<SbbCellPayload, 3> items_{};
    std::uint8_t count_{};
};

struct SbbCell {
    static constexpr std::size_t exterior_block_word = 0;
    static constexpr std::size_t interior_block_word = 1;
    static constexpr std::size_t cell_type_word = 2;
    static constexpr std::size_t exterior_portal_face_word = 3;
    static constexpr std::size_t interior_portal_face_word = 4;
    static constexpr std::size_t placed_flag_word = 5;

    std::size_t index{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    // The order the game's SaveBuilding_SBB writes fields is not the order of
    // BuildingCell in memory. SaveBuilding_SBB_1400548d0.decomp.txt:76-99 writes:
    // exteriorBlock, interiorBlock, cellType, exteriorPortalFace, interiorPortalFace,
    // placedFlag. The BuildingCell memory offsets are checked in
    // BuildingTool_ReadCellValueOrZero_140067650.c:11-20: cellType +0x00,
    // exteriorBlock +0x04, interiorBlock +0x08, exteriorPortalFace +0x0c,
    // interiorPortalFace +0x10, placedFlag +0x14, texture sets +0x18/+0x28,
    // itemsetPayload +0x38.
    std::array<std::uint32_t, 6> words{};
    // A packed item-set word, not a plain index. 0 means none. Placed sets use
    // 0x40000000 | (rotation << 24) | name-list index. The renderer reads two rotation
    // bits and the low 10 index bits. Keep any other value as it is.
    // It is at +0x38 both in the game's memory and in the SBB file.
    std::uint32_t item_set_index{};
    bool occupied{};
    SbbCellPayloads payloads;
    std::array<std::uint16_t, 16> shorts{};

    [[nodiscard]] std::uint32_t exterior_block() const { return words[exterior_block_word]; }
    [[nodiscard]] std::uint32_t interior_block() const { return words[interior_block_word]; }
    [[nodiscard]] std::uint32_t cell_type() const { return words[cell_type_word]; }
    [[nodiscard]] std::uint32_t exterior_portal_face() const { return words[exterior_portal_face_word]; }
    [[nodiscard]] std::uint32_t interior_portal_face() const { return words[interior_portal_face_word]; }
    [[nodiscard]] std::uint32_t placed_flag() const { return words[placed_flag_word]; }
    std::uint32_t& exterior_block() { return words[exterior_block_word]; }
    std::uint32_t& interior_block() { return words[interior_block_word]; }
    std::uint32_t& cell_type() { return words[cell_type_word]; }
    std::uint32_t& exterior_portal_face() { return words[exterior_portal_face_word]; }
    std::uint32_t& interior_portal_face() { return words[interior_portal_face_word]; }
    std::uint32_t& placed_flag() { return words[placed_flag_word]; }

    bool operator==(const SbbCell&) const = default;
};

constexpr std::optional<std::uint32_t> sbb_item_set_payload(std::size_t name_index, std::uint32_t rotation) {
    if (name_index >= 1024U || rotation >= 4U) return std::nullopt;
    return 0x40000000U | (rotation << 24U) | static_cast<std::uint32_t>(name_index);
}

constexpr bool is_sbb_item_set_payload(std::uint32_t value) {
    return (value & ~0x030003ffU) == 0x40000000U;
}

constexpr std::uint32_t sbb_item_set_name_index(std::uint32_t value) { return value & 0x3ffU; }
constexpr std::uint32_t sbb_item_set_rotation(std::uint32_t value) { return (value >> 24U) & 3U; }

struct SbbBuilding {
    std::uint32_t version{};
    std::string name;
    std::uint32_t size_x{};
    std::uint32_t size_y{};
    std::uint32_t size_z{};
    std::uint32_t anchor_x{};
    std::uint32_t anchor_y{};
    std::uint32_t anchor_z{};
    std::uint32_t extent_x{};
    std::uint32_t extent_y{};
    std::uint32_t extent_z{};
    std::vector<std::string> materials;
    std::vector<std::string> piece_names; // piece meshes, picked by the low bits of a piece word.
    std::vector<std::string> block_names; // buildblock templates, picked by SbbCell::cell_type().
    std::vector<std::string> item_set_names;
    std::vector<SbbCell> cells;
    std::uint32_t lod_marker{};
    std::uint32_t lod_group_count{};
    std::vector<std::uint8_t> lod_tail;

    bool operator==(const SbbBuilding&) const = default;
};

inline constexpr std::uint32_t supported_sbb_grid_side = 64;

// Header X and Y are the highest grid indices; Z is the number of floors.
constexpr bool sbb_dimensions_supported(std::uint32_t size_x, std::uint32_t size_y, std::uint32_t size_z) {
    return size_x < supported_sbb_grid_side && size_y < supported_sbb_grid_side && size_z <= supported_sbb_grid_side;
}

struct SbcIntersection {
    std::size_t index{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    bool operator==(const SbcIntersection&) const = default;
};

struct SbcStreet {
    std::size_t index{};
    std::int32_t from_intersection{};
    std::int32_t to_intersection{};
    std::int32_t district{};
    std::int32_t left_width{};
    std::int32_t right_width{};
    std::string name;

    bool operator==(const SbcStreet&) const = default;
};

struct SbcBuilding {
    std::size_t index{};
    std::string name;
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    std::int32_t rotation{};

    bool operator==(const SbcBuilding&) const = default;
};

struct SbcCityBlock {
    std::size_t index{};
    std::string name;
    std::vector<std::array<std::string, 8>> layers;

    bool operator==(const SbcCityBlock&) const = default;
};

inline constexpr std::string_view native_default_city_block_display_name = "nblock";

inline std::string_view city_block_display_name(const SbcCityBlock& city_block) {
    return city_block.name.empty() ? native_default_city_block_display_name : std::string_view{city_block.name};
}

// Sector-to-cell conversion includes room for the eight cells and neighbour tests.
std::int32_t sbc_portal_cell_base(std::int32_t sector);

struct SbcPortal {
    std::size_t index{};
    std::array<std::int32_t, 4> fields{};
    std::vector<std::uint32_t> blocks;
    std::vector<std::uint32_t> legacy_blocks;
    std::vector<std::uint16_t> textures;
    std::vector<std::uint32_t> itemsets;

    // The active district grid of 0x200 voxels. v11 and later SBC files store it in
    // `blocks`. v10 (for example the round map) stores the same grid, with the same word
    // format (0x80000000 = piece, 0x40000000 = solid), in `legacy_blocks`. Building the
    // scene and terrain has to read whichever one is filled in, or v10 maps lose their
    // district pieces and the terrain stamps those pieces make.
    const std::vector<std::uint32_t>& voxel_blocks() const {
        return blocks.empty() ? legacy_blocks : blocks;
    }

    bool operator==(const SbcPortal&) const = default;
};

struct SbcPortalConnection {
    std::size_t index{};
    std::array<std::int32_t, 3> fields{};

    bool operator==(const SbcPortalConnection&) const = default;
};

struct SbcMap {
    std::uint32_t version{};
    std::vector<std::array<std::uint8_t, 64>> header_records;
    std::vector<SbcIntersection> intersections;
    std::vector<SbcStreet> streets;
    std::vector<SbcBuilding> buildings;
    std::vector<SbcCityBlock> city_blocks;
    std::vector<SbcPortal> portals;
    std::vector<SbcPortalConnection> portal_connections;

    bool operator==(const SbcMap&) const = default;
};

struct Vec3f {
    float x{};
    float y{};
    float z{};

    bool operator==(const Vec3f&) const = default;
};

struct Vec2f {
    float u{};
    float v{};

    bool operator==(const Vec2f&) const = default;
};

// The game's face orientation projects world coordinates onto the face's texture
// plane. An unknown orientation keeps the stored UV instead of changing it.
Vec2f native_oriented_uv(Vec3f position, Vec2f fallback, std::uint32_t orientation);

struct SblBlockFace {
    std::uint32_t material{};
    std::vector<Vec3f> vertices;
    std::vector<Vec2f> uvs;
    bool authored{};
    std::uint32_t orientation{};

    bool operator==(const SblBlockFace&) const = default;
};

struct SblBlockLight {
    // This old public name means a textured box, not a light.
    // Scalars[0] is its material; the other number words are kept as they are.
    std::array<Vec3f, 8> points{};
    std::array<std::uint32_t, 6> scalars{};
    // Bits 0..5 turn on the six faces, in the game's box-corner order.
    std::uint32_t final_value{};

    bool operator==(const SblBlockLight&) const = default;
};

struct SblControlPoint {
    Vec3f position{};
    float u{};
    float v{};

    bool operator==(const SblControlPoint&) const = default;
};

struct SblAuthoredPatch {
    std::size_t index{};
    std::size_t offset{};
    std::uint32_t column_order{};
    std::uint32_t column_tess{};
    std::uint32_t row_order{};
    std::uint32_t row_tess{};
    std::uint32_t normal_flag_a{};
    std::uint32_t normal_flag_b{};
    std::vector<SblControlPoint> control_points;

    bool operator==(const SblAuthoredPatch&) const = default;
};

struct SblBlockMesh {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::array<std::uint32_t, 6> exterior_flags{};
    std::uint32_t face_count{};
    std::uint32_t light_count{};
    std::vector<SblBlockFace> faces;
    std::vector<SblAuthoredPatch> authored_patches;
    std::vector<SblBlockLight> lights;

    bool operator==(const SblBlockMesh&) const = default;
};

struct SblDoor {
    std::uint32_t type{};
    float x{};
    float y{};

    bool operator==(const SblDoor&) const = default;
};

struct SblPortalRect {
    std::uint32_t type{};
    float x{};
    float width{};
    float y{};
    float height{};

    bool operator==(const SblPortalRect&) const = default;
};

struct SblLayer {
    std::size_t index{};
    std::size_t offset{};
    // Buildblock portal layer record. Side code sc 3..15 maps to layers[sc - 3].
    std::uint32_t portal_type{};
    std::array<float, 4> bounds{};
    std::uint32_t door_count{};
    std::vector<SblDoor> doors;
    std::uint32_t window_count{};
    std::vector<SblPortalRect> windows;
    std::uint32_t beam_count{};
    std::vector<SblPortalRect> beams;

    bool operator==(const SblLayer&) const = default;
};

struct SblTemplate {
    std::uint32_t version{1};
    // If this is set, the .sbl is a block mesh. If not, the same file is a buildblock
    // .sbl, whose layers shape the portal and edge walls.
    std::optional<SblBlockMesh> block;
    std::vector<SblLayer> layers;

    bool operator==(const SblTemplate&) const = default;
};

using SblBuildblockTemplate = SblTemplate;

enum class CsxEntryKind {
    Sbl,
    Sbb,
    Texture,
    Unknown,
};

// One item in an item set (data/itemset/*.sis, load_item_set): the item's type by
// name, its position from the centre of the cell's floor, and its rotation rows.
struct SisItem {
    std::string type;
    Vec3f pos;
    std::array<Vec3f, 3> rot;
};

// An item's render mesh (data/item/*.it3, load_it3_file version 3, chunk 0):
// vertices, and faces of three or four vertex indices. UVs are skipped.
struct It3Mesh {
    std::vector<Vec3f> vertices;
    std::vector<std::vector<std::uint32_t>> faces;
};

struct CsxEntry {
    std::size_t index{};
    CsxEntryKind kind{CsxEntryKind::Unknown};
    std::uint32_t type{};
    std::uint32_t offset{};
    std::uint32_t size{};
    std::string name;
    std::vector<std::uint8_t> payload;
};

struct CsxArchive {
    std::uint32_t magic{};
    std::uint32_t directory_offset{};
    std::uint32_t entry_count{};
    std::vector<CsxEntry> entries;
};

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path);
struct FileWrite {
    std::filesystem::path path;
    std::span<const std::uint8_t> bytes;
};
// Prepares and closes every output file before replacing anything, and puts the
// earlier files back if a replacement fails. This can't make a multi-file save safe
// from a power cut.
void write_files_transaction(std::span<const FileWrite> files);
void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
SbbBuilding parse_sbb(const std::vector<std::uint8_t>& bytes);
SbbBuilding parse_sbb_file(const std::filesystem::path& path);
SbcMap parse_sbc(const std::vector<std::uint8_t>& bytes);
SbcMap parse_sbc_file(const std::filesystem::path& path);
SblTemplate parse_sbl(const std::vector<std::uint8_t>& bytes);
SblTemplate parse_sbl_file(const std::filesystem::path& path);
CsxArchive parse_csx(const std::vector<std::uint8_t>& bytes);
std::vector<SisItem> parse_sis(const std::vector<std::uint8_t>& bytes);
It3Mesh parse_it3(const std::vector<std::uint8_t>& bytes);
CsxArchive parse_csx_file(const std::filesystem::path& path);
void refresh_sbl_block_faces(SblBlockMesh& block);
std::vector<std::uint8_t> patch_sbb_cell(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building, const SbbCell& cell);
std::vector<std::uint8_t> patch_sbb_cells(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building);
// Writes the v14 layout we support, including the anchors the parser fills in for
// v12 input. CSX packing replaces the leading version with the container's 1.
std::vector<std::uint8_t> rebuild_sbb(const SbbBuilding& building);
std::vector<std::uint8_t> patch_sbc_building(const std::vector<std::uint8_t>& bytes, const SbcBuilding& building, std::uint32_t version);
std::vector<std::uint8_t> patch_sbc_buildings(const std::vector<std::uint8_t>& bytes, const SbcMap& map);
std::vector<std::uint8_t> patch_sbc_streets(const std::vector<std::uint8_t>& bytes, const SbcMap& map);
std::vector<std::uint8_t> patch_sbc_city_blocks(const std::vector<std::uint8_t>& bytes, const SbcMap& map);
std::vector<std::uint8_t> patch_sbc_portals(const std::vector<std::uint8_t>& bytes, const SbcMap& map);
std::vector<std::uint8_t> rebuild_sbc_layout(const std::vector<std::uint8_t>& bytes, const SbcMap& map);
std::vector<std::uint8_t> rebuild_sbc(const SbcMap& map);
std::vector<std::uint8_t> patch_sbl_word(const std::vector<std::uint8_t>& bytes, std::size_t word_index, std::uint32_t value);
std::vector<std::uint8_t> patch_sbl_block_header(const std::vector<std::uint8_t>& bytes, const SblBlockMesh& block);
std::vector<std::uint8_t> patch_sbl_block_patches(const std::vector<std::uint8_t>& bytes, const SblBlockMesh& block);
std::vector<std::uint8_t> patch_sbl_layer_portal_type(
    const std::vector<std::uint8_t>& bytes,
    const SblTemplate& template_file,
    std::size_t layer_index,
    std::uint32_t portal_type);
std::vector<std::uint8_t> rebuild_sbl_block(const SblBlockMesh& block);
// Works out a cell's block references and filled flag again from its words (as
// parsing does).
void refresh_sbb_cell_payloads(SbbCell& cell);
// The quads one patch or one box record makes, built the way the game builds them.
std::vector<SblBlockFace> sbl_patch_faces(const SblAuthoredPatch& patch);
std::vector<SblBlockFace> sbl_box_faces(const SblBlockLight& box);
// Writes the block as stored (version 2), keeping the side flags, patches, and boxes.
std::vector<std::uint8_t> write_sbl_block(const SblBlockMesh& block);
std::vector<std::uint8_t> rebuild_sbl_buildblock(const SblTemplate& template_file);
std::vector<std::uint8_t> patch_sbl_layers(const std::vector<std::uint8_t>& bytes, const SblTemplate& template_file);
std::vector<std::uint8_t> build_csx(const CsxArchive& archive);

} // namespace pafta
