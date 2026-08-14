# F-Zero X VR Standalone — v1.0.0

First public standalone Quest release of F-Zero X VR.

## Highlights
- Native standalone Quest 3 build
- Native OpenXR stereoscopic rendering
- 6DoF head tracking
- Quest Touch controller support
- In-headset launcher with ROM selection
- Configurable refresh rate
- Optional diorama mode
- Wrist-mounted race HUD
- Native AAudio output
- Quest foveated rendering support
- 360-degree VR sky/background rendering

## VR fixes included in 1.0.0
- Corrected stereo/world projection and removed duplicate 4:3 aspect correction in VR
- Corrected render-pose reuse for OpenXR timewarp to remove terrain shaking during head turns
- Corrected race markers such as 1st / 2nd / 3rd / CHECK
- Removed the legacy camera-facing sky quad that appeared as a white wall in Mute City
- Improved launcher/VR process separation and OpenXR session lifecycle handling

## ROM requirement
A legally obtained F-Zero X (USA) Rev 0 Nintendo 64 ROM is required.
The launcher validates the ROM before use.

Expected SHA-1:
`5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`

The ROM is not included with this release.

## Installation
1. Install the APK on a Quest 3 with developer mode enabled.
2. Launch F-Zero X VR.
3. Select your F-Zero X (USA) Rev 0 ROM in the launcher.
4. Press **Launch F-Zero X VR**.

## Notes
This is a community VR port and is not affiliated with Nintendo.
