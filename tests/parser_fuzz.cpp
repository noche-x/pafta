// SPDX-License-Identifier: GPL-2.0-only
#include "formats.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    if (*argc < 2 || (*argv)[1][0] == '-') return 0;
    std::vector<std::uint8_t> seed;
#if defined(PAFTA_FUZZ_sbb)
    pafta::SbbBuilding building{};
    building.version = 14;
    building.size_z = 1;
    building.cells.resize(1);
    seed = pafta::rebuild_sbb(building);
#elif defined(PAFTA_FUZZ_sbc)
    pafta::SbcMap map{};
    map.version = 12;
    seed = pafta::rebuild_sbc(map);
#elif defined(PAFTA_FUZZ_sbl)
    pafta::SblTemplate block{};
    block.version = 1;
    block.layers.resize(1);
    seed = pafta::rebuild_sbl_buildblock(block);
#elif defined(PAFTA_FUZZ_csx)
    seed = pafta::build_csx({0xc5f17e01U, 0, 0, {}});
#endif
    const auto path = std::filesystem::path{(*argv)[1]} / "valid-seed";
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
    output.close();
    if (!output) throw std::runtime_error("Could not write fuzz seed: " + path.string());
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);
    try {
#if defined(PAFTA_FUZZ_sbb)
        (void)pafta::parse_sbb(bytes);
#elif defined(PAFTA_FUZZ_sbc)
        (void)pafta::parse_sbc(bytes);
#elif defined(PAFTA_FUZZ_sbl)
        (void)pafta::parse_sbl(bytes);
#elif defined(PAFTA_FUZZ_csx)
        (void)pafta::parse_csx(bytes);
#endif
    } catch (const std::runtime_error&) {
        // A malformed external file is expected to fail. Other exceptions are bugs.
    }
    return 0;
}
