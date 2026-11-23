# Changelog

All notable changes to **SteelTemple Star Fox** are documented here.

## [0.1.0] - 2026-11-23 (Early Release)

### Added
- **Initial Windows PC Static Recompilation (`Star Fox 1.0.2`):** Statically lifts and compiles `main`, `subsdk0`, `subsdk1`, `subsdk2`, `subsdk3`, and `sdk` into a native x86-64 `starfox.exe` with Vulkan 1.3 rendering and Horizon OS runtime.
- **Full Lighting, Shadows & Indirect/IBL Parity:** Full NVN 2 graphics `c[2]` and compute `c[0]` separate-texture/sampler binding support, volumetric lighting compute storage image handling, and shadow depth-comparison safety for accurate character, cockpit, and environment lighting with zero Vulkan validation errors.
- **True Native 720p–4K Resolution & Live SMAA 1x:** Unlocks full native internal 3D rendering (`Original`, `720p`, `900p`, `1080p`, `1440p`, and native `4K / 2160p`) with automatic physical memory heap scaling and a live 3-pass compute **SMAA 1x** morphological anti-aliasing toggle in the `F1` Graphics menu and `StarFoxLauncher`.
- **Locked 60 FPS Gameplay Performance:** Eliminated per-call virtual memory query syscalls in render config hooks, cached hot environment/operation-mode lookups, and raised the NVN 2 shader control-block size cap so gameplay shaders translate cleanly with zero log spam.
- **Custom Star Fox HUD `F1` Menu, Launcher & Setup Wizard:** Orbital cockpit navy, emerald-cyan, and squadron gold UI theme across the `F1` menu, `StarFoxLauncher.exe`, and native SDL3/ImGui Setup Wizard + uninstaller, complete with original Arwing icon and tactical emblem artwork.
- **Clean Zero-Config Defaults:** Fresh installs always launch at `1920x1080` windowed with safe hardware-agnostic settings; no copyrighted game files, saves, or developer `.ini` files are ever bundled.
