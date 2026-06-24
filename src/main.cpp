#include "app.hpp"
#include "formats.hpp"
#include "profiler.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path default_fixture_path(char const* executable_path) {
    auto current = std::filesystem::absolute(executable_path).parent_path();
    for (int step = 0; step < 8; ++step) {
        const auto candidate = current / "../server/data/test2/building/test.sbb";
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::weakly_canonical(candidate);
        }
        current = current.parent_path();
    }
    return "test.sbb";
}

bool has_extension(const std::filesystem::path& path, std::string extension) {
    auto actual = path.extension().string();
    std::ranges::transform(actual, actual.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return actual == extension;
}

std::uint32_t parse_u32(std::string_view value, std::string_view label) {
    std::size_t parsed = 0;
    const auto number = std::stoull(std::string{value}, &parsed, 0);
    if (parsed != value.size() || number > UINT32_MAX) {
        throw std::runtime_error("invalid " + std::string{label});
    }
    return static_cast<std::uint32_t>(number);
}

std::int32_t parse_i32(std::string_view value, std::string_view label) {
    std::size_t parsed = 0;
    const auto number = std::stoll(std::string{value}, &parsed, 0);
    if (parsed != value.size() ||
        number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("invalid " + std::string{label});
    }
    return static_cast<std::int32_t>(number);
}

std::size_t parse_index(std::string_view value, std::string_view label) {
    std::size_t parsed = 0;
    const auto number = std::stoull(std::string{value}, &parsed, 0);
    if (parsed != value.size()) {
        throw std::runtime_error("invalid " + std::string{label});
    }
    return static_cast<std::size_t>(number);
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        throw std::runtime_error("could not open output file: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("could not write output file: " + path.string());
    }
}

std::uint32_t read_le_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("u32 read out of range");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

const map_editor::SbbCell& sbb_cell_at(const map_editor::SbbBuilding& building, std::size_t index) {
    if (index >= building.cells.size()) {
        throw std::runtime_error("SBB cell index out of range");
    }
    return building.cells[index];
}

const map_editor::SbcBuilding& sbc_building_at(const map_editor::SbcMap& map, std::size_t index) {
    if (index >= map.buildings.size()) {
        throw std::runtime_error("SBC building index out of range");
    }
    return map.buildings[index];
}

void set_sbb_cell_field(map_editor::SbbCell& cell, std::string_view field, std::uint32_t value) {
    if (field == "exterior_block" || field == "exterior") {
        cell.exterior_block() = value;
    } else if (field == "interior_block" || field == "interior") {
        cell.interior_block() = value;
    } else if (field == "cell_type" || field == "portal") {
        cell.cell_type() = value;
    } else if (field == "exterior_portal_face" || field == "edge_x") {
        cell.exterior_portal_face() = value;
    } else if (field == "interior_portal_face" || field == "edge_z") {
        cell.interior_portal_face() = value;
    } else if (field == "placed_flag" || field == "floor") {
        cell.placed_flag() = value;
    } else if (field == "item_set") {
        cell.item_set_index = value;
    } else {
        throw std::runtime_error("unknown SBB cell field: " + std::string{field});
    }
}

void set_sbc_building_field(map_editor::SbcBuilding& building, std::string_view field, std::string_view value) {
    if (field == "name") {
        building.name = std::string{value};
    } else if (field == "x") {
        building.x = parse_i32(value, "building x");
    } else if (field == "y") {
        building.y = parse_i32(value, "building y");
    } else if (field == "z") {
        building.z = parse_i32(value, "building z");
    } else if (field == "rotation") {
        building.rotation = parse_i32(value, "building rotation");
    } else {
        throw std::runtime_error("unknown SBC building field: " + std::string{field});
    }
}

int dump_sbb_cell_command(const std::filesystem::path& path, std::size_t index) {
    const auto building = map_editor::parse_sbb_file(path);
    const auto& cell = sbb_cell_at(building, index);
    std::cout << "SBB_CELL index=" << cell.index
              << " x=" << cell.x
              << " y=" << cell.y
              << " z=" << cell.z
              << " exterior_block=" << cell.exterior_block()
              << " interior_block=" << cell.interior_block()
              << " cell_type=" << cell.cell_type()
              << " exterior_portal_face=" << cell.exterior_portal_face()
              << " interior_portal_face=" << cell.interior_portal_face()
              << " placed_flag=" << cell.placed_flag()
              << " item_set=" << cell.item_set_index
              << '\n';
    return 0;
}

int dump_sbc_building_command(const std::filesystem::path& path, std::size_t index) {
    const auto map = map_editor::parse_sbc_file(path);
    const auto& building = sbc_building_at(map, index);
    std::cout << "SBC_BUILDING index=" << building.index
              << " name=" << building.name
              << " x=" << building.x
              << " y=" << building.y
              << " z=" << building.z
              << " rotation=" << building.rotation
              << '\n';
    return 0;
}

int dump_sbl_word_command(const std::filesystem::path& path, std::size_t word_index) {
    const auto bytes = map_editor::read_file_bytes(path);
    const auto offset = word_index * 4U;
    std::cout << "SBL_WORD index=" << word_index
              << " value=" << read_le_u32(bytes, offset)
              << '\n';
    return 0;
}

int dump_sbl_layer_command(const std::filesystem::path& path, std::size_t layer_index) {
    const auto template_file = map_editor::parse_sbl_file(path);
    if (layer_index >= template_file.layers.size()) {
        throw std::runtime_error("SBL layer index out of range");
    }
    const auto& layer = template_file.layers[layer_index];
    std::cout << "SBL_LAYER index=" << layer.index
              << " offset=" << layer.offset
              << " portal_type=" << layer.portal_type
              << " doors=" << layer.door_count
              << " windows=" << layer.window_count
              << " beams=" << layer.beam_count
              << '\n';
    return 0;
}

int set_sbb_cell_command(
    const std::filesystem::path& input_path,
    std::size_t index,
    std::string_view field,
    std::uint32_t value,
    const std::filesystem::path& output_path) {
    const auto bytes = map_editor::read_file_bytes(input_path);
    const auto building = map_editor::parse_sbb(bytes);
    auto cell = sbb_cell_at(building, index);
    set_sbb_cell_field(cell, field, value);
    write_file_bytes(output_path, map_editor::patch_sbb_cell(bytes, building, cell));
    std::cout << "Wrote " << output_path << '\n';
    return 0;
}

int set_sbc_building_command(
    const std::filesystem::path& input_path,
    std::size_t index,
    std::string_view field,
    std::string_view value,
    const std::filesystem::path& output_path) {
    const auto bytes = map_editor::read_file_bytes(input_path);
    const auto map = map_editor::parse_sbc(bytes);
    auto building = sbc_building_at(map, index);
    set_sbc_building_field(building, field, value);
    write_file_bytes(output_path, map_editor::patch_sbc_building(bytes, building, map.version));
    std::cout << "Wrote " << output_path << '\n';
    return 0;
}

int set_sbl_word_command(
    const std::filesystem::path& input_path,
    std::size_t word_index,
    std::uint32_t value,
    const std::filesystem::path& output_path) {
    const auto bytes = map_editor::read_file_bytes(input_path);
    write_file_bytes(output_path, map_editor::patch_sbl_word(bytes, word_index, value));
    std::cout << "Wrote " << output_path << '\n';
    return 0;
}

int set_sbl_layer_portal_command(
    const std::filesystem::path& input_path,
    std::size_t layer_index,
    std::uint32_t portal_type,
    const std::filesystem::path& output_path) {
    const auto bytes = map_editor::read_file_bytes(input_path);
    const auto template_file = map_editor::parse_sbl(bytes);
    write_file_bytes(output_path, map_editor::patch_sbl_layer_portal_type(bytes, template_file, layer_index, portal_type));
    std::cout << "Wrote " << output_path << '\n';
    return 0;
}

std::vector<map_editor::NamedBlockMesh> load_sibling_block_meshes(const std::filesystem::path& sbb_path) {
    const auto building_dir = sbb_path.parent_path();
    const auto map_dir = building_dir.filename() == "building" ? building_dir.parent_path() : building_dir;
    const auto block_dir = map_dir / "block";
    std::vector<map_editor::NamedBlockMesh> meshes;
    std::vector<std::filesystem::path> paths;

    if (std::filesystem::is_directory(block_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator{block_dir}) {
            if (entry.is_regular_file() && has_extension(entry.path(), ".sbl")) {
                paths.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(paths);

    for (const auto& path : paths) {
        const auto parsed = map_editor::parse_sbl_file(path);
        if (parsed.block.has_value()) {
            meshes.push_back({path.stem().string(), *parsed.block});
        }
    }

    return map_editor::build_native_block_mesh_catalog(meshes);
}

std::vector<map_editor::NamedBuildblockTemplate> load_sibling_buildblock_templates(
    const std::filesystem::path& sbb_path,
    const map_editor::SbbBuilding& building) {
    const auto building_dir = sbb_path.parent_path();
    const auto map_dir = building_dir.filename() == "building" ? building_dir.parent_path() : building_dir;
    const auto buildblock_dir = map_dir / "buildblock";
    std::vector<map_editor::NamedBuildblockTemplate> templates;

    for (const auto& name : building.block_names) {
        const auto path = buildblock_dir / (name + ".sbl");
        if (!std::filesystem::exists(path)) {
            continue;
        }
        templates.push_back({name, map_editor::parse_sbl_file(path)});
    }

    return templates;
}

int print_file_summary(const std::filesystem::path& path) {
    MAPED_PROFILE_FUNCTION();
    const auto is_sbc = has_extension(path, ".sbc");
    const auto is_sbb = has_extension(path, ".sbb");
    const auto is_sbl = has_extension(path, ".sbl");
    const auto is_csx = has_extension(path, ".csx");
    if (!is_sbc && !is_sbb && !is_sbl && !is_csx) {
        throw std::runtime_error("expected an .sbb, .sbc, .sbl, or .csx path");
    }

    if (is_sbb) {
        const auto building = map_editor::parse_sbb_file(path);
        const auto block_meshes = load_sibling_block_meshes(path);
        const auto buildblock_templates = load_sibling_buildblock_templates(path, building);
        const auto scene = map_editor::build_sbb_scene(building, block_meshes, buildblock_templates);
        std::cout << "SBB " << path
                  << " cells=" << scene.cells.size()
                  << " pieces=" << scene.pieces.size()
                  << " portal_edges=" << scene.portal_edges.size()
                  << '\n';
        return scene.cells.empty() ? 1 : 0;
    }

    if (is_sbc) {
        const auto map = map_editor::parse_sbc_file(path);
        const auto scene = map_editor::build_map_scene(map);
        std::cout << "SBC " << path
                  << " intersections=" << map.intersections.size()
                  << " roads=" << scene.roads.size()
                  << " buildings=" << scene.buildings.size()
                  << '\n';
        return scene.roads.empty() || scene.buildings.empty() ? 1 : 0;
    }

    if (is_sbl) {
        const auto template_file = map_editor::parse_sbl_file(path);
        std::cout << "SBL " << path
                  << " block=" << (template_file.block.has_value() ? "yes" : "no")
                  << " layers=" << template_file.layers.size()
                  << '\n';
        return !template_file.block.has_value() && template_file.layers.empty() ? 1 : 0;
    }

    const auto archive = map_editor::parse_csx_file(path);
    std::cout << "CSX " << path << " entries=" << archive.entries.size() << '\n';
    return archive.entries.empty() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    map_editor::profiling::Session profiling_session;
    MAPED_PROFILE_FUNCTION();
    try {
        if (argc == 1) {
            return map_editor::run_editor_app();
        }

        if (argc > 1 && std::string_view{argv[1]} == "--dump-sbb-cell") {
            if (argc != 4) {
                throw std::runtime_error("usage: map_editor_cpp --dump-sbb-cell <file.sbb> <cell-index>");
            }
            return dump_sbb_cell_command(argv[2], parse_index(argv[3], "cell index"));
        }
        if (argc > 1 && std::string_view{argv[1]} == "--dump-sbc-building") {
            if (argc != 4) {
                throw std::runtime_error("usage: map_editor_cpp --dump-sbc-building <file.sbc> <building-index>");
            }
            return dump_sbc_building_command(argv[2], parse_index(argv[3], "building index"));
        }
        if (argc > 1 && std::string_view{argv[1]} == "--dump-sbl-word") {
            if (argc != 4) {
                throw std::runtime_error("usage: map_editor_cpp --dump-sbl-word <file.sbl> <word-index>");
            }
            return dump_sbl_word_command(argv[2], parse_index(argv[3], "word index"));
        }
        if (argc > 1 && std::string_view{argv[1]} == "--dump-sbl-layer") {
            if (argc != 4) {
                throw std::runtime_error("usage: map_editor_cpp --dump-sbl-layer <file.sbl> <layer-index>");
            }
            return dump_sbl_layer_command(argv[2], parse_index(argv[3], "layer index"));
        }
        if (argc > 1 && std::string_view{argv[1]} == "--set-sbb-cell") {
            if (argc != 7) {
                throw std::runtime_error("usage: map_editor_cpp --set-sbb-cell <input.sbb> <cell-index> <field> <value> <output.sbb>");
            }
            return set_sbb_cell_command(argv[2], parse_index(argv[3], "cell index"), argv[4], parse_u32(argv[5], "field value"), argv[6]);
        }
        if (argc > 1 && std::string_view{argv[1]} == "--set-sbc-building") {
            if (argc != 7) {
                throw std::runtime_error("usage: map_editor_cpp --set-sbc-building <input.sbc> <building-index> <field> <value> <output.sbc>");
            }
            return set_sbc_building_command(argv[2], parse_index(argv[3], "building index"), argv[4], argv[5], argv[6]);
        }
        if (argc > 1 && std::string_view{argv[1]} == "--set-sbl-word") {
            if (argc != 6) {
                throw std::runtime_error("usage: map_editor_cpp --set-sbl-word <input.sbl> <word-index> <value> <output.sbl>");
            }
            return set_sbl_word_command(argv[2], parse_index(argv[3], "word index"), parse_u32(argv[4], "word value"), argv[5]);
        }
        if (argc > 1 && std::string_view{argv[1]} == "--set-sbl-layer-portal") {
            if (argc != 6) {
                throw std::runtime_error("usage: map_editor_cpp --set-sbl-layer-portal <input.sbl> <layer-index> <portal-type> <output.sbl>");
            }
            return set_sbl_layer_portal_command(argv[2], parse_index(argv[3], "layer index"), parse_u32(argv[4], "portal type"), argv[5]);
        }

        const auto check_mode = argc > 1 && std::string{argv[1]} == "--check";
        const auto path = check_mode
            ? (argc > 2 ? std::filesystem::path{argv[2]} : default_fixture_path(argv[0]))
            : (argc > 1 ? std::filesystem::path{argv[1]} : default_fixture_path(argv[0]));
        return print_file_summary(path);
    } catch (const std::exception& error) {
        std::cerr << "map_editor_cpp: " << error.what() << '\n';
        return 1;
    }
}
