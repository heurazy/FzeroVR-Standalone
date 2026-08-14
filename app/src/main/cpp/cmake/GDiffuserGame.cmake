# Android build of the game/decomp half of G-Diffuser.
# This intentionally mirrors the upstream port/CMakeLists.txt gdiffuser_game target while keeping
# desktop UI/extractor/Discord pieces out of the Quest NativeActivity.

if(NOT FZERO_VR_FETCH_GDIFFUSER)
    message(FATAL_ERROR "GDiffuserGame.cmake requires FZERO_VR_FETCH_GDIFFUSER=ON")
endif()

set(GDX_PORT_DIR "${GDX_SOURCE_DIR}/port")
set(DECOMP "${GDX_DECOMP_DIR}")

file(GLOB_RECURSE GDX_GAME_SOURCES CONFIGURE_DEPENDS
    "${DECOMP}/src/game/*.c"
    "${DECOMP}/src/sys/*.c"
    "${DECOMP}/src/overlays/*.c"
    "${DECOMP}/src/audio/*.c"
    "${DECOMP}/src/framebuffers/*.c"
    "${DECOMP}/src/libultra/gu/*.c"
    "${DECOMP}/src/libultra/os/*.c"
)

# Host/Quest replacements, same exclusions as G-Diffuser's upstream port target.
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/gettime\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/settime\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/settimer\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/timerintr\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/createthread\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/initialize\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/getmemsize\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")
# Quest uses a configure-time sys_main copy that disables the 64DD/LEO boot path; the upstream
# host linker symbols for that overlay are stubs and cannot be used as Android memory ranges.
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/sys/sys_main\\.c$")
# Quest substitutes camera.c so the center-head VR pose is present before CPU-side culling and
# background/model setup. course.c stays upstream: once the camera itself is VR-correct, stock
# frustum culling is the right behavior and avoids the old 360-degree overdraw workaround.
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/game/camera\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/game/racer\\.c$")
# Quest uses configure-time race/background copies for VR section markers. race_quest separates
# sky/world/HUD sections; background_quest switches finite background geometry back to WORLD after
# the true sky/stars portion so venue floor/clouds/sprites receive room-scale translation.
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/race\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i3/background\\.c$")

# Start the standalone Quest port with the base US cartridge path. Expansion Kit can be layered
# back in once the core VR renderer is running; this removes the 64DD disk/Leo surface from the
# first Android integration pass without changing ordinary F-Zero X gameplay.
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/sys/disk/")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/dd_save\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/ovl_i2_data2\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i10/.*187510\\.c$")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/expansion_kit/")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/course_edit/")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/machine_create/")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
list(FILTER GDX_GAME_SOURCES EXCLUDE REGEX "/audio/disk/")

add_library(gdiffuser_game OBJECT
    ${GDX_GAME_SOURCES}
    "${GDX_QUEST_DECOMP_PORT}"
    "${GDX_PORT_DIR}/n64_sched.c"
    "${GDX_PORT_DIR}/n64_vi.c"
    "${DECOMP}/src/libultra/io/devmgr.c"
    "${GDX_QUEST_ASSET_BINDINGS}"
    "${GDX_PORT_DIR}/gen/LinkStubs.c"
    "${GDX_QUEST_RACE}"
    "${GDX_QUEST_BACKGROUND}"
    "${GDX_QUEST_CAMERA}"
    "${GDX_QUEST_RACER}"
    "${GDX_QUEST_SYS_MAIN}"
    "${CMAKE_CURRENT_SOURCE_DIR}/quest_audio_fontconv.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_libucontext.c"
)

set_target_properties(gdiffuser_game PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(gdiffuser_game PRIVATE quest_libucontext)

target_include_directories(gdiffuser_game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_android_compat"
    "${DECOMP}"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${DECOMP}/src/overlays/ovl_i3"
    "${GDX_SOURCE_DIR}/include"
    "${GDX_PORT_DIR}"
)

target_compile_definitions(gdiffuser_game PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1
    ASSET_VERSION=us
    ASSET_REVISION=rev0
    GDX_QUEST_NO_64DD=1
    GDX_QUEST_VR=1
)

# Preserve the decomp's host-port assumptions from upstream. These flags are important at -O2:
# the original C relies heavily on aliasing and wrapping arithmetic semantics.
target_compile_options(gdiffuser_game PRIVATE
    # Gradle's debug variant otherwise compiles the entire N64 simulation/decomp at -O0. Quest
    # needs debug symbols/installability, not unoptimized execution. Keep the decomp's alias/wrap
    # safety flags below and explicitly optimize this hot object library like the desktop port.
    -O2
    -include
    "${CMAKE_CURRENT_SOURCE_DIR}/gdx_android_compat/gdx_android_libc_compat.h"
    # Bionic's fortified bzero macro uses Clang overloads and turns the decomp's
    # legacy N64 integer-address call sites into hard errors. The Linux host build
    # uses the plain libc function, where those sites remain warnings; mirror that
    # behavior for these decomp objects only.
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
    -w
    -Werror=return-type
    -fcommon
    -fno-strict-aliasing
    -fwrapv
    -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types
    -Wno-error=implicit-function-declaration
)
