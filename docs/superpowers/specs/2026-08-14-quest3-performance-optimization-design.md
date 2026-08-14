# Quest 3 Performance Optimization Design

## Goal

Reach a stable 72 FPS target on Quest 3 (13.89 ms frame budget) during races without reducing the current visual quality of the F-Zero X VR standalone port.

## Non-negotiable constraints

- Do not reduce the current eye render resolution or `kWorldRenderScale`.
- Do not remove or downgrade skybox, fog/background treatment, clouds, HUD, race markers, textures, geometry, effects, draw distance, 6DoF, stereo projection, diorama mode, audio, or controller behavior.
- Do not change gameplay timing or the original ~60 Hz game simulation cadence.
- Preserve the current OpenXR refresh configuration and existing foveation settings; performance work must come from code/pipeline efficiency.
- Preserve the current fixes for Mute City 1 background, cached eye pose/timewarp stability, GP startup lifecycle, wrist HUD, and race marker orientation.

## Phase 1: Low-risk structural optimization

### 1. Activate the existing VR course culling implementation

`GDiffuserQuestPatches.cmake` already generates `course_quest.c`, including the Quest-specific course chunk visibility path, but the current Ninja graph still compiles upstream `src/game/course.c`.

Change the game source list so upstream `course.c` is excluded and `${GDX_QUEST_COURSE}` is compiled instead. This must preserve the same visible course geometry while avoiding work for chunks that cannot contribute to either VR eye.

Verification: inspect `build.ninja` after CMake regeneration and confirm `gdx_quest_generated/course_quest.c.o` is present while `_deps/gdx_decomp-src/src/game/course.c.o` is absent.

### 2. Optimize hot native targets at compile time

Fast3D/libultraship already compiles at `-O3`; `gdiffuser_game` and the host currently compile at `-O2` in the debug APK. Move only performance-critical targets to `-O3` while retaining debug symbols and existing safety flags (`-fno-strict-aliasing`, `-fwrapv` where already required).

No functional or visual behavior may change from this step.

### 3. Remove hot-path diagnostic overhead

Identify debug logging/probes that can execute every frame, every graphics task, every audio command, or every eye. Keep startup, lifecycle, error, and low-frequency performance logs, but compile out or rate-limit per-frame diagnostic probes in normal Quest builds.

The known audio probe and Fast3D section tracing are candidates. Removing logging must not change game state or rendering.

### 4. Remove only redundant GPU synchronization

Audit `glFlush()` calls in `renderer_gles.cpp` and `openxr_context.cpp`. Preserve every synchronization required before releasing OpenXR swapchain images or before another context/thread consumes results. Remove only flushes that are immediately followed by another guaranteed flush/swapchain release synchronization and therefore cannot affect image correctness.

This task requires before/after runtime validation because an incorrect removal can create stale or incomplete frames.

### 5. Cache eye-invariant calculations

In `gdiffuser_vr_host.cpp`, identify calculations repeated once per eye that depend only on the center game camera, game frame, or shared OpenXR frame state. Compute those once per game graphics task/frame and reuse them for left/right eye matrix construction. Keep all eye-dependent pose, IPD, asymmetric projection, and cached render-pose data separate.

Do not cache anything whose value can differ between the two eyes.

## Phase 2: Deeper stereo/Fast3D optimization only if needed

Phase 2 starts only if Phase 1 does not produce stable 72 FPS in representative races.

Profile the Fast3D stereo replay and separate display-list work into eye-invariant command/state preparation versus eye-dependent matrix/vertex projection and draw submission. Reuse invariant decoded state between eyes without changing the resulting draw order or pixels.

Single-pass multiview is explicitly deferred until after this analysis because it is a larger renderer architecture change and carries higher compatibility risk.

## Measurement and acceptance

Collect Quest runtime performance using the existing `FZeroXVR/Perf` telemetry before and after each meaningful optimization. Record OpenXR FPS and average render-frame time.

Acceptance criteria:

- Target: stable 72 FPS during representative races on Quest 3.
- Frame budget: average render time at or below 13.89 ms, with reduced spikes.
- No intentional visual-quality reduction.
- No regression in Mute City 1 sky/background, race markers, HUD, diorama, 6DoF/head translation, cached eye pose/timewarp behavior, audio, input, GP startup, or clean application lifecycle.
- Full Android arm64 debug build succeeds after each source-integration change.
- Final APK installs successfully on the connected Quest 3.

## Rollback policy

Each optimization must be independently testable. If a change causes a visual, lifecycle, audio, input, or stability regression, revert that optimization rather than compensating with a quality reduction.
