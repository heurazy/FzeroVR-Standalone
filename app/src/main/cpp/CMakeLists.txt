cmake_minimum_required(VERSION 3.22.1)
project(fzerox_vr LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenXR REQUIRED CONFIG)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/QuestLibucontext.cmake)

option(FZERO_VR_WITH_LIBULTRASHIP "Build the G-Diffuser pinned Fast3D/libultraship dependency" ON)
if(FZERO_VR_WITH_LIBULTRASHIP)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GDiffuserDependencies.cmake)
endif()

option(FZERO_VR_FETCH_GDIFFUSER "Populate pinned G-Diffuser/decomp sources for the Quest port" ON)
if(FZERO_VR_FETCH_GDIFFUSER)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GDiffuserSources.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GDiffuserAudio.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GDiffuserQuestPatches.cmake)
endif()

option(FZERO_VR_BUILD_GDIFFUSER_GAME "Compile the F-Zero X decomp game objects for Android" ON)
if(FZERO_VR_BUILD_GDIFFUSER_GAME)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GDiffuserGame.cmake)
endif()

add_library(android_native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(android_native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(fzerox_vr SHARED
    native_main.cpp
    openxr_context.cpp
    quest_input.cpp
    renderer_gles.cpp
    gdiffuser_bridge.cpp
    gdx_quest_input_provider.cpp
    quest_aaudio_player.cpp)

target_compile_definitions(fzerox_vr PRIVATE
    XR_USE_PLATFORM_ANDROID=1
    XR_USE_GRAPHICS_API_OPENGL_ES=1
    ENABLE_OPENGL=1
    USE_OPENGLES=1
    PORT=1
    GDIFFUSER_PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1
    ASSET_VERSION=us
    ASSET_REVISION=rev0)

target_include_directories(fzerox_vr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fzerox_vr PRIVATE
    android_native_app_glue
    OpenXR::openxr_loader
    quest_libucontext
    android
    log
    dl
    aaudio
    EGL
    GLESv3)

if(FZERO_VR_WITH_LIBULTRASHIP)
    target_link_libraries(fzerox_vr PRIVATE libultraship)
endif()

if(FZERO_VR_FETCH_GDIFFUSER)
    target_link_libraries(fzerox_vr PRIVATE cxd4_rsp)
endif()

if(FZERO_VR_BUILD_GDIFFUSER_GAME)
    # Link the decomp/game objects plus the platform-neutral host shims/asset bridge. This makes
    # the Android linker our integration checklist for the remaining rendering/audio host pieces.
    target_sources(fzerox_vr PRIVATE
        $<TARGET_OBJECTS:gdiffuser_game>
        "${GDX_SOURCE_DIR}/port/shims.c"
        "${GDX_QUEST_ASSET_LOADER}"
        "${GDX_SOURCE_DIR}/port/input_bridge.c"
        "${GDX_SOURCE_DIR}/port/gdx_input_script.c"
        "${GDX_SOURCE_DIR}/port/gdx_interp.cpp"
        "${GDX_SOURCE_DIR}/port/gdx_dev_gates.c"
        "${GDX_SOURCE_DIR}/port/enhancements/events/GameEvents.cpp"
        "${GDX_SOURCE_DIR}/port/sram_buffer.cpp"
        "${GDX_SOURCE_DIR}/port/gdx_ghost_io.c"
        "${GDX_SOURCE_DIR}/port/n64_audio_hle.c"
        "${GDX_SOURCE_DIR}/port/gdx_audio_lle.c"
        "${GDX_SOURCE_DIR}/port/gdx_audio_thread.cpp"
        "${GDX_SOURCE_DIR}/port/gdx_perf.cpp"
        "${GDX_SOURCE_DIR}/port/rom_buffer.cpp"
        "${GDX_SOURCE_DIR}/port/gdx_segment_source.c"
        "${GDX_SOURCE_DIR}/port/mio0_wrap.c"
        "${GDX_SOURCE_DIR}/port/n64_gfx_convert.cpp"
        "${GDX_SOURCE_DIR}/port/gdx_vi_convert.c"
        "${GDX_QUEST_GFX_BRIDGE}"
        "${GDX_QUEST_BUILTIN_RESOURCES}"
        "${GDX_TORCH_DIR}/lib/libmio0/mio0.c"
        "${GDX_SOURCE_DIR}/port/resource/ResourceFactories.cpp"
        quest_runtime_stubs.c
        quest_fast3d_window.cpp
        gdiffuser_vr_host.cpp)
    target_include_directories(fzerox_vr PRIVATE
        "${GDX_SOURCE_DIR}"
        "${GDX_SOURCE_DIR}/include"
        "${GDX_SOURCE_DIR}/port"
        "${GDX_DECOMP_DIR}"
        "${GDX_DECOMP_DIR}/include"
        "${GDX_DECOMP_DIR}/src"
        "${GDX_TORCH_DIR}/lib/libmio0")
endif()

target_compile_options(fzerox_vr PRIVATE
    # The Quest debug APK keeps symbols/logging, but the N64->Fast3D translator is far too
    # branch/container-heavy to run at Clang's default -O0. Keep it optimized during headset
    # iteration as well; -g still gives useful native crash symbols.
    -O3
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden)

# G-Diffuser is intentionally integrated as a second stage instead of being blindly
# add_subdirectory()'d here: upstream is currently desktop-oriented and pulls SDL/window code.
# scripts/bootstrap-gdiffuser.ps1 vendors the exact upstream tree under third_party/G-Diffuser.
# The runtime bridge in gdiffuser_bridge.* already defines the ABI used by the Android-specific
# G-Diffuser host once those desktop-only pieces are split from the game/decomp objects.
