/* Quest-only feature surface for desktop G-Diffuser facilities that are intentionally absent.
 * Keep runtime-critical game/renderer code linked while disabling desktop Workshop tooling.
 * The real G-Diffuser dedicated audio thread and perf module are linked on Quest.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Base US build: no Expansion Kit disk is mounted on Quest yet.
 * LEODiskID is 32 bytes (8-byte aligned) in the decomp ABI.  Export storage with the exact C
 * symbol and layout footprint without including the N64 libc headers, whose 32-bit typedefs are
 * intentionally incompatible with Android arm64's LP64 host ABI. */
_Alignas(8) unsigned char leoBootID[32] = {0};

/* libultraship's Android Context path normally asks SDL for external storage. This native
 * OpenXR/NativeActivity port does not bootstrap SDL's Android activity state, so that call can
 * dereference a null SDL activity before the game starts. Route libultraship to SHIP_HOME instead;
 * gdiffuser_vr_host sets it to the app-private files directory before Context initialization. */
const char* gdx_quest_android_storage_path(void) {
    const char* shipHome = getenv("SHIP_HOME");
    return (shipHome != NULL && shipHome[0] != '\0') ? shipHome : ".";
}

/* Quest Rumble Pak backend. The decomp calls __osMotorAccess through osMotorStart/osMotorStop;
 * publish the binary motor state to the OpenXR input layer, which sustains haptics on both Touch
 * controllers. OSPfs is opaque here because only the motor state matters for the single player. */
extern void gdx_quest_set_rumble(int active);
int __osMotorAccess(void* pfs, unsigned int vibrate) {
    (void)pfs;
    gdx_quest_set_rumble(vibrate != 0U);
    return 0;
}

/* Host-side RSP microcode identity markers. Fast3D compares these addresses with the task's ucode
 * token; it does not execute MIPS/RSP text on ARM.  The desktop port's generated LinkStubs uses
 * the same one-element-symbol pattern for translated microcodes. */
unsigned long long gspF3DEX2_Rej_fifoTextStart[1] = {0};
unsigned long long gspL3DEX2_fifoTextStart[1] = {0};

/* Only the desktop autotest script calls this today. System quit on Quest is handled by the
 * NativeActivity/OpenXR lifecycle, so a scripted desktop quit request is intentionally ignored. */
void gdx_request_quit(void) {}

int gdx_workshop_texture_packs_enabled(void) {
    return 0;
}

const char* GdxWorkshopLookupOverridePath(const char* key) {
    (void)key;
    return NULL;
}

int gdx_workshop_texture_dump_enabled(void) {
    return 0;
}

void gdx_workshop_dump_texture(const void* origSrcAddr, size_t origSrcLen,
                               const char* resourcePathOrNull, const uint8_t* rgba32,
                               int width, int height, int n64Fmt, int n64Siz) {
    (void)origSrcAddr;
    (void)origSrcLen;
    (void)resourcePathOrNull;
    (void)rgba32;
    (void)width;
    (void)height;
    (void)n64Fmt;
    (void)n64Siz;
}
