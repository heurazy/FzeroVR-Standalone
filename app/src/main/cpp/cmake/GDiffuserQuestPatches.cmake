# Generate tiny Quest-specific adaptations from the exact pinned upstream sources.  Keeping these
# as configure-time transforms means the upstream checkout remains pristine and every changed seam
# is explicit/reviewable here.

set(GDX_QUEST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/gdx_quest_generated")
file(MAKE_DIRECTORY "${GDX_QUEST_GENERATED_DIR}")

# 1) Fast3D display-list seam: when VR supplies two acquired OpenXR eye targets, render the same
# converted display list twice through the Quest host instead of the normal single desktop pass.
file(READ "${GDX_SOURCE_DIR}/port/n64_gfx_bridge.cpp" _gdx_gfx_bridge)
# Android's /proc/self/exe is app_process64, not this NativeActivity shared object. The upstream
# POSIX module-range helper therefore bounds the wrong ELF and every low32 pointer into the actual
# F-Zero image looks "outside the module". dl_iterate_phdr lets us identify the ELF object that
# physically contains GetMainModuleRange and compute its complete PT_LOAD extent, independent of
# APK!/lib path spelling.
string(REPLACE
    "#include <dlfcn.h>\n#include <unistd.h>"
    "#include <dlfcn.h>\n#include <link.h>\n#include <unistd.h>"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")
set(_quest_module_range_marker [=[
#else
    // Mirror "module base + SizeOfImage": take the contiguous run of /proc/self/maps entries whose
]=])
set(_quest_module_range_android [=[
#else
#if defined(__ANDROID__)
    struct QuestModuleRangeContext {
        uintptr_t target;
        uintptr_t begin;
        uintptr_t end;
        bool found;
    } ctx = { reinterpret_cast<uintptr_t>(&GetMainModuleRange), 0, 0, false };

    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* opaque) -> int {
            auto* c = static_cast<QuestModuleRangeContext*>(opaque);
            uintptr_t lo = UINTPTR_MAX;
            uintptr_t hi = 0;
            bool containsTarget = false;
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
                const uintptr_t begin = static_cast<uintptr_t>(info->dlpi_addr) +
                                        static_cast<uintptr_t>(ph.p_vaddr);
                const uintptr_t end = begin + static_cast<uintptr_t>(ph.p_memsz);
                lo = std::min(lo, begin);
                hi = std::max(hi, end);
                if (c->target >= begin && c->target < end) containsTarget = true;
            }
            if (containsTarget && lo != UINTPTR_MAX && hi > lo) {
                c->begin = lo;
                c->end = hi;
                c->found = true;
                return 1;
            }
            return 0;
        },
        &ctx);

    if (ctx.found) {
        moduleBegin = ctx.begin;
        moduleEnd = ctx.end;
        return;
    }
#endif
    // Mirror "module base + SizeOfImage": take the contiguous run of /proc/self/maps entries whose
]=])
string(REPLACE "${_quest_module_range_marker}" "${_quest_module_range_android}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# Android arm64 heap pointers can carry a top-byte allocation tag (for example 0xB4..............).
# /proc/self/maps always reports the canonical untagged virtual address, so the upstream POSIX
# readability probes reject perfectly valid heap-backed asset segments. Keep tagged pointers for
# dereference/TBI, but canonicalize only the address used for maps lookup and range arithmetic.
set(_quest_posix_region_old [=[
static bool PosixRegionFor(uintptr_t addr, MapsRegion& out) {
    if (sMaps.empty()) {
        ParseProcMaps();
    }
    const MapsRegion* r = FindMapsRegion(addr);
    if (r == nullptr) {
        ParseProcMaps(); // one re-parse then re-query
        r = FindMapsRegion(addr);
    }
]=])
set(_quest_posix_region_new [=[
static inline uintptr_t PosixMapsCanonicalAddress(uintptr_t addr) {
#if defined(__ANDROID__) && defined(__aarch64__)
    return addr & static_cast<uintptr_t>(0x00FFFFFFFFFFFFFFULL);
#else
    return addr;
#endif
}

static bool PosixRegionFor(uintptr_t addr, MapsRegion& out) {
    const uintptr_t mapsAddr = PosixMapsCanonicalAddress(addr);
    if (sMaps.empty()) {
        ParseProcMaps();
    }
    const MapsRegion* r = FindMapsRegion(mapsAddr);
    if (r == nullptr) {
        ParseProcMaps(); // one re-parse then re-query
        r = FindMapsRegion(mapsAddr);
    }
]=])
string(REPLACE "${_quest_posix_region_old}" "${_quest_posix_region_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

set(_quest_readable_command_old [=[
#else
    const uintptr_t addr = reinterpret_cast<uintptr_t>(source);
    MapsRegion r;
    if (!PosixRegionFor(addr, r) || !r.readable) {
        return 0;
    }
    if (addr >= r.end) {
        return 0;
    }
    return static_cast<size_t>((r.end - addr) / stride);
#endif
}
]=])
set(_quest_readable_command_new [=[
#else
    const uintptr_t addr = PosixMapsCanonicalAddress(reinterpret_cast<uintptr_t>(source));
    MapsRegion r;
    if (!PosixRegionFor(addr, r) || !r.readable) {
        return 0;
    }
    if (addr >= r.end) {
        return 0;
    }
    return static_cast<size_t>((r.end - addr) / stride);
#endif
}
]=])
string(REPLACE "${_quest_readable_command_old}" "${_quest_readable_command_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

set(_quest_readable_bytes_old [=[
#else
    MapsRegion r;
    if (!PosixRegionFor(address, r) || !r.readable) {
        return 0;
    }
    if (address >= r.end) {
        return 0;
    }
    return static_cast<size_t>(r.end - address);
#endif
}
]=])
set(_quest_readable_bytes_new [=[
#else
    const uintptr_t mapsAddress = PosixMapsCanonicalAddress(address);
    MapsRegion r;
    if (!PosixRegionFor(mapsAddress, r) || !r.readable) {
        return 0;
    }
    if (mapsAddress >= r.end) {
        return 0;
    }
    return static_cast<size_t>(r.end - mapsAddress);
#endif
}
]=])
string(REPLACE "${_quest_readable_bytes_old}" "${_quest_readable_bytes_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# Upstream's setup-gfx fallback table predates three generated symbols that are present in the
# current AssetBindings. Declare them in the Quest-generated bridge so the complete, bounded table
# below can resolve both exact and interior G_DL entry points.
string(REPLACE
    "extern \"C\" uint8_t D_30006D0[];"
    "extern \"C\" uint8_t D_30006D0[];\nextern \"C\" uint8_t D_30002D0[];\nextern \"C\" uint8_t D_3000608[];\nextern \"C\" uint8_t D_30006F8[];"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")
string(REPLACE
    "#include \"n64_gfx_bridge.h\""
    "#include \"n64_gfx_bridge.h\"\nextern \"C\" int gdx_vr_host_render_converted(void* interpreter, void* converted, int taskVariant);\nextern \"C\" void gdx_vr_host_mirror_texture_delete(const void* addr);"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")
set(_quest_single_run "interp->Run(reinterpret_cast<Gfx*>(converted), {}); // pass 0 (real render, t=1)")
set(_quest_stereo_run "if (!gdx_vr_host_render_converted(interp.get(), reinterpret_cast<void*>(converted), static_cast<int>(gdxTaskVariant))) {\n        interp->Run(reinterpret_cast<Gfx*>(converted), {}); // desktop/fallback single pass\n    }")
string(REPLACE "${_quest_single_run}" "${_quest_stereo_run}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# The Quest host owns a second persistent Fast3D interpreter for the right eye. Upstream cache
# invalidations target only the process-global/primary interpreter, so animated/address-reused
# textures can remain stale in the right-eye cache. Mirror every explicit bridge-side deletion.
string(REPLACE
    "interp->TextureCacheDelete(sConverted);"
    "interp->TextureCacheDelete(sConverted);\n    gdx_vr_host_mirror_texture_delete(sConverted);"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")
string(REPLACE
    "interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(addr));"
    "interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(addr));\n    gdx_vr_host_mirror_texture_delete(addr);"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")
string(REPLACE
    "interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(ptr));"
    "interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(ptr));\n            gdx_vr_host_mirror_texture_delete(reinterpret_cast<const void*>(ptr));"
    _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# The generic generated-asset lookup should normally cover these symbols, but the dedicated
# setup-gfx fallback was exact-base-only. Real F-Zero display lists branch to interior commands
# (observed on Quest: D_3000100 + 0x28), so exact-only fallback turns the entire 3D sub-list into a
# no-op and leaves only large RDP rectangles. Make this fallback complete and strictly bounded by
# each generated symbol's declared byte size from AssetBindings.c.
set(_quest_setup_resolver_old [=[
bool ResolveSetupGfxStub(uint32_t raw, ResolvedAddress& out) {
    struct SetupSymbol {
        const uint8_t* symbol;
        uint32_t offset;
    };

    static const SetupSymbol kSetupSymbols[] = {
        { D_3000000, 0x000 }, { D_3000028, 0x028 }, { D_3000050, 0x050 }, { D_3000088, 0x088 },
        { D_30000C0, 0x0C0 }, { D_3000100, 0x100 }, { D_3000138, 0x138 }, { D_3000170, 0x170 },
        { D_30001A8, 0x1A8 }, { D_3000270, 0x270 }, { D_30002E0, 0x2E0 }, { D_3000338, 0x338 },
        { D_3000400, 0x400 }, { D_3000438, 0x438 }, { D_3000470, 0x470 }, { D_30004A8, 0x4A8 },
        { D_30004E0, 0x4E0 }, { D_3000510, 0x510 }, { D_3000540, 0x540 }, { D_3000590, 0x590 },
        { D_30005D8, 0x5D8 }, { D_3000688, 0x688 }, { D_30006D0, 0x6D0 },
    };

    for (const SetupSymbol& entry : kSetupSymbols) {
        if (raw == Low32(reinterpret_cast<uintptr_t>(entry.symbol))) {
            const uintptr_t base = EnsureSetupGfxSegment();
            if (base == 0) {
                return false;
            }
            out.full = base + entry.offset;
            out.segment = 3;
            out.offset = entry.offset;
            out.segmented = true;
            return true;
        }
    }
    return false;
}
]=])
set(_quest_setup_resolver_new [=[
bool ResolveSetupGfxStub(uint32_t raw, ResolvedAddress& out) {
    struct SetupSymbol {
        const uint8_t* symbol;
        uint32_t offset;
        uint32_t size;
    };

    static const SetupSymbol kSetupSymbols[] = {
        { D_3000000, 0x000, 0x28 }, { D_3000028, 0x028, 0x28 }, { D_3000050, 0x050, 0x38 },
        { D_3000088, 0x088, 0x38 }, { D_30000C0, 0x0C0, 0x40 }, { D_3000100, 0x100, 0x38 },
        { D_3000138, 0x138, 0x38 }, { D_3000170, 0x170, 0x38 }, { D_30001A8, 0x1A8, 0xC8 },
        { D_3000270, 0x270, 0x60 }, { D_30002D0, 0x2D0, 0x10 }, { D_30002E0, 0x2E0, 0x58 },
        { D_3000338, 0x338, 0xC8 }, { D_3000400, 0x400, 0x38 }, { D_3000438, 0x438, 0x38 },
        { D_3000470, 0x470, 0x38 }, { D_30004A8, 0x4A8, 0x38 }, { D_30004E0, 0x4E0, 0x30 },
        { D_3000510, 0x510, 0x30 }, { D_3000540, 0x540, 0x50 }, { D_3000590, 0x590, 0x48 },
        { D_30005D8, 0x5D8, 0x30 }, { D_3000608, 0x608, 0x80 }, { D_3000688, 0x688, 0x48 },
        { D_30006D0, 0x6D0, 0x28 }, { D_30006F8, 0x6F8, 0x80 },
    };

    for (const SetupSymbol& entry : kSetupSymbols) {
        const uint32_t symbolLow = Low32(reinterpret_cast<uintptr_t>(entry.symbol));
        const uint32_t delta = raw - symbolLow;
        if (delta < entry.size) {
            const uintptr_t base = EnsureSetupGfxSegment();
            if (base == 0) {
                return false;
            }
            out.full = base + entry.offset + delta;
            out.segment = 3;
            out.offset = entry.offset + delta;
            out.segmented = true;
            return true;
        }
    }
    return false;
}
]=])
string(REPLACE "${_quest_setup_resolver_old}" "${_quest_setup_resolver_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# Deep G_DL fix for PORT-wide packets. A non-zero high32 does not guarantee that w1 is a real data
# pointer: generated asset symbols are linker stubs whose *full* host address is written into the
# 16-byte Gfx packet. Resolve exact generated/setup/BSS identities before trusting hostFull. This
# keeps real host pointers on the fast path while preventing Fast3D from branching into zero-filled
# LinkStubs instead of the decoded O2R/ROM segment image.
set(_quest_gdl_hostptr_old [=[
        if (hostPtr) {
            return TranslateDisplayListPointer(raw, parentSource, parentIndex,
                                                reinterpret_cast<const N64Gfx*>(hostFull));
        }
]=])
set(_quest_gdl_hostptr_new [=[
        if (hostPtr) {
            ResolvedAddress exact = {};
            if (ResolveGeneratedAssetStub(raw, exact) ||
                ResolveSetupGfxStub(raw, exact) ||
                ResolvePortBssAlias(raw, exact) ||
                ResolveVenueBankAlias(raw, exact)) {
                return TranslateDisplayListPointer(raw, parentSource, parentIndex,
                                                    reinterpret_cast<const N64Gfx*>(exact.full));
            }
            return TranslateDisplayListPointer(raw, parentSource, parentIndex,
                                                reinterpret_cast<const N64Gfx*>(hostFull));
        }
]=])
string(REPLACE "${_quest_gdl_hostptr_old}" "${_quest_gdl_hostptr_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

# Keep one high-value failure record for Quest pointer debugging. Unlike the upstream capped generic
# line this records the triggering GBI opcode, payload size and referencing list. It also samples
# the readable same-window candidate, allowing us to classify the token without enabling the broad
# legacy resolver.
set(_quest_resolve_fail_old [=[
                gdx_port_logf("[resolve-fail] raw=%08X "
                              "mModule=[%016llX,%016llX) "
                              "rootCand=%016llX rootReadable=%d "
                              "modCand=%016llX modInRange=%d modReadable=%d\n",
                              raw,
                              static_cast<unsigned long long>(mModuleBegin),
                              static_cast<unsigned long long>(mModuleEnd),
                              static_cast<unsigned long long>(rootCand),
                              IsReadableAddress(rootCand) ? 1 : 0,
                              static_cast<unsigned long long>(modCand),
                              (modCand >= mModuleBegin && modCand < mModuleEnd) ? 1 : 0,
                              IsReadableAddress(modCand) ? 1 : 0);
]=])
set(_quest_resolve_fail_new [=[
                uint32_t questProbe0 = 0, questProbe1 = 0;
                if (ReadableByteLimit(rootCand) >= 8) {
                    std::memcpy(&questProbe0, reinterpret_cast<const void*>(rootCand), 4);
                    std::memcpy(&questProbe1, reinterpret_cast<const void*>(rootCand + 4), 4);
                }
                gdx_port_logf("[resolve-fail] raw=%08X op=%02X req=%zu pref=%d src=%016llX root=%016llX "
                              "mModule=[%016llX,%016llX) rootCand=%016llX rootReadable=%zu "
                              "probe=%08X/%08X modCand=%016llX modInRange=%d modReadable=%zu\n",
                              raw, static_cast<unsigned>(gLegacyResolveCurrentOp), requiredBytes,
                              preferPhysical ? 1 : 0,
                              static_cast<unsigned long long>(sourceHint),
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mRootBegin)),
                              static_cast<unsigned long long>(mModuleBegin),
                              static_cast<unsigned long long>(mModuleEnd),
                              static_cast<unsigned long long>(rootCand),
                              ReadableByteLimit(rootCand), questProbe0, questProbe1,
                              static_cast<unsigned long long>(modCand),
                              (modCand >= mModuleBegin && modCand < mModuleEnd) ? 1 : 0,
                              ReadableByteLimit(modCand));
]=])
string(REPLACE "${_quest_resolve_fail_old}" "${_quest_resolve_fail_new}" _gdx_gfx_bridge "${_gdx_gfx_bridge}")

file(WRITE "${GDX_QUEST_GENERATED_DIR}/n64_gfx_bridge_quest.cpp" "${_gdx_gfx_bridge}")

# 2) Asset binding reverse lookup: the generated table already records {symbol, ROM offset, o2r key}.
# Expose key->ROM offset so Quest can fill common compressed arrays directly from the user's US ROM
# when no extracted o2r archive is installed.
file(READ "${GDX_SOURCE_DIR}/port/gen/AssetBindings.c" _gdx_asset_bindings)
set(_asset_lookup_marker "unsigned int gdx_lookup_common_asset_rom_offset(unsigned long long sym_addr) {")
set(_asset_reverse_lookup [=[
static int gdx_quest_key_equal(const char* a, const char* b) {
    if (a == NULL || b == NULL) return 0;
    while (*a != 0 && *b != 0) {
        if (*a++ != *b++) return 0;
    }
    return (*a == 0 && *b == 0) ? 1 : 0;
}

unsigned int gdx_lookup_common_asset_rom_offset_by_key(const char* key) {
    int i;
    for (i = 0; sCommonAssetRomMap[i].sym != NULL; i++) {
        if (gdx_quest_key_equal(sCommonAssetRomMap[i].o2r_key, key))
            return sCommonAssetRomMap[i].rom_offset;
    }
    return 0U;
}

unsigned int gdx_lookup_common_asset_rom_offset(unsigned long long sym_addr) {
]=])
string(REPLACE "${_asset_lookup_marker}" "${_asset_reverse_lookup}" _gdx_asset_bindings "${_gdx_asset_bindings}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/AssetBindingsQuest.c" "${_gdx_asset_bindings}")

# 3) Resource loader fallback. Normal o2r loading stays first. If an archive/resource is absent,
# copy the raw compressed bytes from the already validated 16 MiB US ROM using the generated table.
file(READ "${GDX_SOURCE_DIR}/port/AssetLoader.cpp" _gdx_asset_loader)
string(REPLACE
    "#include <vector>"
    "#include <vector>\nextern \"C\" unsigned int gdx_lookup_common_asset_rom_offset_by_key(const char* key);\nextern \"C\" unsigned char* gdx_rom_buffer;\nextern \"C\" size_t gdx_rom_size;"
    _gdx_asset_loader "${_gdx_asset_loader}")
# Replace only GDiffuser_LoadAssetBytes with a Quest-first implementation by renaming upstream and
# adding our public wrapper. The upstream implementation remains available for archive attempts.
string(REPLACE
    "extern \"C\" int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {"
    "static int GDiffuser_LoadAssetBytes_Archive(const char* key, void* out, size_t outSize, size_t* copiedSize) {"
    _gdx_asset_loader "${_gdx_asset_loader}")
set(_before_archive_file "extern \"C\" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {")
set(_quest_asset_wrapper [=[
extern "C" int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    if (GDiffuser_LoadAssetBytes_Archive(key, out, outSize, copiedSize)) {
        return 1;
    }
    if (key == nullptr || out == nullptr || outSize == 0 || gdx_rom_buffer == nullptr) {
        return 0;
    }
    const unsigned int offset = gdx_lookup_common_asset_rom_offset_by_key(key);
    if (offset == 0U || static_cast<size_t>(offset) + outSize > gdx_rom_size) {
        return 0;
    }
    std::memcpy(out, gdx_rom_buffer + offset, outSize);
    if (copiedSize != nullptr) *copiedSize = outSize;
    return 1;
}

extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
]=])
string(REPLACE "${_before_archive_file}" "${_quest_asset_wrapper}" _gdx_asset_loader "${_gdx_asset_loader}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/AssetLoaderQuest.cpp" "${_gdx_asset_loader}")

# 4) Race render-section markers. Fast3D must keep one uninterrupted Run() per eye, but the N64
# background is an enormous sky/venue shell and must NOT receive room-scale translation or IPD.
# Tag each Background_Draw as rotation-only sky, switch back to full stereo immediately after it,
# then tag the final Menus_Draw boundary for the separate transparent HUD quad.
file(READ "${GDX_DECOMP_DIR}/src/overlays/ovl_i2/race.c" _gdx_race)
foreach(_bg_call
    "            gfx = Background_Draw(gfx, 0, SCISSOR_BOX_FULL_SCREEN);"
    "            gfx = Background_Draw(gfx, 0, SCISSOR_BOX_TOP_HALF);"
    "            gfx = Background_Draw(gfx, 1, SCISSOR_BOX_BOTTOM_HALF);"
    "            gfx = Background_Draw(gfx, 0, SCISSOR_BOX_TOP_LEFT_QUARTER);"
    "            gfx = Background_Draw(gfx, 1, SCISSOR_BOX_BOTTOM_LEFT_QUARTER);"
    "            gfx = Background_Draw(gfx, 2, SCISSOR_BOX_TOP_RIGHT_QUARTER);"
    "            gfx = Background_Draw(gfx, 3, SCISSOR_BOX_BOTTOM_RIGHT_QUARTER);")
    string(REPLACE "${_bg_call}"
        "            gDPNoOpTag(gfx++, 0x5652534B); /* 'VRSK' rotation-only background */\n${_bg_call}\n            gDPNoOpTag(gfx++, 0x56523344); /* 'VR3D' full stereo world */"
        _gdx_race "${_gdx_race}")
endforeach()
set(_quest_race_hud_boundary "    return Menus_Draw(gfx);")
set(_quest_race_hud_marker [=[
#ifdef PORT
    gDPNoOpTag(gfx++, 0x56524844); /* 'VRHD' -- Quest VR HUD boundary */
#endif
    return Menus_Draw(gfx);
]=])
string(REPLACE "${_quest_race_hud_boundary}" "${_quest_race_hud_marker}" _gdx_race "${_gdx_race}")
# Restore the ordinary chase camera immediately before the flat HUD is built. The world display
# list has already been authored from the center-head VR camera at this point.
string(REPLACE
    "    gDPNoOpTag(gfx++, 0x56524844); /* 'VRHD' -- Quest VR HUD boundary */"
    "    extern void gdx_quest_vr_camera_restore(void);\n    gdx_quest_vr_camera_restore();\n    gDPNoOpTag(gfx++, 0x56524844); /* 'VRHD' -- Quest VR HUD boundary */"
    _gdx_race "${_gdx_race}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/race_quest.c" "${_gdx_race}")

# 4b) Background VR split. Background_Draw contains both true infinite-distance sky content and
# finite scene content. Race_Draw enters VRSK before the call; switch back to VR3D immediately
# after sky + stars so venue floor, clouds and background sprites participate in room-scale 6DoF.
file(READ "${GDX_DECOMP_DIR}/src/overlays/ovl_i3/background.c" _gdx_background)

# Quest 360 sky + celestial stars. Stock F-Zero builds one camera-facing sky quad and projects
# stars to 2D screen rectangles on the CPU. That is correct for a TV camera but leaves black space
# when the HMD looks behind the craft and makes the stars follow the car camera. In VR, add a
# 16-sided inward sky cylinder using the game's native 64x1 vertical-gradient texture, and render
# stars as textured 3D billboards on a camera-centered celestial sphere. Their directions are fixed
# in world/celestial space while the rotation-only Fast3D SKY matrix handles HMD look direction.
set(_quest_background_helper_marker [=[
extern GfxPool D_1000000;
extern Mtx D_2000000[];

Gfx* Background_Draw(Gfx* gfx, s32 cameraIndex, s32 scissorBoxType) {
]=])
set(_quest_background_vr_helpers [=[
extern GfxPool D_1000000;
extern Mtx D_2000000[];

#ifdef GDX_QUEST_VR
#define GDX_VR_SKY_SEGMENTS 16
#define GDX_VR_MAX_STARS 100
static Vtx sGdxVrSkyCylinderVtx[GDX_VR_SKY_SEGMENTS * 4];
static Vtx sGdxVrFogCylinderVtx[GDX_VR_SKY_SEGMENTS * 4];
static Vtx sGdxVrStarVtx[GDX_VR_MAX_STARS][4];

static s32 GdxVr_ClampVtx(f32 value) {
    if (value < -32000.0f) return -32000;
    if (value > 32000.0f) return 32000;
    return (s32) value;
}

static Gfx* GdxVr_DrawSkyCylinder(Gfx* gfx, Camera* camera, Background* background) {
    s32 i;
    Vtx* vtx = sGdxVrSkyCylinderVtx;
    f32 radius = background->skyboxDepth * 1.25f;
    f32 halfHeight = radius * 3.0f;
    s32 topS = 0;
    s32 bottomS = 63 * 32;

    for (i = 0; i < GDX_VR_SKY_SEGMENTS; i++) {
        s32 angle0 = (i * 0x1000) / GDX_VR_SKY_SEGMENTS;
        s32 angle1 = ((i + 1) * 0x1000) / GDX_VR_SKY_SEGMENTS;
        f32 x0 = camera->eye.x + COS(angle0) * radius;
        f32 z0 = camera->eye.z + SIN(angle0) * radius;
        f32 x1 = camera->eye.x + COS(angle1) * radius;
        f32 z1 = camera->eye.z + SIN(angle1) * radius;
        f32 topY = camera->eye.y + halfHeight;
        f32 bottomY = camera->eye.y - halfHeight;

        SET_VTX(vtx + 0, GdxVr_ClampVtx(x0), GdxVr_ClampVtx(topY), GdxVr_ClampVtx(z0), topS, 0, 255, 255, 255, 255);
        SET_VTX(vtx + 1, GdxVr_ClampVtx(x1), GdxVr_ClampVtx(topY), GdxVr_ClampVtx(z1), topS, 0, 255, 255, 255, 255);
        SET_VTX(vtx + 2, GdxVr_ClampVtx(x0), GdxVr_ClampVtx(bottomY), GdxVr_ClampVtx(z0), bottomS, 0, 255, 255, 255, 255);
        SET_VTX(vtx + 3, GdxVr_ClampVtx(x1), GdxVr_ClampVtx(bottomY), GdxVr_ClampVtx(z1), bottomS, 0, 255, 255, 255, 255);
        vtx += 4;
    }

    gSPClearGeometryMode(gfx++, G_CULL_BACK | G_CULL_FRONT);
    for (i = 0; i < GDX_VR_SKY_SEGMENTS; i += 8) {
        s32 j;
        gSPVertex(gfx++, &sGdxVrSkyCylinderVtx[i * 4], 32, 0);
        for (j = 0; j < 8; j++) {
            s32 b = j * 4;
            gSP2Triangles(gfx++, b + 0, b + 1, b + 2, 0, b + 2, b + 1, b + 3, 0);
        }
    }
    return gfx;
}

static Gfx* GdxVr_DrawFogCylinder(Gfx* gfx, Camera* camera, Background* background) {
    s32 i;
    Vtx* vtx = sGdxVrFogCylinderVtx;
    CourseSkyboxes* skybox = sBackgroundCtx.skybox;
    f32 radius = background->skyboxDepth * 1.05f;
    f32 halfHeight = radius * 0.72f;

    for (i = 0; i < GDX_VR_SKY_SEGMENTS; i++) {
        s32 angle0 = (i * 0x1000) / GDX_VR_SKY_SEGMENTS;
        s32 angle1 = ((i + 1) * 0x1000) / GDX_VR_SKY_SEGMENTS;
        f32 x0 = camera->eye.x + COS(angle0) * radius;
        f32 z0 = camera->eye.z + SIN(angle0) * radius;
        f32 x1 = camera->eye.x + COS(angle1) * radius;
        f32 z1 = camera->eye.z + SIN(angle1) * radius;
        f32 topY = camera->eye.y + halfHeight;
        f32 bottomY = camera->eye.y - halfHeight;

        SET_VTX(vtx + 0, GdxVr_ClampVtx(x0), GdxVr_ClampVtx(topY), GdxVr_ClampVtx(z0), 0, 0, 255, 255, 255, 0);
        SET_VTX(vtx + 1, GdxVr_ClampVtx(x1), GdxVr_ClampVtx(topY), GdxVr_ClampVtx(z1), 0, 0, 255, 255, 255, 0);
        SET_VTX(vtx + 2, GdxVr_ClampVtx(x0), GdxVr_ClampVtx(bottomY), GdxVr_ClampVtx(z0), 0, 0, 255, 255, 255, 150);
        SET_VTX(vtx + 3, GdxVr_ClampVtx(x1), GdxVr_ClampVtx(bottomY), GdxVr_ClampVtx(z1), 0, 0, 255, 255, 255, 150);
        vtx += 4;
    }

    gDPPipeSync(gfx++);
    gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(gfx++, 0, 0, skybox->courseFogR, skybox->courseFogG, skybox->courseFogB, 150);
    gSPClearGeometryMode(gfx++, G_CULL_BACK | G_CULL_FRONT);
    for (i = 0; i < GDX_VR_SKY_SEGMENTS; i += 8) {
        s32 j;
        gSPVertex(gfx++, &sGdxVrFogCylinderVtx[i * 4], 32, 0);
        for (j = 0; j < 8; j++) {
            s32 b = j * 4;
            gSP2Triangles(gfx++, b + 0, b + 1, b + 2, 0, b + 2, b + 1, b + 3, 0);
        }
    }
    return gfx;
}

static Gfx* GdxVr_DrawCloudPlane(Gfx* gfx, Camera* camera, Background* background) {
    static Vtx sCloudPlane[4];
    ScrollingBackground* cloud = &background->cloudScroll;
    f32 cloudHeightAboveEye = (background->pos.y + cloud->relativeBackgroundHeight) - camera->eye.y;
    f32 y = (cloudHeightAboveEye > 0.0f) ? camera->eye.y + cloud->relativeEyeHeight
                                         : background->pos.y + cloud->relativeBackgroundHeight;
    f32 range = 7200.0f;
    f32 x0 = camera->eye.x - range;
    f32 x1 = camera->eye.x + range;
    f32 z0 = camera->eye.z - range;
    f32 z1 = camera->eye.z + range;
    f32 s0f = (x0 * cloud->xScale) + cloud->xScroll;
    f32 s1f = (x1 * cloud->xScale) + cloud->xScroll;
    f32 t0f = (z0 * cloud->zScale) + cloud->zScroll;
    f32 t1f = (z1 * cloud->zScale) + cloud->zScroll;
    f32 sBase = floorf(s0f / 1024.0f) * 1024.0f;
    f32 tBase = floorf(t0f / 1024.0f) * 1024.0f;
    s32 s0 = (s32) ((s0f - sBase) * 32.0f);
    s32 s1 = (s32) ((s1f - sBase) * 32.0f);
    s32 t0 = (s32) ((t0f - tBase) * 32.0f);
    s32 t1 = (s32) ((t1f - tBase) * 32.0f);
    s32 alpha = 190;

    SET_VTX(&sCloudPlane[0], GdxVr_ClampVtx(x0), GdxVr_ClampVtx(y), GdxVr_ClampVtx(z0), s0, t0, 255, 255, 255, alpha);
    SET_VTX(&sCloudPlane[1], GdxVr_ClampVtx(x1), GdxVr_ClampVtx(y), GdxVr_ClampVtx(z0), s1, t0, 255, 255, 255, alpha);
    SET_VTX(&sCloudPlane[2], GdxVr_ClampVtx(x0), GdxVr_ClampVtx(y), GdxVr_ClampVtx(z1), s0, t1, 255, 255, 255, alpha);
    SET_VTX(&sCloudPlane[3], GdxVr_ClampVtx(x1), GdxVr_ClampVtx(y), GdxVr_ClampVtx(z1), s1, t1, 255, 255, 255, alpha);

    gSPClearGeometryMode(gfx++, G_CULL_BACK | G_CULL_FRONT);
    gSPVertex(gfx++, sCloudPlane, 4, 0);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 2, 1, 3, 0);
    return gfx;
}

static Gfx* GdxVr_DrawStars(Gfx* gfx, s32 cameraIndex, Camera* camera) {
    Star* star;
    s32 i;
    (void) cameraIndex;

    gDPLoadTextureBlock(gfx++, sStarTexture, G_IM_FMT_IA, G_IM_SIZ_8b, 8, 8, 0,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPClearGeometryMode(gfx++, G_CULL_BACK | G_CULL_FRONT);

    for (i = 0, star = sStars; i < sStarCount && i < GDX_VR_MAX_STARS; i++, star++) {
        f32 len = sqrtf(SQ(star->pos.x) + SQ(star->pos.y) + SQ(star->pos.z));
        f32 dx, dy, dz;
        f32 rx, ry, rz;
        f32 ux, uy, uz;
        f32 rn;
        f32 size = 42.0f;
        f32 radius = 6200.0f;
        f32 cx, cy, cz;
        Vtx* sv = sGdxVrStarVtx[i];

        if (len < 1.0f) {
            continue;
        }
        dx = star->pos.x / len;
        dy = star->pos.y / len;
        dz = star->pos.z / len;
        cx = camera->eye.x + dx * radius;
        cy = camera->eye.y + dy * radius;
        cz = camera->eye.z + dz * radius;

        /* Tangent billboard basis on the celestial sphere. */
        rx = dz;
        ry = 0.0f;
        rz = 0.0f - dx;
        rn = sqrtf(SQ(rx) + SQ(rz));
        if (rn < 0.001f) {
            rx = 1.0f;
            rz = 0.0f;
        } else {
            rx /= rn;
            rz /= rn;
        }
        ux = (dy * rz) - (dz * ry);
        uy = (dz * rx) - (dx * rz);
        uz = (dx * ry) - (dy * rx);

        SET_VTX(sv + 0, GdxVr_ClampVtx(cx - rx * size + ux * size),
                GdxVr_ClampVtx(cy - ry * size + uy * size),
                GdxVr_ClampVtx(cz - rz * size + uz * size), 0, 0, 255, 255, 255, 255);
        SET_VTX(sv + 1, GdxVr_ClampVtx(cx + rx * size + ux * size),
                GdxVr_ClampVtx(cy + ry * size + uy * size),
                GdxVr_ClampVtx(cz + rz * size + uz * size), 7 * 32, 0, 255, 255, 255, 255);
        SET_VTX(sv + 2, GdxVr_ClampVtx(cx - rx * size - ux * size),
                GdxVr_ClampVtx(cy - ry * size - uy * size),
                GdxVr_ClampVtx(cz - rz * size - uz * size), 0, 7 * 32, 255, 255, 255, 255);
        SET_VTX(sv + 3, GdxVr_ClampVtx(cx + rx * size - ux * size),
                GdxVr_ClampVtx(cy + ry * size - uy * size),
                GdxVr_ClampVtx(cz + rz * size - uz * size), 7 * 32, 7 * 32, 255, 255, 255, 255);

        gDPPipeSync(gfx++);
        gDPSetPrimColor(gfx++, 0, 0, star->red, star->green, star->blue, star->alpha);
        gSPVertex(gfx++, sv, 4, 0);
        gSP2Triangles(gfx++, 0, 1, 2, 0, 2, 1, 3, 0);
    }
    gDPPipeSync(gfx++);
    return gfx;
}
#endif

Gfx* Background_Draw(Gfx* gfx, s32 cameraIndex, s32 scissorBoxType) {
]=])
string(REPLACE "${_quest_background_helper_marker}" "${_quest_background_vr_helpers}"
       _gdx_background "${_gdx_background}")

set(_quest_sky_draw_old [=[
    gDPLoadTextureBlock(gfx++, sSkyboxTexture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 1, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

    gSP2Triangles(gfx++, 8, 11, 9, 0, 8, 10, 11, 0);

    if (sSkyboxFlags & SKYBOX_STARRY) {
        gSPDisplayList(gfx++, D_303AA18);
        gfx = Background_DrawStars(gfx, cameraIndex);
    }
]=])
set(_quest_sky_draw_new [=[
    gDPLoadTextureBlock(gfx++, sSkyboxTexture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 1, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

#ifdef GDX_QUEST_VR
    /* The stock sky is a single camera-facing quad (vertices 8..11). In a headset that quad is a
       very obvious flat wall, especially on Mute City where its fog/sky colours are nearly white.
       The 360-degree cylinder above is the VR replacement, so NEVER draw the legacy frontal quad
       in an eye pass. Keeping both layers was the source of the white wall at the old skybox depth. */
    gfx = GdxVr_DrawSkyCylinder(gfx, camera, &sBackgrounds[cameraIndex]);
    gSPVertex(gfx++, &D_1000000.unk_29B48[cameraIndex * 28], 28, 0);
#else
    gSP2Triangles(gfx++, 8, 11, 9, 0, 8, 10, 11, 0);
#endif

    if (sSkyboxFlags & SKYBOX_STARRY) {
        gSPDisplayList(gfx++, D_303AA18);
#ifdef GDX_QUEST_VR
        gfx = GdxVr_DrawStars(gfx, cameraIndex, camera);
        /* Star billboards replace the vertex cache; restore the background set used below. */
        gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPVertex(gfx++, &D_1000000.unk_29B48[cameraIndex * 28], 28, 0);
#else
        gfx = Background_DrawStars(gfx, cameraIndex);
#endif
    }
]=])
string(REPLACE "${_quest_sky_draw_old}" "${_quest_sky_draw_new}" _gdx_background "${_gdx_background}")

set(_quest_cloud_draw_old [=[
        gSP2Triangles(gfx++, 12, 15, 13, 0, 12, 14, 15, 0);
        gSP2Triangles(gfx++, 16, 19, 17, 0, 16, 18, 19, 0);
]=])
set(_quest_cloud_draw_new [=[
#ifdef GDX_QUEST_VR
        gfx = GdxVr_DrawCloudPlane(gfx, camera, &sBackgrounds[cameraIndex]);
#else
        gSP2Triangles(gfx++, 12, 15, 13, 0, 12, 14, 15, 0);
        gSP2Triangles(gfx++, 16, 19, 17, 0, 16, 18, 19, 0);
#endif
]=])
string(REPLACE "${_quest_cloud_draw_old}" "${_quest_cloud_draw_new}" _gdx_background "${_gdx_background}")

set(_quest_background_world_boundary [=[
    if (cameraIndex == 0 && (sBackgroundSpriteCount != 0)) {
]=])
set(_quest_background_world_marker [=[
#ifdef PORT
    gDPNoOpTag(gfx++, 0x56523344); /* 'VR3D' -- finite background starts here */
#endif
    if (cameraIndex == 0 && (sBackgroundSpriteCount != 0)) {
]=])
string(REPLACE "${_quest_background_world_boundary}" "${_quest_background_world_marker}"
       _gdx_background "${_gdx_background}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/background_quest.c" "${_gdx_background}")

# 5) Quest VR camera capture hook. The host reads the untouched chase-camera pose at the exact point
# where F-Zero has finished updating it. Current Quest code intentionally returns 0 from the hook,
# so camera_quest does NOT mutate gCameras; HMD 6DoF/IPD are composed render-only in Fast3D, like
# mario64VRStandalone. The legacy restore function remains harmless for compatibility.
file(READ "${GDX_DECOMP_DIR}/src/game/camera.c" _gdx_camera)
string(REPLACE
    "Camera gCameras[4];"
    "Camera gCameras[4];\n#ifdef GDX_QUEST_VR\ntypedef struct GdxVrCameraIo {\n    s32 id;\n    s32 numPlayers;\n    f32 eyeX, eyeY, eyeZ;\n    f32 atX, atY, atZ;\n    f32 upX, upY, upZ;\n    f32 fov;\n    f32 nearZ, farZ;\n    f32 fovScaleX, fovScaleY;\n    f32 frustrumCenterX, frustrumCenterY;\n} GdxVrCameraIo;\nstatic Camera sGdxQuestVrOriginalCamera;\nstatic s32 sGdxQuestVrCameraApplied = 0;\nextern GfxPool* gGfxPool;\nextern s32 gNumPlayers;\nextern s32 gdx_vr_host_apply_center_camera(GdxVrCameraIo* camera);\n#endif"
    _gdx_camera "${_gdx_camera}")
set(_quest_camera_update_old [=[
    Camera_UpdateFromSettings(camera, cameraSettings);
    func_8071315C(camera);
    Camera_UpdateProjectionViewMtx(gGfxPool, camera);
]=])
set(_quest_camera_update_new [=[
    Camera_UpdateFromSettings(camera, cameraSettings);
#ifdef GDX_QUEST_VR
    if (camera->id == 0) {
        GdxVrCameraIo vr;
        sGdxQuestVrOriginalCamera = *camera;
        vr.id = camera->id;
        vr.numPlayers = gNumPlayers;
        vr.eyeX = camera->eye.x;
        vr.eyeY = camera->eye.y;
        vr.eyeZ = camera->eye.z;
        vr.atX = camera->at.x;
        vr.atY = camera->at.y;
        vr.atZ = camera->at.z;
        vr.upX = camera->basis.y.x;
        vr.upY = camera->basis.y.y;
        vr.upZ = camera->basis.y.z;
        vr.fov = camera->fov;
        vr.nearZ = camera->near;
        vr.farZ = camera->far;
        vr.fovScaleX = camera->fovScaleX;
        vr.fovScaleY = camera->fovScaleY;
        vr.frustrumCenterX = camera->frustrumCenterX;
        vr.frustrumCenterY = camera->frustrumCenterY;
        sGdxQuestVrCameraApplied = gdx_vr_host_apply_center_camera(&vr);
        if (sGdxQuestVrCameraApplied) {
            camera->eye.x = vr.eyeX;
            camera->eye.y = vr.eyeY;
            camera->eye.z = vr.eyeZ;
            camera->at.x = vr.atX;
            camera->at.y = vr.atY;
            camera->at.z = vr.atZ;
            camera->basis.y.x = vr.upX;
            camera->basis.y.y = vr.upY;
            camera->basis.y.z = vr.upZ;
            camera->fov = vr.fov;
            camera->near = vr.nearZ;
            camera->far = vr.farZ;
            camera->fovScaleX = vr.fovScaleX;
            camera->fovScaleY = vr.fovScaleY;
            camera->frustrumCenterX = vr.frustrumCenterX;
            camera->frustrumCenterY = vr.frustrumCenterY;
        }
    }
#endif
    func_8071315C(camera);
    Camera_UpdateProjectionViewMtx(gGfxPool, camera);
]=])
string(REPLACE "${_quest_camera_update_old}" "${_quest_camera_update_new}" _gdx_camera "${_gdx_camera}")

set(_quest_camera_restore_marker "extern s32 gNearestRacer;")
set(_quest_camera_restore [=[
#ifdef GDX_QUEST_VR
void gdx_quest_vr_camera_restore(void) {
    if (!sGdxQuestVrCameraApplied) {
        return;
    }
    gCameras[0] = sGdxQuestVrOriginalCamera;
    func_8071315C(&gCameras[0]);
    Camera_UpdateProjectionViewMtx(gGfxPool, &gCameras[0]);
    sGdxQuestVrCameraApplied = 0;
}
#endif

extern s32 gNearestRacer;
]=])
string(REPLACE "${_quest_camera_restore_marker}" "${_quest_camera_restore}" _gdx_camera "${_gdx_camera}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/camera_quest.c" "${_gdx_camera}")

# 6) Quest VR course culling. The stock CPU builds visible course chunks using the chase camera
# BEFORE the host applies headset yaw/pitch to the final projection-view matrix. Turning the head
# therefore looks like the track bends/disappears because side/back chunks were never submitted.
# Keep the streamed chunk set bounded with a Quest/HMD-oriented frustum. The original VR patch
# made visibility fully omnidirectional (absolute depth only), which prevented head-turn pop-in but
# forced large amounts of off-screen track geometry through both eye renders. The host now evaluates
# the current physical HMD direction/position with a conservative chunk margin, preserving 6DoF
# visibility while restoring the most important CPU/GPU culling win.
file(READ "${GDX_DECOMP_DIR}/src/game/course.c" _gdx_course)
set(_quest_course_cull_old [=[
#ifdef PORT
        if ((chunk->depth < 0.0f) || ((sCourseFarRenderDistance * gdxFarRenderDistanceScale) < chunk->depth)) {
#else
        if ((chunk->depth < 0.0f) || (sCourseFarRenderDistance < chunk->depth)) {
#endif
            chunk->drawState = 0;
        } else {
            temp_fa0 =
                1.0f /
                (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                 sp60.m[3][3]);
            var_fv1 = temp_fa0 *
                      (((sp60.m[0][0] * chunk->pos.x) + (sp60.m[1][0] * chunk->pos.y) + (sp60.m[2][0] * chunk->pos.z)) +
                       sp60.m[3][0]);
#ifdef PORT
            if ((var_fv1 < -gdxCullX) || (var_fv1 > gdxCullX)) {
#else
            if ((var_fv1 < -1.0f) || (var_fv1 > 1.0f)) {
#endif
                chunk->drawState = 0;
            } else {
                temp_fa0 =
                    1.0f /
                    (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                     sp60.m[3][3]);
                var_fv1 =
                    temp_fa0 *
                    (((sp60.m[0][1] * chunk->pos.x) + (sp60.m[1][1] * chunk->pos.y) + (sp60.m[2][1] * chunk->pos.z)) +
                     sp60.m[3][1]);
                if ((var_fv1 < -1.0f) || (var_fv1 > 1.0f)) {
                    chunk->drawState = 0;
                } else {
                    chunk->drawState = 1;
                }
            }
        }
]=])
set(_quest_course_cull_new [=[
#ifdef GDX_QUEST_VR
        {
            extern s32 gdx_vr_host_course_chunk_visible(f32 x, f32 y, f32 z, f32 depth, f32 farDistance);
            chunk->drawState = gdx_vr_host_course_chunk_visible(
                chunk->pos.x, chunk->pos.y, chunk->pos.z, chunk->depth,
                sCourseFarRenderDistance * gdxFarRenderDistanceScale);
        }
#elif defined(PORT)
        if ((chunk->depth < 0.0f) || ((sCourseFarRenderDistance * gdxFarRenderDistanceScale) < chunk->depth)) {
            chunk->drawState = 0;
        } else {
            temp_fa0 =
                1.0f /
                (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                 sp60.m[3][3]);
            var_fv1 = temp_fa0 *
                      (((sp60.m[0][0] * chunk->pos.x) + (sp60.m[1][0] * chunk->pos.y) + (sp60.m[2][0] * chunk->pos.z)) +
                       sp60.m[3][0]);
            if ((var_fv1 < -gdxCullX) || (var_fv1 > gdxCullX)) {
                chunk->drawState = 0;
            } else {
                temp_fa0 =
                    1.0f /
                    (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                     sp60.m[3][3]);
                var_fv1 =
                    temp_fa0 *
                    (((sp60.m[0][1] * chunk->pos.x) + (sp60.m[1][1] * chunk->pos.y) + (sp60.m[2][1] * chunk->pos.z)) +
                     sp60.m[3][1]);
                chunk->drawState = ((var_fv1 < -1.0f) || (var_fv1 > 1.0f)) ? 0 : 1;
            }
        }
#else
        if ((chunk->depth < 0.0f) || (sCourseFarRenderDistance < chunk->depth)) {
            chunk->drawState = 0;
        } else {
            temp_fa0 =
                1.0f /
                (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                 sp60.m[3][3]);
            var_fv1 = temp_fa0 *
                      (((sp60.m[0][0] * chunk->pos.x) + (sp60.m[1][0] * chunk->pos.y) + (sp60.m[2][0] * chunk->pos.z)) +
                       sp60.m[3][0]);
            if ((var_fv1 < -1.0f) || (var_fv1 > 1.0f)) {
                chunk->drawState = 0;
            } else {
                temp_fa0 =
                    1.0f /
                    (((sp60.m[0][3] * chunk->pos.x) + (sp60.m[1][3] * chunk->pos.y) + (sp60.m[2][3] * chunk->pos.z)) +
                     sp60.m[3][3]);
                var_fv1 =
                    temp_fa0 *
                    (((sp60.m[0][1] * chunk->pos.x) + (sp60.m[1][1] * chunk->pos.y) + (sp60.m[2][1] * chunk->pos.z)) +
                     sp60.m[3][1]);
                chunk->drawState = ((var_fv1 < -1.0f) || (var_fv1 > 1.0f)) ? 0 : 1;
            }
        }
#endif
]=])
string(REPLACE "${_quest_course_cull_old}" "${_quest_course_cull_new}" _gdx_course "${_gdx_course}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/course_quest.c" "${_gdx_course}")

# 5d) Racer position/rival markers. Stock F-Zero projects the top-3/rival world positions through
# the TV camera on the CPU, then draws screen-space texture rectangles. In HMD rendering those
# rectangles behave like the old stars: they follow the camera instead of remaining attached to
# the machines. Replace only those rectangles with small world-space textured quads. The game's
# ordinary marker selection/rank logic and textures remain untouched.
file(READ "${GDX_DECOMP_DIR}/src/game/racer.c" _gdx_racer)
set(_quest_racer_draw_marker "Gfx* Racer_Draw(Gfx* gfx, s32 playerIndex) {")
set(_quest_racer_vr_helpers [=[
#ifdef GDX_QUEST_VR
extern Mtx D_2000000[];
static Vtx sGdxVrRacerMarkerVtx[4][5][4];

static s32 GdxVr_RacerMarkerClamp(f32 v) {
    if (v < -32000.0f) return -32000;
    if (v > 32000.0f) return 32000;
    return (s32) v;
}

static Gfx* GdxVr_DrawRacerMarkerQuad(Gfx* gfx, Camera* camera, Racer* racer,
                                      s32 playerIndex, s32 slot, s32 texWidth, s32 texHeight) {
    Vtx* v;
    f32 pixelScale = 1.35f;
    f32 halfW;
    f32 halfH;
    f32 cx;
    f32 cy;
    f32 cz;
    f32 rx;
    f32 ry;
    f32 rz;
    f32 ux;
    f32 uy;
    f32 uz;
    f32 lift = 72.0f;

    if ((playerIndex < 0) || (playerIndex >= 4) || (slot < 0) || (slot >= 5)) {
        return gfx;
    }
    v = sGdxVrRacerMarkerVtx[playerIndex][slot];
    halfW = texWidth * pixelScale * 0.5f;
    halfH = texHeight * pixelScale * 0.5f;

    cx = racer->segmentPositionInfo.pos.x + racer->trueBasis.y.x * lift;
    cy = racer->segmentPositionInfo.pos.y + racer->trueBasis.y.y * lift;
    cz = racer->segmentPositionInfo.pos.z + racer->trueBasis.y.z * lift;

    /* Face the ordinary race camera at authoring time, but keep the marker's CENTER in real world
       coordinates. Fast3D's per-eye HMD matrix then supplies the actual VR projection. */
    rx = camera->basis.z.x;
    ry = camera->basis.z.y;
    rz = camera->basis.z.z;
    ux = camera->basis.y.x;
    uy = camera->basis.y.y;
    uz = camera->basis.y.z;

    /* Match gSPScisTextureRectangle's full texel footprint. Using (width-1,height-1) mapped the
       OUTER quad vertices to the centres of the last texels, so raster interpolation dropped the
       final row/column and visibly cropped small glyphs such as CHECK and 1st/2nd/3rd. The texrect
       equivalent is [0,width] x [0,height] in 5-bit fractional texture coordinates. */
    /* The world billboard basis itself is correct; the converted texrect glyphs are mirrored in U.
       Keep the original top-to-bottom T convention and reverse only S/U. This fixes the horizontal
       mirror on 1st/2nd/3rd/CHECK without rotating or vertically flipping the world-space quad. */
    SET_VTX(v + 0, GdxVr_RacerMarkerClamp(cx - rx * halfW + ux * halfH),
            GdxVr_RacerMarkerClamp(cy - ry * halfW + uy * halfH),
            GdxVr_RacerMarkerClamp(cz - rz * halfW + uz * halfH), texWidth * 32, 0, 255, 255, 255, 255);
    SET_VTX(v + 1, GdxVr_RacerMarkerClamp(cx + rx * halfW + ux * halfH),
            GdxVr_RacerMarkerClamp(cy + ry * halfW + uy * halfH),
            GdxVr_RacerMarkerClamp(cz + rz * halfW + uz * halfH), 0, 0, 255, 255, 255, 255);
    SET_VTX(v + 2, GdxVr_RacerMarkerClamp(cx - rx * halfW - ux * halfH),
            GdxVr_RacerMarkerClamp(cy - ry * halfW - uy * halfH),
            GdxVr_RacerMarkerClamp(cz - rz * halfW - uz * halfH), texWidth * 32,
            texHeight * 32, 255, 255, 255, 255);
    SET_VTX(v + 3, GdxVr_RacerMarkerClamp(cx + rx * halfW - ux * halfH),
            GdxVr_RacerMarkerClamp(cy + ry * halfW - uy * halfH),
            GdxVr_RacerMarkerClamp(cz + rz * halfW - uz * halfH), 0, texHeight * 32,
            255, 255, 255, 255);

    gDPPipeSync(gfx++);
    /* These labels used to be screen-space texrects. In VR they are world-positioned billboards,
       but they must retain overlay semantics: never let the racer body/track Z-buffer cut a glyph
       in half, and never inherit a narrow TV-camera scissor. Use a full N64 viewport just for the
       quad, then restore the active player's scissor immediately afterwards. */
    gDPSetScissor(gfx++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gSPMatrix(gfx++, D_2000000, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPClearGeometryMode(gfx++, G_ZBUFFER | G_CULL_BACK | G_CULL_FRONT);
    gSPTexture(gfx++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPVertex(gfx++, v, 4, 0);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 2, 1, 3, 0);
    gSPPopMatrix(gfx++, G_MTX_MODELVIEW);
    gSPSetGeometryMode(gfx++, G_ZBUFFER);
    gDPSetScissor(gfx++, G_SC_NON_INTERLACE,
                  camera->currentScissorLeft, camera->currentScissorTop,
                  camera->currentScissorRight, camera->currentScissorBottom);
    return gfx;
}
#endif

Gfx* Racer_Draw(Gfx* gfx, s32 playerIndex) {
]=])
string(REPLACE "${_quest_racer_draw_marker}" "${_quest_racer_vr_helpers}" _gdx_racer "${_gdx_racer}")

set(_quest_marker_1p_old [=[
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (24 * 4 - 1), sp5C4 + (30 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
]=])
set(_quest_marker_1p_new [=[
#ifdef GDX_QUEST_VR
                    gfx = GdxVr_DrawRacerMarkerQuad(gfx, camera, racer, playerIndex, var_s3, 24, 30);
#else
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (24 * 4 - 1), sp5C4 + (30 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
#endif
]=])
string(REPLACE "${_quest_marker_1p_old}" "${_quest_marker_1p_new}" _gdx_racer "${_gdx_racer}")

set(_quest_marker_mp_old [=[
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (16 * 4 - 1), sp5C4 + (16 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
]=])
set(_quest_marker_mp_new [=[
#ifdef GDX_QUEST_VR
                    gfx = GdxVr_DrawRacerMarkerQuad(gfx, camera, racer, playerIndex, var_s3, 16, 16);
#else
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (16 * 4 - 1), sp5C4 + (16 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
#endif
]=])
string(REPLACE "${_quest_marker_mp_old}" "${_quest_marker_mp_new}" _gdx_racer "${_gdx_racer}")

set(_quest_marker_rival_old [=[
                gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (32 * 4 - 1), sp5C4 + (16 * 4 - 1), 0, 0, 0,
                                        1 << 10, 1 << 10);
]=])
set(_quest_marker_rival_new [=[
#ifdef GDX_QUEST_VR
                gfx = GdxVr_DrawRacerMarkerQuad(gfx, camera, sRivalRacer, playerIndex, 3, 32, 16);
#else
                gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (32 * 4 - 1), sp5C4 + (16 * 4 - 1), 0, 0, 0,
                                        1 << 10, 1 << 10);
#endif
]=])
string(REPLACE "${_quest_marker_rival_old}" "${_quest_marker_rival_new}" _gdx_racer "${_gdx_racer}")

set(_quest_check_1p_old [=[
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (32 * 4 - 1), sp5C4 + (23 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
]=])
set(_quest_check_1p_new [=[
#ifdef GDX_QUEST_VR
                    gfx = GdxVr_DrawRacerMarkerQuad(gfx, camera, playerRacer->racerBehind, playerIndex, 4, 32, 23);
#else
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (32 * 4 - 1), sp5C4 + (23 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
#endif
]=])
string(REPLACE "${_quest_check_1p_old}" "${_quest_check_1p_new}" _gdx_racer "${_gdx_racer}")

set(_quest_check_mp_old [=[
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (16 * 4 - 1), sp5C4 + (10 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
]=])
set(_quest_check_mp_new [=[
#ifdef GDX_QUEST_VR
                    gfx = GdxVr_DrawRacerMarkerQuad(gfx, camera, playerRacer->racerBehind, playerIndex, 4, 16, 10);
#else
                    gSPScisTextureRectangle(gfx++, var_s7, sp5C4, var_s7 + (16 * 4 - 1), sp5C4 + (10 * 4 - 1), 0, 0, 0,
                                            1 << 10, 1 << 10);
#endif
]=])
string(REPLACE "${_quest_check_mp_old}" "${_quest_check_mp_new}" _gdx_racer "${_gdx_racer}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/racer_quest.c" "${_gdx_racer}")

# 6) Full texture-cache clears on mode/arena rewinds must also reach the right-eye interpreter.
file(READ "${GDX_SOURCE_DIR}/port/decomp_port.c" _gdx_decomp_port)
string(REPLACE
    "extern void gfx_texture_cache_clear(void);"
    "extern void gfx_texture_cache_clear(void);\nextern void gdx_vr_host_mirror_texture_clear(void);"
    _gdx_decomp_port "${_gdx_decomp_port}")
string(REPLACE
    "    gfx_texture_cache_clear();"
    "    gfx_texture_cache_clear();\n    gdx_vr_host_mirror_texture_clear();"
    _gdx_decomp_port "${_gdx_decomp_port}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/decomp_port_quest.c" "${_gdx_decomp_port}")

# 7) Base-cart Quest builds intentionally omit the 64DD/Expansion Kit runtime. The decomp's base
# boot code treats >=8 MiB RAM as "DD compatible" and then DMA-loads the LEO overlay into
# leo_VRAM / clears leo_BSS. On a host build those linker symbols are one-byte LinkStubs, not an
# N64 overlay address range; using them as a DMA/BSS destination corrupts memory (and on Android
# the BSS stub ordering can produce a negative size passed to bzero). Force the DD-compat flag off
# only in this generated Quest translation unit; ordinary cartridge gameplay keeps the 16 MiB host
# RDRAM allocator while all LEO/64DD startup work is skipped.
file(READ "${GDX_DECOMP_DIR}/src/sys/sys_main.c" _gdx_sys_main)
set(_quest_ramdd_marker "    GDX_CKI(6b_ramdd, gRamDDCompatible);")
set(_quest_ramdd_disable [=[
#ifdef GDX_QUEST_NO_64DD
    gRamDDCompatible = false;
    osAppNMIBuffer[14] = false;
#endif
    GDX_CKI(6b_ramdd, gRamDDCompatible);
]=])
string(REPLACE "${_quest_ramdd_marker}" "${_quest_ramdd_disable}" _gdx_sys_main "${_gdx_sys_main}")
file(WRITE "${GDX_QUEST_GENERATED_DIR}/sys_main_quest.c" "${_gdx_sys_main}")

# 7b) Quest audio ROM path. Upstream G-Diffuser historically skipped Audio_Init on PORT, so three
# ROM-audio entry points were compiled as no-ops with comments saying exactly that. This Quest port
# now runs the real Audio_Init/AudioLoad_Init path in Audio_ThreadEntry, so leaving those PORT
# returns in place initializes the context but never advances its per-frame audio commands. Re-enable
# the original ROM behavior only for this generated Quest build.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/external.c" _gdx_audio_rom_external)
string(REPLACE
    "void Audio_Update(void) {\n#ifdef PORT\n    return; // Audio_Init is skipped on PORT; nothing to update.\n#endif\n    Audio_UpdateImpl();\n    AudioThread_ScheduleProcessCmds();\n}"
    "void Audio_Update(void) {\n    Audio_UpdateImpl();\n    AudioThread_ScheduleProcessCmds();\n}"
    _gdx_audio_rom_external "${_gdx_audio_rom_external}")
string(REPLACE
    "void Audio_SetOutMode(u8 soundMode) {\n#ifdef PORT\n    (void) soundMode;\n    return; // Audio_Init is skipped on PORT.\n#endif\n    AUDIOCMD_GLOBAL_SET_SOUND_MODE(soundMode);\n}"
    "void Audio_SetOutMode(u8 soundMode) {\n    AUDIOCMD_GLOBAL_SET_SOUND_MODE(soundMode);\n}"
    _gdx_audio_rom_external "${_gdx_audio_rom_external}")
string(REPLACE
    "void Audio_GuitarSeqStart(void) {\n#ifdef PORT\n    return; // Audio_Init is skipped on PORT; the seqplayer init below would crash.\n#endif"
    "void Audio_GuitarSeqStart(void) {"
    _gdx_audio_rom_external "${_gdx_audio_rom_external}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/external.c" "${_gdx_audio_rom_external}")

# 7c) ROM AudioCmd host ABI/endian fix. AUDIO_MK_CMD packs op/arg0/arg1/arg2 into the high-to-low
# bytes of a u32 exactly like the big-endian N64. On little-endian PORT hosts, the anonymous byte
# struct otherwise reads those four bytes backwards, so INIT_SEQPLAYER (0x82xxxxxx) appears as
# op=0x00 and is silently ignored. The original typed queue helpers also cast 32-bit locals to
# void** and dereference 8 bytes on arm64. Reverse only the byte view on PORT and write each data
# variant directly into its union member.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/audio.h" _gdx_audio_rom_header)
set(_quest_audio_cmd_layout_old [=[
        struct {
            u8 op;
            u8 arg0;
            u8 arg1;
            u8 arg2;
        };
]=])
set(_quest_audio_cmd_layout_new [=[
        struct {
#ifdef PORT
            u8 arg2;
            u8 arg1;
            u8 arg0;
            u8 op;
#else
            u8 op;
            u8 arg0;
            u8 arg1;
            u8 arg2;
#endif
        };
]=])
string(REPLACE "${_quest_audio_cmd_layout_old}" "${_quest_audio_cmd_layout_new}"
       _gdx_audio_rom_header "${_gdx_audio_rom_header}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/audio.h" "${_gdx_audio_rom_header}")

file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/thread.c" _gdx_audio_rom_typed_queue)
set(_quest_audio_queue_old [=[
void AudioThread_QueueCmd(u32 opArgs, void** data) {
    AudioCmd* audioCmd = &gThreadCmdBuffer[gThreadCmdWritePos & 0xFF];

    audioCmd->opArgs = opArgs;
    audioCmd->data = *data;

    gThreadCmdWritePos++;
    if (gThreadCmdWritePos == gThreadCmdReadPos) {
        gThreadCmdWritePos--;
    }
}

void AudioThread_QueueCmdF32(u32 opArgs, f32 data) {
    AudioThread_QueueCmd(opArgs, (void**) &data);
}

void AudioThread_QueueCmdU32(u32 opArgs, u32 data) {
    AudioThread_QueueCmd(opArgs, (void**) &data);
}

void AudioThread_QueueCmdS8(u32 opArgs, s8 data) {
    u32 uData = data << 0x18;

    AudioThread_QueueCmd(opArgs, (void**) &uData);
}

void AudioThread_QueueCmdU16(u32 opArgs, u16 data) {
    u32 uData = data << 0x10;

    AudioThread_QueueCmd(opArgs, (void**) &uData);
}
]=])
set(_quest_audio_queue_new [=[
static AudioCmd* GdxQuestAudioQueueCmdBegin(u32 opArgs) {
    AudioCmd* audioCmd = &gThreadCmdBuffer[gThreadCmdWritePos & 0xFF];
    audioCmd->opArgs = opArgs;
    gThreadCmdWritePos++;
    if (gThreadCmdWritePos == gThreadCmdReadPos) {
        gThreadCmdWritePos--;
    }
    return audioCmd;
}

void AudioThread_QueueCmdF32(u32 opArgs, f32 data) {
    GdxQuestAudioQueueCmdBegin(opArgs)->asFloat = data;
}

void AudioThread_QueueCmdU32(u32 opArgs, u32 data) {
    GdxQuestAudioQueueCmdBegin(opArgs)->asUInt = data;
}

void AudioThread_QueueCmdS8(u32 opArgs, s8 data) {
    GdxQuestAudioQueueCmdBegin(opArgs)->asSbyte = data;
}

void AudioThread_QueueCmdU16(u32 opArgs, u16 data) {
    GdxQuestAudioQueueCmdBegin(opArgs)->asUShort = data;
}
]=])
string(REPLACE "${_quest_audio_queue_old}" "${_quest_audio_queue_new}"
       _gdx_audio_rom_typed_queue "${_gdx_audio_rom_typed_queue}")
string(REPLACE "AudioThread_SetFadeInTimer(cmd->arg0, cmd->data);"
               "AudioThread_SetFadeInTimer(cmd->arg0, cmd->asInt);"
               _gdx_audio_rom_typed_queue "${_gdx_audio_rom_typed_queue}")
string(REPLACE "AudioLoad_SyncInitSeqPlayerSkipTicks(cmd->arg0, cmd->arg1, cmd->data);"
               "AudioLoad_SyncInitSeqPlayerSkipTicks(cmd->arg0, cmd->arg1, cmd->asInt);"
               _gdx_audio_rom_typed_queue "${_gdx_audio_rom_typed_queue}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/thread.c" "${_gdx_audio_rom_typed_queue}")

# 7d) Base-cart soundfont relocation on 64-bit hosts. The ROM blob stores big-endian 32-bit
# offsets and cannot be patched in-place into native pointers on AArch64. quest_audio_fontconv.c
# provides a host-native parser derived from G-Diffuser's already-working Expansion-Kit PORT
# converter. Compile the original in-place relocator only for non-PORT builds.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/load.c" _gdx_audio_rom_reloc)
if(NOT _gdx_audio_rom_reloc MATCHES "#ifndef PORT\\nvoid AudioLoad_RelocateFont")
    string(REPLACE
        "void AudioLoad_RelocateFont(s32 fontId, uintptr_t fontBaseAddr, void* relocData) {"
        "#ifndef PORT\nvoid AudioLoad_RelocateFont(s32 fontId, uintptr_t fontBaseAddr, void* relocData) {"
        _gdx_audio_rom_reloc "${_gdx_audio_rom_reloc}")
endif()
if(NOT _gdx_audio_rom_reloc MATCHES "#endif /\\* !PORT \\*/\\n\\nvoid AudioLoad_SyncDma")
    string(REPLACE
        "    gSoundFontList[fontId].instruments = (Instrument**) &fontDataPtrs[1];\n}\n\nvoid AudioLoad_SyncDma"
        "    gSoundFontList[fontId].instruments = (Instrument**) &fontDataPtrs[1];\n}\n#endif /* !PORT */\n\nvoid AudioLoad_SyncDma"
        _gdx_audio_rom_reloc "${_gdx_audio_rom_reloc}")
endif()
string(REPLACE
    "#ifndef PORT\n#ifndef PORT\nvoid AudioLoad_RelocateFont"
    "#ifndef PORT\nvoid AudioLoad_RelocateFont"
    _gdx_audio_rom_reloc "${_gdx_audio_rom_reloc}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/load.c" "${_gdx_audio_rom_reloc}")

# 7e) Permanent audio-cache allocator return value. The original decomp function is declared
# void* but falls off the end after filling the cache entry. On the original MIPS ABI the nested
# AudioHeap_Alloc return happened to remain in the return register; on AArch64 this is undefined
# behaviour and was observed returning 0x100000000, which then became the DMA destination and
# crashed in GdxSegmentSourceRead. Return the allocated native pointer explicitly.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/heap.c" _gdx_audio_rom_heap)
string(REPLACE
    "    gPermanentCache.entries[index].tableType = tableType;\n    gPermanentCache.entries[index].id = id;\n    gPermanentCache.entries[index].size = size;\n}"
    "    gPermanentCache.entries[index].tableType = tableType;\n    gPermanentCache.entries[index].id = id;\n    gPermanentCache.entries[index].size = size;\n    return ramAddr;\n}"
    _gdx_audio_rom_heap "${_gdx_audio_rom_heap}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/heap.c" "${_gdx_audio_rom_heap}")

# 7f) N64 sequence-font table endian fix. gSequenceFontTableData is deliberately emitted with S16
# in N64 byte order (e.g. 00 04 for index 4). Casting that byte buffer to u16* on little-endian
# ARM64 turns 0x0004 into 0x0400 and makes the loader walk hundreds of bogus fonts. Route every
# ROM-audio access through an explicit big-endian reader on PORT, including the sequence interpreter.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/audio.h" _gdx_audio_seqfont_header)
string(REPLACE
    "#define AUDIO_MK_CMD(b0,b1,b2,b3) ((((b0) & 0xFF) << 0x18) | (((b1) & 0xFF) << 0x10) | (((b2) & 0xFF) << 0x8) | (((b3) & 0xFF) << 0))"
    "#define AUDIO_MK_CMD(b0,b1,b2,b3) ((((b0) & 0xFF) << 0x18) | (((b1) & 0xFF) << 0x10) | (((b2) & 0xFF) << 0x8) | (((b3) & 0xFF) << 0))\n#ifdef PORT\n#define GDX_AUDIO_SEQFONT_INDEX(seqId) ((u16)((((u16)gSeqFontTable[(seqId) * 2]) << 8) | ((u16)gSeqFontTable[(seqId) * 2 + 1])))\n#else\n#define GDX_AUDIO_SEQFONT_INDEX(seqId) (((u16*)gSeqFontTable)[(seqId)])\n#endif"
    _gdx_audio_seqfont_header "${_gdx_audio_seqfont_header}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/audio.h" "${_gdx_audio_seqfont_header}")

file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/load.c" _gdx_audio_seqfont_load)
string(REPLACE
    "((u16*) gSeqFontTable)[AudioLoad_GetLoadTableIndex(SEQUENCE_TABLE, seqId)]"
    "GDX_AUDIO_SEQFONT_INDEX(AudioLoad_GetLoadTableIndex(SEQUENCE_TABLE, seqId))"
    _gdx_audio_seqfont_load "${_gdx_audio_seqfont_load}")
string(REPLACE
    "((u16*) gSeqFontTable)[seqId]"
    "GDX_AUDIO_SEQFONT_INDEX(seqId)"
    _gdx_audio_seqfont_load "${_gdx_audio_seqfont_load}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/load.c" "${_gdx_audio_seqfont_load}")

file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/seqplayer.c" _gdx_audio_seqfont_player)
string(REPLACE
    "((u16*) gSeqFontTable)[seqPlayer->seqId]"
    "GDX_AUDIO_SEQFONT_INDEX(seqPlayer->seqId)"
    _gdx_audio_seqfont_player "${_gdx_audio_seqfont_player}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/seqplayer.c" "${_gdx_audio_seqfont_player}")

# 7g) ROM audio command queue 64-bit ABI. OSMesg is void* on the host, but the ROM audio
# CreateTask implementation receives into a u32 and MQ_GET_MESG casts &u32 to OSMesg*. On arm64
# osRecvMesg therefore writes 8 bytes into a 4-byte local, corrupting the stack and losing command
# tokens such as INIT_SEQPLAYER. Store the host message at native width and explicitly decode the
# console token afterward.
file(READ "${GDX_DECOMP_DIR}/src/audio/rom/lib/thread.c" _gdx_audio_rom_thread)
string(REPLACE
    "    u32 specId;\n    u32 msg;\n    s32 pad30;"
    "    u32 specId;\n    OSMesg msg;\n    s32 pad30;"
    _gdx_audio_rom_thread "${_gdx_audio_rom_thread}")
string(REPLACE
    "            AudioThread_ProcessCmds(msg);"
    "            AudioThread_ProcessCmds((u32)(uintptr_t)msg);"
    _gdx_audio_rom_thread "${_gdx_audio_rom_thread}")
file(WRITE "${GDX_DECOMP_DIR}/src/audio/rom/lib/thread.c" "${_gdx_audio_rom_thread}")

# 7h) Base-cart Quest audio bootstrap. The original title path starts SEQ_SOUND_EFFECTS from a
# branch coupled to gRamDDCompatible / the title disk-drive state. Quest deliberately forces
# gRamDDCompatible=false to skip unsafe LEO/64DD overlays, so that branch never executes and all
# three sequence players remain disabled forever. Start the master SFX sequence once, after the
# audio heap reset has completed; all normal BGM/SFX commands then flow through that seqplayer.
file(READ "${GDX_DECOMP_DIR}/src/sys/sys_audio.c" _gdx_sys_audio)
set(_quest_audio_bootstrap_old [=[
        if (sCurAudioTask != NULL) {
            gCurAudioOSTask = &sCurAudioTask->task;
            osSendMesg(&gMainThreadMesgQueue, (OSMesg) EVENT_MESG_AUDIO_TASK_SET, OS_MESG_BLOCK);
        }
        sCurAudioTask = Audio_SetupCreateTask();
]=])
set(_quest_audio_bootstrap_new [=[
        if (sCurAudioTask != NULL) {
            gCurAudioOSTask = &sCurAudioTask->task;
            osSendMesg(&gMainThreadMesgQueue, (OSMesg) EVENT_MESG_AUDIO_TASK_SET, OS_MESG_BLOCK);
        }
#ifndef EXPANSION_KIT
#ifdef PORT
        {
            static s32 sGdxQuestMasterSeqStarted = false;
            if (!sGdxQuestMasterSeqStarted && gResetStatus == 0) {
                extern void gdx_dbg_logf(const char* fmt, ...);
                Audio_SESeqStart();
                sGdxQuestMasterSeqStarted = true;
                gdx_dbg_logf("[audio-probe] Quest bootstrapped SEQ_SOUND_EFFECTS after heap reset\n");
            }
        }
#endif
#endif
        sCurAudioTask = Audio_SetupCreateTask();
]=])
string(REPLACE "${_quest_audio_bootstrap_old}" "${_quest_audio_bootstrap_new}" _gdx_sys_audio "${_gdx_sys_audio}")
file(WRITE "${GDX_DECOMP_DIR}/src/sys/sys_audio.c" "${_gdx_sys_audio}")

# 8) Fast3D's OpenGL backend loads its Prism shader template through ResourceManager. A raw ROM
# contains game assets but not libultraship's own shader resource, so embed the pinned upstream
# shader into a tiny generated Quest source. The runtime writes it to an app-private FolderArchive
# before ResourceManager initialization.
file(READ "${gdx_libultraship_SOURCE_DIR}/src/fast/shaders/opengl/default.shader.glsl"
     GDX_QUEST_DEFAULT_SHADER_SOURCE)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/quest_builtin_resources.cpp.in"
    "${GDX_QUEST_GENERATED_DIR}/quest_builtin_resources.cpp"
    @ONLY)

set(GDX_QUEST_GFX_BRIDGE "${GDX_QUEST_GENERATED_DIR}/n64_gfx_bridge_quest.cpp" CACHE INTERNAL "Quest-patched graphics bridge")
set(GDX_QUEST_ASSET_BINDINGS "${GDX_QUEST_GENERATED_DIR}/AssetBindingsQuest.c" CACHE INTERNAL "Quest-patched asset bindings")
set(GDX_QUEST_ASSET_LOADER "${GDX_QUEST_GENERATED_DIR}/AssetLoaderQuest.cpp" CACHE INTERNAL "Quest-patched asset loader")
set(GDX_QUEST_RACE "${GDX_QUEST_GENERATED_DIR}/race_quest.c" CACHE INTERNAL "Quest race/HUD split marker")
set(GDX_QUEST_BACKGROUND "${GDX_QUEST_GENERATED_DIR}/background_quest.c" CACHE INTERNAL "Quest sky/finite-background VR split")
set(GDX_QUEST_CAMERA "${GDX_QUEST_GENERATED_DIR}/camera_quest.c" CACHE INTERNAL "Quest center-head VR camera injection")
set(GDX_QUEST_COURSE "${GDX_QUEST_GENERATED_DIR}/course_quest.c" CACHE INTERNAL "Quest VR omnidirectional course culling")
set(GDX_QUEST_RACER "${GDX_QUEST_GENERATED_DIR}/racer_quest.c" CACHE INTERNAL "Quest VR world-space racer/rival markers")
set(GDX_QUEST_DECOMP_PORT "${GDX_QUEST_GENERATED_DIR}/decomp_port_quest.c" CACHE INTERNAL "Quest stereo texture-cache mirror hooks")
set(GDX_QUEST_SYS_MAIN "${GDX_QUEST_GENERATED_DIR}/sys_main_quest.c" CACHE INTERNAL "Quest-patched base-cart system main")
set(GDX_QUEST_BUILTIN_RESOURCES "${GDX_QUEST_GENERATED_DIR}/quest_builtin_resources.cpp" CACHE INTERNAL "Quest built-in Fast3D resources")
