#pragma once

#if defined(MAP_EDITOR_CPP_ENABLE_PROFILER)
#include <easy/profiler.h>

#include <cstdlib>
#include <iostream>
#endif

namespace map_editor::profiling {

inline void start() {
#if defined(MAP_EDITOR_CPP_ENABLE_PROFILER)
    profiler::registerThread("Main");
    EASY_PROFILER_ENABLE;
    profiler::startListen();
#endif
}

inline void stop() {
#if defined(MAP_EDITOR_CPP_ENABLE_PROFILER)
    if (const char* output_path = std::getenv("MAPED_PROFILE_OUT");
        output_path != nullptr && output_path[0] != '\0') {
        const auto blocks = profiler::dumpBlocksToFile(output_path);
        std::cerr << "map_editor_cpp: wrote " << blocks << " profiler blocks to "
                  << output_path << '\n';
    }
    profiler::stopListen();
#endif
}

struct Session {
    Session() {
        start();
    }

    ~Session() {
        stop();
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

} // namespace map_editor::profiling

#if defined(MAP_EDITOR_CPP_ENABLE_PROFILER)
#define MAPED_PROFILE_FUNCTION(...) EASY_FUNCTION(__VA_ARGS__)
#define MAPED_PROFILE_BLOCK(...) EASY_BLOCK(__VA_ARGS__)
#define MAPED_PROFILE_END_BLOCK EASY_END_BLOCK
#define MAPED_PROFILE_THREAD(name) EASY_THREAD_SCOPE(name)
#else
#define MAPED_PROFILE_FUNCTION(...) ((void)0)
#define MAPED_PROFILE_BLOCK(...) ((void)0)
#define MAPED_PROFILE_END_BLOCK ((void)0)
#define MAPED_PROFILE_THREAD(name) ((void)0)
#endif
