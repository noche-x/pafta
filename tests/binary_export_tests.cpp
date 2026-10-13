// SPDX-License-Identifier: GPL-2.0-only
#include "formats.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/stat.h>
#endif

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
    pafta::SbbBuilding building{};
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
    pafta::SbbCell cell{};
    cell.exterior_block() = 0x40000000U;
    cell.interior_portal_face() = 7;
    cell.shorts[0] = 9;
    cell.item_set_index = *pafta::sbb_item_set_payload(0, 2);
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

    const auto exported = pafta::rebuild_sbb(building);
    same_bytes(exported, expected, "SBB frozen export");
    same_bytes(pafta::rebuild_sbb(pafta::parse_sbb(exported)), expected, "SBB second export");
    const auto parsed = pafta::parse_sbb(exported);
    same_bytes(pafta::patch_sbb_cells(exported, parsed), expected, "SBB no-op patch");
    auto edited = parsed.cells[0];
    edited.item_set_index = *pafta::sbb_item_set_payload(0, 3);
    auto changed = expected;
    changed[changed.size() - building.lod_tail.size() - 4] = 0;
    changed[changed.size() - building.lod_tail.size() - 3] = 0;
    changed[changed.size() - building.lod_tail.size() - 2] = 0;
    changed[changed.size() - building.lod_tail.size() - 1] = 0x43;
    same_bytes(pafta::patch_sbb_cell(exported, parsed, edited), changed, "SBB focused patch");

    edited.index = 10000;
    bool rejected = false;
    try { (void)pafta::patch_sbb_cell(exported, parsed, edited); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "SBB patch reported success for an out-of-range cell");
}

void tests_sbc_export() {
    pafta::SbcMap map{};
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

    const auto exported = pafta::rebuild_sbc(map);
    same_bytes(exported, expected, "SBC frozen export");
    const auto parsed = pafta::parse_sbc(exported);
    same_bytes(pafta::rebuild_sbc(parsed), expected, "SBC second export");
    same_bytes(pafta::patch_sbc_buildings(exported, parsed), expected, "SBC building no-op patch");
    same_bytes(pafta::patch_sbc_streets(exported, parsed), expected, "SBC street no-op patch");
    same_bytes(pafta::rebuild_sbc_layout(exported, parsed), expected, "SBC layout no-op export");

    // Some of the game's files have non-zero bytes in unused parts of names. Editing a
    // different field must keep those bytes, even though a full rebuild clears them.
    auto padded = exported;
    const std::string building_name = "house";
    const auto house = std::search(padded.begin(), padded.end(), building_name.begin(), building_name.end());
    require(house != padded.end(), "SBC test building name missing");
    const auto padding = static_cast<std::size_t>(house - padded.begin()) + 6;
    padded[padding] = 0xa5;
    const auto padded_map = pafta::parse_sbc(padded);
    same_bytes(pafta::patch_sbc_buildings(padded, padded_map), padded, "SBC preserves unused name bytes");
    auto moved = padded_map.buildings[0];
    moved.x += 1;
    const auto moved_bytes = pafta::patch_sbc_building(padded, moved, padded_map.version);
    require(moved_bytes[padding] == 0xa5, "SBC coordinate edit changed unused name bytes");
    require(pafta::parse_sbc(moved_bytes).buildings[0].x == moved.x, "SBC coordinate edit was lost");
}

