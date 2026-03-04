cmake_minimum_required(VERSION 3.16)
project(dap_firmware C)

set(CMAKE_C_STANDARD 11)

# ── Find libraries ───────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2       REQUIRED sdl2)
pkg_check_modules(SDL2_TTF   REQUIRED SDL2_ttf)
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
pkg_check_modules(TAGLIB_C   REQUIRED taglib_c)

# ── Source groups ────────────────────────────────────────────────
set(THEME_SOURCES
    src/ui/themes/theme_dark.c
    src/ui/themes/theme_warm.c
    src/ui/themes/theme_ipod.c
    src/ui/themes/theme_registry.c
)

set(SCREEN_SOURCES
    src/ui/screens/screen_now_playing.c
    src/ui/screens/screen_library.c
)

set(AUDIO_SOURCES
    src/audio/metadata.c
    src/audio/audio_engine.c
    src/audio/file_scanner.c
)

# ── Simulator target ─────────────────────────────────────────────
add_executable(dap_sim
    sim_main.c
    ${THEME_SOURCES}
    ${SCREEN_SOURCES}
    ${AUDIO_SOURCES}
)

target_include_directories(dap_sim PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${SDL2_IMAGE_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${TAGLIB_C_INCLUDE_DIRS}
)

target_link_libraries(dap_sim
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${TAGLIB_C_LIBRARIES}
    Threads::Threads
    m
)

target_compile_options(dap_sim PRIVATE
    ${SDL2_CFLAGS_OTHER}
    -Wall -Wextra -O2
)

# ── Build output ─────────────────────────────────────────────────
set_target_properties(dap_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "SDL2:        ${SDL2_VERSION}")
message(STATUS "SDL2_ttf:    ${SDL2_TTF_VERSION}")
message(STATUS "SDL2_image:  ${SDL2_IMAGE_VERSION}")
message(STATUS "SDL2_mixer:  ${SDL2_MIXER_VERSION}")
message(STATUS "TagLib C:    ${TAGLIB_C_VERSION}")
