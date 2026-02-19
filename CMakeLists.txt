cmake_minimum_required(VERSION 3.16)
project(dap-firmware C)

set(CMAKE_C_STANDARD 11)

# ─── Resolution config (change here later if needed) ───────────────────────
set(DAP_SCREEN_WIDTH  320 CACHE STRING "Screen width in pixels")
set(DAP_SCREEN_HEIGHT 240 CACHE STRING "Screen height in pixels")

# ─── Platform target ────────────────────────────────────────────────────────
set(DAP_TARGET "sdl2" CACHE STRING "Build target: sdl2 or stm32")

if(DAP_TARGET STREQUAL "sdl2")
    message(STATUS "Building for SDL2 simulator (${DAP_SCREEN_WIDTH}x${DAP_SCREEN_HEIGHT})")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
    pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

    add_executable(dap-sim
        src/main.c
        src/hal/sdl2/hal_sdl2.c
    )

    target_include_directories(dap-sim PRIVATE
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_TTF_INCLUDE_DIRS}
        ${SDL2_IMAGE_INCLUDE_DIRS}
        src/
    )

    target_link_libraries(dap-sim PRIVATE
        ${SDL2_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        ${SDL2_IMAGE_LIBRARIES}
    )

    target_compile_definitions(dap-sim PRIVATE
        DAP_PLATFORM_SDL2
        DAP_SCREEN_WIDTH=${DAP_SCREEN_WIDTH}
        DAP_SCREEN_HEIGHT=${DAP_SCREEN_HEIGHT}
    )

elseif(DAP_TARGET STREQUAL "stm32")
    message(STATUS "Building for STM32 hardware")
    # STM32 toolchain and sources will be added later
    message(FATAL_ERROR "STM32 target not yet implemented")
endif()