void tests_sbl_exports() {
    pafta::SblBlockMesh block{};
    block.width = 2;
    block.height = 3;
    block.depth = 4;
    block.exterior_flags[0] = 7;
    Bytes block_expected;
    for (const auto value : {2U, 2U, 3U, 4U, 7U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}) word(block_expected, value);
    const auto block_export = pafta::write_sbl_block(block);
    same_bytes(block_export, block_expected, "SBL block frozen export");
    const auto parsed_block = pafta::parse_sbl(block_export);
    require(parsed_block.block.has_value(), "SBL block did not parse");
    same_bytes(pafta::write_sbl_block(*parsed_block.block), block_expected, "SBL block second export");
    same_bytes(pafta::patch_sbl_block_header(block_export, *parsed_block.block), block_expected, "SBL block no-op patch");

    pafta::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(1);
    buildblock.layers[0].portal_type = 9;
    buildblock.layers[0].bounds = {0, 1, 0.5F, 2};
    buildblock.layers[0].doors.push_back({3, 0.25F, 0.75F});
    Bytes buildblock_expected;
    for (const auto value : {1U, 9U, 0U, 0x3f800000U, 0x3f000000U, 0x40000000U,
                             1U, 3U, 0x3e800000U, 0x3f400000U, 0U, 0U}) word(buildblock_expected, value);
    const auto buildblock_export = pafta::rebuild_sbl_buildblock(buildblock);
    same_bytes(buildblock_export, buildblock_expected, "SBL buildblock frozen export");
    const auto parsed_buildblock = pafta::parse_sbl(buildblock_export);
    same_bytes(pafta::rebuild_sbl_buildblock(parsed_buildblock), buildblock_expected, "SBL buildblock second export");
    same_bytes(pafta::patch_sbl_layers(buildblock_export, parsed_buildblock), buildblock_expected, "SBL layer no-op patch");
    same_bytes(pafta::patch_sbl_layer_portal_type(buildblock_export, parsed_buildblock, 0, 9),
        buildblock_expected, "SBL portal no-op patch");
}

void tests_csx_export() {
    pafta::CsxArchive archive{};
    archive.magic = 0x43535801U;
    archive.entries.push_back({0, pafta::CsxEntryKind::Unknown, 17, 0, 0, "item", {1, 2, 3}});
    Bytes expected;
    word(expected, archive.magic); word(expected, 15); word(expected, 1);
    expected.insert(expected.end(), {1, 2, 3});
    word(expected, 17); word(expected, 12); word(expected, 3);
    name(expected, "item", 0x34);
    same_bytes(pafta::build_csx(archive), expected, "CSX frozen export");
    same_bytes(pafta::build_csx(pafta::parse_csx(expected)), expected, "CSX second export");
}

void tests_native_uv_orientation() {
    const pafta::Vec3f position{2, 3, 4};
    const pafta::Vec2f authored{0.25F, 0.75F};
    require(pafta::native_oriented_uv(position, authored, 0x10000U) == pafta::Vec2f{4, 3}, "negative X UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0x20000U) == pafta::Vec2f{-3, 3}, "positive X UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0x30000U) == pafta::Vec2f{2, 4}, "negative Y UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0x40000U) == pafta::Vec2f{2, -3}, "positive Y UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0x50000U) == pafta::Vec2f{-1, 3}, "negative Z UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0x60000U) == pafta::Vec2f{2, 3}, "positive Z UV projection changed");
    require(pafta::native_oriented_uv(position, authored, 0) == authored, "zero orientation lost authored UVs");
    require(pafta::native_oriented_uv(position, authored, 0x70000U) == authored, "unknown orientation lost authored UVs");
}

template<class Action>
void rejects(Action action, const std::string& label) {
    bool rejected = false;
    try { action(); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, label + " was accepted");
}

