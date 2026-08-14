# F-Zero X VR Standalone — v1.0.1

Performance and VR stability update for the standalone Quest 3 port.

## Highlights
- Default refresh rate changed to 90 Hz
- Major CPU-side performance cleanup without lowering visual quality
- Improved Quest VR course culling
- Fixed diorama course geometry disappearing
- Improved diorama sorting for overlapping course sections
- OpenXR eye resolution remains locked to the validated 2800x2933 quality floor when supported

## Performance improvements
- Enabled the Quest-specific course culling path that was previously generated but not compiled
- Cached HMD/frustum calculations instead of recomputing them for each course chunk
- Built the VR host, game hot paths, Fast3D and cxd4 RSP audio core with stronger optimization
- Removed high-frequency audio and graphics diagnostic logging from production hot paths
- Removed unused per-frame development gate refresh work
- Removed geometry/UV/material diagnostic bookkeeping executed per vertex and per triangle
- Removed old per-command conversion counters and diagnostic probes
- Reduced redundant OpenGL state setup and cached HUD shader uniform locations
- Removed unnecessary display-list copying before HUD rendering
- Cached OpenXR FOV projection terms and shared camera basis calculations between WORLD and SKY

These changes are code optimizations only. They do not intentionally reduce texture quality, geometry quality, effects, HUD quality, draw distance, or headset render resolution.

## Diorama fixes
- Diorama now keeps the complete streamed course chunk ring to prevent track/ground sections from disappearing when viewed from arbitrary angles
- Expanded the Quest diorama chunk-group capacity so full-ring sorting cannot abort course rendering on large tracks
- Kept the diorama-specific ordering path for overlapping/stacked track sections

## OpenXR / display
- 90 Hz is now the default refresh rate for new configurations
- 72 Hz, 80 Hz, 90 Hz and 120 Hz remain selectable in VR settings
- Projection swapchains keep a 2800x2933 per-eye quality floor when allowed by the OpenXR runtime
- HUD compositor remains 2048x1536
- Quest fixed foveated rendering remains HIGH + dynamic

## ROM requirement
No ROM is included. The application requires a legally obtained, unmodified 16 MiB US Rev 0 version of F-Zero X in `.z64`, `.n64`, or `.v64` format. Other regions, revisions, or ROM hacks are not supported and may not work properly.

Expected SHA-1:
`5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`

## Installation
1. Install the APK on a Quest 3 with developer mode enabled.
2. Launch F-Zero X VR.
3. Select your F-Zero X (USA) Rev 0 ROM in the launcher.
4. Press **Launch F-Zero X VR**.

## Notes
This is a community VR port and is not affiliated with Nintendo.
