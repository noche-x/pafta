#include "formats.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void same_bytes(const Bytes& actual, const Bytes& expected, const std::string& name) {
    if (actual == expected) return;
    const auto difference = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
    throw std::runtime_error(name + ": byte mismatch at " +
        std::to_string(static_cast<std::size_t>(difference.first - actual.begin())) +
        " (actual size " + std::to_string(actual.size()) +
        ", expected size " + std::to_string(expected.size()) + ")");
}

void word(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void name(Bytes& bytes, const std::string& value, std::size_t width) {
    require(value.size() < width, "test name exceeds field width");
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.resize(bytes.size() + width - value.size());
}

void tests_sbb_export() {
    map_editor::SbbBuilding building{};
    building.version = 14;
    building.name = "test";
    building.size_z = 1;
    building.anchor_x = 4;
    building.anchor_y = 5;
    building.anchor_z = 6;
    building.materials = {"brick"};
    building.piece_names = {"floor"};
    building.block_names = {"door"};
    building.item_set_names = {"desk"};
    map_editor::SbbCell cell{};
    cell.exterior_block() = 0x40000000U;
    cell.interior_portal_face() = 7;
    cell.shorts[0] = 9;
    cell.item_set_index = *map_editor::sbb_item_set_payload(0, 2);
    building.cells.push_back(cell);
    building.lod_tail = {1, 0, 0, 0, 0, 0, 0, 0};

    Bytes expected;
    word(expected, 14);
    name(expected, "test", 64);
    for (const auto value : {0U, 0U, 1U, 4U, 5U, 6U}) word(expected, value);
    for (const auto& value : {"brick", "floor", "door", "desk"}) {
        word(expected, 1);
        name(expected, value, 64);
    }
    for (const auto value : cell.words) word(expected, value);
    word(expected, 9); // First two 16-bit texture fields.
    expected.resize(expected.size() + 28);
    word(expected, 0x42000000U);
    expected.insert(expected.end(), building.lod_tail.begin(), building.lod_tail.end());

    const auto exported = map_editor::rebuild_sbb(building);
    same_bytes(exported, expected, "SBB frozen export");
    same_bytes(map_editor::rebuild_sbb(map_editor::parse_sbb(exported)), expected, "SBB second export");
    const auto parsed = map_editor::parse_sbb(exported);
    same_bytes(map_editor::patch_sbb_cells(exported, parsed), expected, "SBB no-op patch");
    auto edited = parsed.cells[0];
    edited.item_set_index = *map_editor::sbb_item_set_payload(0, 3);
    auto changed = expected;
    changed[changed.size() - building.lod_tail.size() - 4] = 0;
    changed[changed.size() - building.lod_tail.size() - 3] = 0;
    changed[changed.size() - building.lod_tail.size() - 2] = 0;
    changed[changed.size() - building.lod_tail.size() - 1] = 0x43;
    same_bytes(map_editor::patch_sbb_cell(exported, parsed, edited), changed, "SBB focused patch");

    edited.index = 10000;
    bool rejected = false;
    try { (void)map_editor::patch_sbb_cell(exported, parsed, edited); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "SBB patch reported success for an out-of-range cell");
}

void tests_sbc_export() {
    map_editor::SbcMap map{};
    map.version = 12;
    map.intersections.push_back({0, 1, 2, 3});
    map.streets.push_back({0, 0, 0, 4, 5, 6, "road"});
    map.buildings.push_back({0, "house", 7, 8, 9, 2});

    Bytes expected;
    word(expected, 12); word(expected, 0); // Version and header records.
    word(expected, 1);
    for (const auto value : {1U, 2U, 3U}) word(expected, value);
    word(expected, 1);
    for (const auto value : {0U, 0U, 4U, 5U, 6U}) word(expected, value);
    name(expected, "road", 32);
    word(expected, 1);
    name(expected, "house", 64);
    for (const auto value : {7U, 8U, 9U, 2U}) word(expected, value);
    for (int i = 0; i < 3; ++i) word(expected, 0); // City blocks, portals, connections.

    const auto exported = map_editor::rebuild_sbc(map);
    same_bytes(exported, expected, "SBC frozen export");
    const auto parsed = map_editor::parse_sbc(exported);
    same_bytes(map_editor::rebuild_sbc(parsed), expected, "SBC second export");
    same_bytes(map_editor::patch_sbc_buildings(exported, parsed), expected, "SBC building no-op patch");
    same_bytes(map_editor::patch_sbc_streets(exported, parsed), expected, "SBC street no-op patch");
    same_bytes(map_editor::rebuild_sbc_layout(exported, parsed), expected, "SBC layout no-op export");

    // Some native files contain nonzero unused name bytes. Editing a different
    // field must keep those bytes, even though a full rebuild normalizes them.
    auto padded = exported;
    const std::string building_name = "house";
    const auto house = std::search(padded.begin(), padded.end(), building_name.begin(), building_name.end());
    require(house != padded.end(), "SBC test building name missing");
    const auto padding = static_cast<std::size_t>(house - padded.begin()) + 6;
    padded[padding] = 0xa5;
    const auto padded_map = map_editor::parse_sbc(padded);
    same_bytes(map_editor::patch_sbc_buildings(padded, padded_map), padded, "SBC preserves unused name bytes");
    auto moved = padded_map.buildings[0];
    moved.x += 1;
    const auto moved_bytes = map_editor::patch_sbc_building(padded, moved, padded_map.version);
    require(moved_bytes[padding] == 0xa5, "SBC coordinate edit changed unused name bytes");
    require(map_editor::parse_sbc(moved_bytes).buildings[0].x == moved.x, "SBC coordinate edit was lost");
}

void tests_sbl_exports() {
    map_editor::SblBlockMesh block{};
    block.width = 2;
    block.height = 3;
    block.depth = 4;
    block.exterior_flags[0] = 7;
    Bytes block_expected;
    for (const auto value : {2U, 2U, 3U, 4U, 7U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}) word(block_expected, value);
    const auto block_export = map_editor::write_sbl_block(block);
    same_bytes(block_export, block_expected, "SBL block frozen export");
    const auto parsed_block = map_editor::parse_sbl(block_export);
    require(parsed_block.block.has_value(), "SBL block did not parse");
    same_bytes(map_editor::write_sbl_block(*parsed_block.block), block_expected, "SBL block second export");
    same_bytes(map_editor::patch_sbl_block_header(block_export, *parsed_block.block), block_expected, "SBL block no-op patch");

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(1);
    buildblock.layers[0].portal_type = 9;
    buildblock.layers[0].bounds = {0, 1, 0.5F, 2};
    buildblock.layers[0].doors.push_back({3, 0.25F, 0.75F});
    Bytes buildblock_expected;
    for (const auto value : {1U, 9U, 0U, 0x3f800000U, 0x3f000000U, 0x40000000U,
                             1U, 3U, 0x3e800000U, 0x3f400000U, 0U, 0U}) word(buildblock_expected, value);
    const auto buildblock_export = map_editor::rebuild_sbl_buildblock(buildblock);
    same_bytes(buildblock_export, buildblock_expected, "SBL buildblock frozen export");
    const auto parsed_buildblock = map_editor::parse_sbl(buildblock_export);
    same_bytes(map_editor::rebuild_sbl_buildblock(parsed_buildblock), buildblock_expected, "SBL buildblock second export");
    same_bytes(map_editor::patch_sbl_layers(buildblock_export, parsed_buildblock), buildblock_expected, "SBL layer no-op patch");
    same_bytes(map_editor::patch_sbl_layer_portal_type(buildblock_export, parsed_buildblock, 0, 9),
        buildblock_expected, "SBL portal no-op patch");
}

void tests_csx_export() {
    map_editor::CsxArchive archive{};
    archive.magic = 0x43535801U;
    archive.entries.push_back({0, map_editor::CsxEntryKind::Unknown, 17, 0, 0, "item", {1, 2, 3}});
    Bytes expected;
    word(expected, archive.magic); word(expected, 15); word(expected, 1);
    expected.insert(expected.end(), {1, 2, 3});
    word(expected, 17); word(expected, 12); word(expected, 3);
    name(expected, "item", 0x34);
    same_bytes(map_editor::build_csx(archive), expected, "CSX frozen export");
    same_bytes(map_editor::build_csx(map_editor::parse_csx(expected)), expected, "CSX second export");
}

void fixture_exports(const std::filesystem::path& root) {
    std::size_t sbb_exact = 0, sbb_canonical = 0, sbc_exact = 0, sbc_canonical = 0;
    std::size_t sbl_block_count = 0, sbl_buildblock_exact = 0, sbl_buildblock_canonical = 0;
    std::size_t csx_exact = 0, csx_canonical = 0;
    const auto audit = [&](const Bytes& original, const Bytes& exported, const Bytes& second,
                           const std::string& label, std::size_t& exact, std::size_t& canonical) {
        same_bytes(second, exported, label + " second export");
        if (exported == original) ++exact;
        else {
            ++canonical;
            if (canonical <= 3) std::cout << "Canonicalized: " << label << '\n';
        }
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension();
        if (ext != ".sbb" && ext != ".sbc" && ext != ".sbl" && ext != ".csx") continue;
        const auto bytes = map_editor::read_file_bytes(entry.path());
        const auto label = entry.path().string();
        if (ext == ".sbb") {
            const auto parsed = map_editor::parse_sbb(bytes);
            same_bytes(map_editor::patch_sbb_cells(bytes, parsed), bytes, label + " SBB no-op patch");
            if (!parsed.cells.empty())
                same_bytes(map_editor::patch_sbb_cell(bytes, parsed, parsed.cells.front()), bytes, label + " SBB cell no-op patch");
            if (parsed.version == 14) {
                const auto exported = map_editor::rebuild_sbb(parsed);
                audit(bytes, exported, map_editor::rebuild_sbb(map_editor::parse_sbb(exported)),
                    label + " SBB", sbb_exact, sbb_canonical);
            }
        } else if (ext == ".sbc") {
            const auto parsed = map_editor::parse_sbc(bytes);
            same_bytes(map_editor::patch_sbc_buildings(bytes, parsed), bytes, label + " SBC building no-op patch");
            same_bytes(map_editor::patch_sbc_streets(bytes, parsed), bytes, label + " SBC street no-op patch");
            same_bytes(map_editor::patch_sbc_city_blocks(bytes, parsed), bytes, label + " SBC city-block no-op patch");
            same_bytes(map_editor::patch_sbc_portals(bytes, parsed), bytes, label + " SBC portal no-op patch");
            if (parsed.version == 12) {
                const auto exported = map_editor::rebuild_sbc(parsed);
                audit(bytes, exported, map_editor::rebuild_sbc(map_editor::parse_sbc(exported)),
                    label + " SBC", sbc_exact, sbc_canonical);
            }
        } else if (ext == ".sbl") {
            const auto parsed = map_editor::parse_sbl(bytes);
            if (parsed.block && parsed.version == 2) {
                same_bytes(map_editor::write_sbl_block(*parsed.block), bytes, label + " SBL block export");
                same_bytes(map_editor::patch_sbl_block_header(bytes, *parsed.block), bytes, label + " SBL block header no-op patch");
                same_bytes(map_editor::patch_sbl_block_patches(bytes, *parsed.block), bytes, label + " SBL block patch no-op export");
                ++sbl_block_count;
            } else if (!parsed.block && parsed.version == 1) {
                same_bytes(map_editor::patch_sbl_layers(bytes, parsed), bytes, label + " SBL layer no-op patch");
                if (!parsed.layers.empty())
                    same_bytes(map_editor::patch_sbl_layer_portal_type(bytes, parsed, 0, parsed.layers[0].portal_type),
                        bytes, label + " SBL portal no-op patch");
                const auto exported = map_editor::rebuild_sbl_buildblock(parsed);
                audit(bytes, exported, map_editor::rebuild_sbl_buildblock(map_editor::parse_sbl(exported)),
                    label + " SBL buildblock", sbl_buildblock_exact, sbl_buildblock_canonical);
            }
        } else {
            const auto exported = map_editor::build_csx(map_editor::parse_csx(bytes));
            audit(bytes, exported, map_editor::build_csx(map_editor::parse_csx(exported)),
                label + " CSX", csx_exact, csx_canonical);
        }
    }
    require(sbl_block_count > 10 && sbl_buildblock_exact + sbl_buildblock_canonical > 0 && csx_exact + csx_canonical > 0,
        "fixture scan did not cover the expected formats");
    std::cout << "Fixture exports (exact/canonicalized): SBB " << sbb_exact << '/' << sbb_canonical
              << ", SBC " << sbc_exact << '/' << sbc_canonical
              << ", SBL block " << sbl_block_count << "/0, SBL buildblock "
              << sbl_buildblock_exact << '/' << sbl_buildblock_canonical
              << ", CSX " << csx_exact << '/' << csx_canonical << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        tests_sbb_export();
        tests_sbc_export();
        tests_sbl_exports();
        tests_csx_export();
        if (argc == 2) fixture_exports(argv[1]);
        else require(argc == 1, "usage: binary_export_tests [fixture-directory]");
        std::cout << "Frozen binary exports match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