void put_word(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

void tests_malformed_files() {
    const Bytes word_bytes{1, 0, 0, 0};
    rejects([&] { (void)pafta::patch_sbl_word(word_bytes, 1, 9); }, "out-of-range SBL word edit");
    const auto overflowing_word = std::numeric_limits<std::size_t>::max() / 4U + 1U;
    rejects([&] { (void)pafta::patch_sbl_word(word_bytes, overflowing_word, 9); }, "overflowing SBL word edit");
    pafta::SbbBuilding building{};
    building.version = 14;
    building.size_z = 1;
    building.cells.resize(1);
    // Empty slots in a list still take up bytes and keep later indices the same.
    building.piece_names = {"", "mesh"};
    auto sbb = pafta::rebuild_sbb(building);
    const auto parsed = pafta::parse_sbb(sbb);
    require(parsed.piece_names == building.piece_names && parsed.cells.size() == 1,
        "empty SBB name slot changed the cell offset");
    same_bytes(pafta::rebuild_sbb(parsed), sbb, "empty SBB name slots");
    auto legacy_source = sbb;
    legacy_source.erase(legacy_source.begin() + 0x50, legacy_source.begin() + 0x5c);
    legacy_source.resize(legacy_source.size() - 8); // v12 has no LOD section.
    put_word(legacy_source, 0, 12);
    const auto legacy_building = pafta::parse_sbb(legacy_source);
    auto packed = pafta::rebuild_sbb(legacy_building);
    put_word(packed, 0, 1); // CSX container sentinel.
    auto normalized = packed;
    put_word(normalized, 0, 14); // Same normalization used by the project loader.
    const auto packed_building = pafta::parse_sbb(normalized);
    require(packed_building.version == 14 && packed_building.anchor_x == 4 && packed_building.anchor_y == 4 && packed_building.anchor_z == 4,
        "packed v12 upgrade lost its default anchors");
    require(packed_building.piece_names == legacy_building.piece_names && packed_building.cells == legacy_building.cells,
        "packed v12 upgrade changed the name tables or cells");
    same_bytes(pafta::rebuild_sbb(packed_building), normalized, "packed v12 body uses canonical v14 layout");
    auto truncated = sbb;
    truncated.resize(truncated.size() - 9); // Last cell byte, before the optional tail.
    rejects([&] { (void)pafta::parse_sbb(truncated); }, "truncated SBB cells");
    auto oversized = sbb;
    put_word(oversized, 0x44, UINT32_MAX);
    rejects([&] { (void)pafta::parse_sbb(oversized); }, "overflowing SBB dimension");
    oversized = sbb;
    put_word(oversized, 0x5c, UINT32_MAX);
    rejects([&] { (void)pafta::parse_sbb(oversized); }, "oversized SBB material table");
    sbb.resize(sbb.size() - 8);
    sbb.insert(sbb.end(), {0xaa, 0xbb, 0xcc});
    same_bytes(pafta::rebuild_sbb(pafta::parse_sbb(sbb)), sbb, "short unknown SBB tail");
    for (const auto dimensions : {std::array<std::uint32_t, 3>{64, 0, 1}, {0, 64, 1}, {0, 0, 65}}) {
        auto unsupported = building;
        unsupported.size_x = dimensions[0];
        unsupported.size_y = dimensions[1];
        unsupported.size_z = dimensions[2];
        rejects([&] { (void)pafta::rebuild_sbb(unsupported); }, "unsupported SBB export dimensions");
    }
    for (const auto coordinate : {std::array<std::uint32_t, 3>{64, 0, 0}, {0, 64, 0}, {0, 0, 64}, {0, UINT32_MAX, 0}}) {
        auto unsupported = building;
        auto& cell = unsupported.cells.front();
        cell.exterior_block() = 0x40000000U;
        cell.x = coordinate[0];
        cell.y = coordinate[1];
        cell.z = coordinate[2];
        rejects([&] { (void)pafta::rebuild_sbb(unsupported); }, "unsupported SBB export cell coordinate");
    }

    pafta::SbcMap map{};
    map.version = 12;
    pafta::SbcPortal portal{};
    portal.blocks.resize(512, 0x80000001U);
    portal.itemsets.resize(512, 7);
    map.portals.push_back(portal);
    map.portal_connections.push_back({0, {1, 2, 3}});
    const auto sbc = pafta::rebuild_sbc(map);
    // Every prefix of this file is incomplete, including missing section headers with a
    // count of zero and cut-off portal and connection data.
    for (std::size_t size = 0; size < sbc.size(); ++size) {
        truncated.assign(sbc.begin(), sbc.begin() + static_cast<std::ptrdiff_t>(size));
        rejects([&] { (void)pafta::parse_sbc(truncated); }, "SBC prefix " + std::to_string(size));
    }
    for (const auto offset : {4U, 8U, 12U, 16U, 20U, 24U}) {
        oversized = sbc;
        put_word(oversized, offset, UINT32_MAX);
        rejects([&] { (void)pafta::parse_sbc(oversized); }, "oversized SBC count at " + std::to_string(offset));
    }

    pafta::SblBlockMesh block{};
    block.width = block.height = block.depth = 1;
    auto sbl = pafta::write_sbl_block(block);
    put_word(sbl, 40, 1);
    rejects([&] { (void)pafta::parse_sbl(sbl); }, "missing SBL authored patch");
    put_word(sbl, 40, UINT32_MAX);
    rejects([&] { (void)pafta::parse_sbl(sbl); }, "oversized SBL patch count");
    sbl = pafta::write_sbl_block(block);
    put_word(sbl, 44, 1);
    rejects([&] { (void)pafta::parse_sbl(sbl); }, "missing SBL box");
    pafta::SblTemplate buildblock{};
    buildblock.layers.resize(1);
    sbl = pafta::rebuild_sbl_buildblock(buildblock);
    sbl.pop_back();
    rejects([&] { (void)pafta::parse_sbl(sbl); }, "missing SBL beam-count byte");

    pafta::CsxArchive archive{};
    archive.magic = 0xc5f17e01U;
    archive.entries.push_back({0, pafta::CsxEntryKind::Unknown, 99, 0, 0, "item", {1, 2, 3}});
    const auto csx = pafta::build_csx(archive);
    truncated = csx;
    truncated.pop_back();
    rejects([&] { (void)pafta::parse_csx(truncated); }, "truncated CSX directory");
    oversized = csx;
    put_word(oversized, 8, UINT32_MAX);
    rejects([&] { (void)pafta::parse_csx(oversized); }, "oversized CSX directory count");
    oversized = csx;
    put_word(oversized, 15 + 4, UINT32_MAX);
    rejects([&] { (void)pafta::parse_csx(oversized); }, "out-of-range CSX payload");
    oversized = csx;
    put_word(oversized, 15 + 8, UINT32_MAX);
    rejects([&] { (void)pafta::parse_csx(oversized); }, "overflowing CSX payload size");

    Bytes sis;
    word(sis, 1); word(sis, UINT32_MAX);
    rejects([&] { (void)pafta::parse_sis(sis); }, "oversized SIS item count");
    Bytes it3;
    word(it3, 3); word(it3, 0); word(it3, 0); word(it3, UINT32_MAX);
    rejects([&] { (void)pafta::parse_it3(it3); }, "oversized IT3 vertex count");
}

void tests_file_saves() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("pafta-save-test-" + std::to_string(stamp));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    const auto first = root / "city2.sbc";
    const auto second = root / "test.csx";
    const Bytes before{1, 2, 3, 4}, after{5, 6, 7, 8};
    pafta::write_file_bytes(first, before);
    pafta::write_file_bytes(second, before);
    const std::array<pafta::FileWrite, 2> outputs{{{first, after}, {second, after}}};
    pafta::write_files_transaction(outputs);
    same_bytes(pafta::read_file_bytes(first), after, "saved city");
    same_bytes(pafta::read_file_bytes(second), after, "saved archive");

    const auto blocked = root / "blocked";
    std::filesystem::create_directory(blocked);
    const std::array<pafta::FileWrite, 2> bad_outputs{{{first, before}, {blocked, before}}};
    rejects([&] { pafta::write_files_transaction(bad_outputs); }, "directory output");
    same_bytes(pafta::read_file_bytes(first), after, "failed staging preserved original");
    const std::array<pafta::FileWrite, 2> duplicate{{{first, before}, {first, after}}};
    rejects([&] { pafta::write_files_transaction(duplicate); }, "duplicate output paths");
    same_bytes(pafta::read_file_bytes(first), after, "duplicate save preserved original");

#ifndef _WIN32
    // A small write fits in the stream buffer, then fails when flushing hits the OS file
    // size limit. Run it in a child process so the limit can't affect CTest.
    const auto child = fork();
    require(child >= 0, "could not fork flush-failure test");
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        const rlimit limit{8, 8};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        try { pafta::write_file_bytes(first, Bytes(64, 9)); }
        catch (const std::runtime_error& error) {
            _exit(std::string{error.what()}.find("flush") != std::string::npos ? 0 : 3);
        }
        _exit(4);
    }
    int child_status = 0;
    require(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
        "staged write did not report its flush failure");
    same_bytes(pafta::read_file_bytes(first), after, "flush failure preserved original");
