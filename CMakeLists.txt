cmake_minimum_required(VERSION 3.20)

# Titanium Legion: Steel Battalion (Xbox, 2002), statically recompiled with xboxrecomp. The toolkit sits next to this folder
# (../xboxrecomp), and the game's code is lifted into src/recomp/gen/ by
# lift.sh from your own copy. That folder is never committed.

project(titanium_legion C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit (v0.16.0 with Aggro Royale's patch applied)")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# CONFIGURE_DEPENDS so a re-lift that adds or removes a file is picked up
# without re-running cmake by hand.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

add_executable(${PROJECT_NAME} WIN32
    src/main.c
    src/recomp_manual.c          # hand-written replacements for game functions
    src/sb_input.c               # the Steel Battalion controller from a pad and keyboard
    src/game_check.c             # is this the supported default.xbe?
    src/game_import.c            # first-run setup from a disc image or folder
    src/xiso.c                   # Xbox disc image reader
    src/prof_sampler.c           # sampling profiler, off unless RECOMP_PROF=1
    ${RECOMP_GEN_SOURCES}
)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    xboxrecomp
    d3d11 dxgi dxguid xinput winmm
    bcrypt                       # SHA-1 for the game check
    dbghelp                      # crash reports name the guest function that faulted
)
if(MSVC)
    # The generated files are huge: /bigobj for their section count.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP8)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE /MAP)
endif()

# Window title prefix and settings file name (titaniumlegion.ini).
target_compile_definitions(xbox_video PRIVATE RECOMP_APP_NAME="TitaniumLegion")

option(TL_RELEASE "Build for a downloadable release" OFF)
if(TL_RELEASE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TL_RELEASE=1)
else()
    file(TO_NATIVE_PATH "${XBOXRECOMP_DIR}/game_files" TL_FALLBACK_GAME_DIR_NATIVE)
    string(REPLACE "\\" "\\\\" TL_FALLBACK_GAME_DIR_C "${TL_FALLBACK_GAME_DIR_NATIVE}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        TL_FALLBACK_GAME_DIR="${TL_FALLBACK_GAME_DIR_C}")
endif()

set(TL_VERSION "dev" CACHE STRING "Version shown on the launcher")
target_compile_definitions(${PROJECT_NAME} PRIVATE TL_VERSION="${TL_VERSION}")

# The project art, embedded (src/titanium_legion.rc.in).
set(TL_OVERLAY_LOGO "${CMAKE_CURRENT_SOURCE_DIR}/assets/titanium-legion-logo.png")
set(TL_TITLE_MARK "${CMAKE_CURRENT_SOURCE_DIR}/assets/asukaneo-mark.png")
configure_file(src/titanium_legion.rc.in ${CMAKE_CURRENT_BINARY_DIR}/titanium_legion.rc @ONLY)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/titanium_legion.rc)
