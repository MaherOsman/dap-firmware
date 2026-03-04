cmake_minimum_required(VERSION 3.16)
project(dap_firmware C)

set(CMAKE_C_STANDARD 11)

# ── Find SDL2 + SDL2_ttf ────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2     REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

# ── Source groups ───────────────────────────────────────────────
set(THEME_SOURCES
    src/ui/themes/theme_dark.c
    src/ui/themes/theme_warm.c
    src/ui/themes/theme_ipod.c
    src/ui/themes/theme_registry.c
)

set(SCREEN_SOURCES
    src/ui/screens/screen_now_playing.c
)

# ── Simulator target ────────────────────────────────────────────
add_executable(dap_sim
    sim_main.c
    ${THEME_SOURCES}
    ${SCREEN_SOURCES}
)

target_include_directories(dap_sim PRIVATE
    ${CMAKE_SOURCE_DIR}          # so #include "src/ui/..." resolves
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
)

target_link_libraries(dap_sim
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    m          # math (sqrtf, cosf, sinf)
)

target_compile_options(dap_sim PRIVATE
    ${SDL2_CFLAGS_OTHER}
    -Wall -Wextra -O2
)

# ── Build output ────────────────────────────────────────────────
set_target_properties(dap_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "SDL2: ${SDL2_VERSION}")
message(STATUS "SDL2_ttf: ${SDL2_TTF_VERSION}")
