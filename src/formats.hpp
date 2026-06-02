#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace map_editor {

enum class PayloadKind {
    // Historical port bucket for 0x40000000 solid/buildblock cells.
    Block,
    // 0x80000000 standalone SblBlockMesh payload; low 10 bits select piece name/mesh.
    Piece,
    Other,
};

struct SbbCellPayload {
    PayloadKind kind{};
    std::uint32_t word{};
    // Block/Piece id decoded from low payload bits. Piece uses word & 0x3ff
    // in native UI paths; legacy block payload tests keep the broader low word.
    std::uint32_t index{};
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
    // Native SBB file order from SaveBuilding_SBB differs from raw BuildingCell
    // memory order. SaveBuilding_SBB_1400548d0.decomp.txt:76-99 writes:
    // exteriorBlock, interiorBlock, cellType, exteriorPortalFace,
    // interiorPortalFace, placedFlag. Raw BuildingCell offsets are verified in
    // BuildingTool_ReadCellValueOrZero_140067650.c:11-20: cellType +0x00,
    // exteriorBlock +0x04, interiorBlock +0x08, exteriorPortalFace +0x0c,
    // interiorPortalFace +0x10, placedFlag +0x14, texture sets +0x18/+0x28,
    // itemsetPayload +0x38.
    std::array<std::uint32_t, 6> words{};
    // RE itemsetPayload +0x38; SBB file offset +0x38,
    // SaveBuilding_SBB_1400548d0.decomp.txt:164-167.
    std::uint32_t item_set_index{};
    bool occupied{};
    std::vector<SbbCellPayload> payloads;
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
};

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
    std::vector<std::string> piece_names; // standalone piece meshes, indexed by Piece payload low bits.
    std::vector<std::string> block_names; // buildblock templates, indexed by SbbCell::cell_type().
    std::vector<std::string> item_set_names;
    std::vector<SbbCell> cells;
    std::uint32_t lod_marker{};
    std::uint32_t lod_group_count{};
    std::vector<std::uint8_t> lod_tail;
};

struct SbcIntersection {
    std::size_t index{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
};

struct SbcStreet {
    std::size_t index{};
    std::int32_t from_intersection{};
    std::int32_t to_intersection{};
    std::int32_t district{};
    std::int32_t left_width{};
    std::int32_t right_width{};
    std::string name;
};

struct SbcBuilding {
    std::size_t index{};
    std::string name;
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    std::int32_t rotation{};
};

struct SbcCityBlock {
    std::size_t index{};
    std::string name;
    std::vector<std::array<std::string, 8>> layers;
};

struct SbcPortal {
    std::size_t index{};
    std::array<std::int32_t, 4> fields{};
    std::vector<std::uint32_t> blocks;
    std::vector<std::uint32_t> legacy_blocks;
    std::vector<std::uint16_t> textures;
    std::vector<std::uint32_t> itemsets;
};

struct SbcPortalConnection {
    std::size_t index{};
    std::array<std::int32_t, 3> fields{};
};

struct SbcMap {
    std::uint32_t version{};
    std::vector<SbcIntersection> intersections;
    std::vector<SbcStreet> streets;
    std::vector<SbcBuilding> buildings;
    std::vector<SbcCityBlock> city_blocks;
    std::vector<SbcPortal> portals;
    std::vector<SbcPortalConnection> portal_connections;
};

struct Vec3f {
    float x{};
    float y{};
    float z{};
};

struct Vec2f {
    float u{};
    float v{};
};

struct SblBlockFace {
    std::uint32_t material{};
    std::vector<Vec3f> vertices;
    std::vector<Vec2f> uvs;
    bool authored{};
    std::uint32_t orientation{};
};

struct SblBlockLight {
    std::array<Vec3f, 8> points{};
    std::array<std::uint32_t, 6> scalars{};
    std::uint32_t final_value{};
};

struct SblControlPoint {
    Vec3f position{};
    float u{};
    float v{};
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
};

struct SblDoor {
    std::uint32_t type{};
    float x{};
    float y{};
};

struct SblPortalRect {
    std::uint32_t type{};
    float x{};
    float width{};
    float y{};
    float height{};
};

struct SblLayer {
    std::size_t index{};
    std::size_t offset{};
    // Buildblock portal layer record. Side-code sc 3..15 maps to layers[sc - 3].
    std::uint32_t portal_type{};
    std::array<float, 4> bounds{};
    std::uint32_t door_count{};
    std::vector<SblDoor> doors;
    std::uint32_t window_count{};
    std::vector<SblPortalRect> windows;
    std::uint32_t beam_count{};
    std::vector<SblPortalRect> beams;
};

struct SblTemplate {
    std::uint32_t version{};
    // If present, this is a block mesh .sbl. If absent, the same container is a
    // buildblock .sbl whose layers drive portal/edge wall geometry.
    std::optional<SblBlockMesh> block;
    std::vector<SblLayer> layers;
};

using SblBuildblockTemplate = SblTemplate;

enum class CsxEntryKind {
    Sbl,
    Sbb,
    Texture,
    Unknown,
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
SbbBuilding parse_sbb(const std::vector<std::uint8_t>& bytes);
SbbBuilding parse_sbb_file(const std::filesystem::path& path);
SbcMap parse_sbc(const std::vector<std::uint8_t>& bytes);
SbcMap parse_sbc_file(const std::filesystem::path& path);
SblTemplate parse_sbl(const std::vector<std::uint8_t>& bytes);
SblTemplate parse_sbl_file(const std::filesystem::path& path);
CsxArchive parse_csx(const std::vector<std::uint8_t>& bytes);
CsxArchive parse_csx_file(const std::filesystem::path& path);
void refresh_sbl_block_faces(SblBlockMesh& block);
std::vector<std::uint8_t> patch_sbb_cell(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building, const SbbCell& cell);
std::vector<std::uint8_t> patch_sbb_cells(const std::vector<std::uint8_t>& bytes, const SbbBuilding& building);
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
std::vector<std::uint8_t> patch_sbl_layers(const std::vector<std::uint8_t>& bytes, const SblTemplate& template_file);
std::vector<std::uint8_t> build_csx(const CsxArchive& archive);

} // namespace map_editor