#endif
#ifdef __APPLE__
    // The second replacement fails after the first works. An original file that can't be
    // changed can still be read and prepared, but can't be renamed as a backup.
    require(chflags(second.c_str(), UF_IMMUTABLE) == 0, "could not set replacement-failure fixture flag");
    bool failed = false;
    try { pafta::write_files_transaction(std::array<pafta::FileWrite, 2>{{{first, before}, {second, before}}}); }
    catch (const std::runtime_error&) { failed = true; }
    const auto cleared = chflags(second.c_str(), 0);
    require(cleared == 0, "could not clear replacement-failure fixture flag");
    require(failed, "immutable second output did not fail replacement");
    same_bytes(pafta::read_file_bytes(first), after, "replacement failure rolled back first output");
    same_bytes(pafta::read_file_bytes(second), after, "replacement failure preserved second output");
#endif
    for (const auto& entry : std::filesystem::directory_iterator(root))
        require(entry.path().filename().string().find(".pafta-save-") == std::string::npos,
            "save left a staging or recovery directory after success or rollback");
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
        const auto bytes = pafta::read_file_bytes(entry.path());
        const auto label = entry.path().string();
        if (ext == ".sbb") {
            const auto parsed = pafta::parse_sbb(bytes);
            same_bytes(pafta::patch_sbb_cells(bytes, parsed), bytes, label + " SBB no-op patch");
            if (!parsed.cells.empty())
                same_bytes(pafta::patch_sbb_cell(bytes, parsed, parsed.cells.front()), bytes, label + " SBB cell no-op patch");
            if (parsed.version == 14) {
                const auto exported = pafta::rebuild_sbb(parsed);
                audit(bytes, exported, pafta::rebuild_sbb(pafta::parse_sbb(exported)),
                    label + " SBB", sbb_exact, sbb_canonical);
            }
        } else if (ext == ".sbc") {
            const auto parsed = pafta::parse_sbc(bytes);
            same_bytes(pafta::patch_sbc_buildings(bytes, parsed), bytes, label + " SBC building no-op patch");
            same_bytes(pafta::patch_sbc_streets(bytes, parsed), bytes, label + " SBC street no-op patch");
            same_bytes(pafta::patch_sbc_city_blocks(bytes, parsed), bytes, label + " SBC city-block no-op patch");
            same_bytes(pafta::patch_sbc_portals(bytes, parsed), bytes, label + " SBC portal no-op patch");
            if (parsed.version == 12) {
                const auto exported = pafta::rebuild_sbc(parsed);
                audit(bytes, exported, pafta::rebuild_sbc(pafta::parse_sbc(exported)),
                    label + " SBC", sbc_exact, sbc_canonical);
            }
        } else if (ext == ".sbl") {
            const auto parsed = pafta::parse_sbl(bytes);
            if (parsed.block && parsed.version == 2) {
                same_bytes(pafta::write_sbl_block(*parsed.block), bytes, label + " SBL block export");
                same_bytes(pafta::patch_sbl_block_header(bytes, *parsed.block), bytes, label + " SBL block header no-op patch");
                same_bytes(pafta::patch_sbl_block_patches(bytes, *parsed.block), bytes, label + " SBL block patch no-op export");
                ++sbl_block_count;
            } else if (!parsed.block && parsed.version == 1) {
                same_bytes(pafta::patch_sbl_layers(bytes, parsed), bytes, label + " SBL layer no-op patch");
                if (!parsed.layers.empty())
                    same_bytes(pafta::patch_sbl_layer_portal_type(bytes, parsed, 0, parsed.layers[0].portal_type),
                        bytes, label + " SBL portal no-op patch");
                const auto exported = pafta::rebuild_sbl_buildblock(parsed);
                audit(bytes, exported, pafta::rebuild_sbl_buildblock(pafta::parse_sbl(exported)),
                    label + " SBL buildblock", sbl_buildblock_exact, sbl_buildblock_canonical);
            }
        } else {
            const auto exported = pafta::build_csx(pafta::parse_csx(bytes));
            audit(bytes, exported, pafta::build_csx(pafta::parse_csx(exported)),
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
        tests_native_uv_orientation();
        tests_malformed_files();
        tests_file_saves();
        if (argc == 2) fixture_exports(argv[1]);
        else require(argc == 1, "usage: binary_export_tests [fixture-directory]");
        std::cout << "Frozen binary exports match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
