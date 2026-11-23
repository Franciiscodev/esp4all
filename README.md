# ESP4All Core

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Android-ARM64%20(5%E2%80%9317)-brightgreen.svg)](https://developer.android.com/ndk)
[![ImGui](https://img.shields.io/badge/Dear%20ImGui-1.92.7-blueviolet.svg)](https://github.com/ocornut/imgui)
[![Graphics](https://img.shields.io/badge/Graphics-Vulkan%20%7C%20OpenGL%20ES%202%2F3-orange.svg)](https://www.khronos.org/vulkan/)
[![ELF](https://img.shields.io/badge/ELF%20Pages-16%20KB%20Aligned-informational.svg)](jni/Android.mk)
[![Tests](https://img.shields.io/badge/Host%20Tests-375%20Assertions%20%7C%200%20Allocs-success.svg)](tests/test_config_and_templates.cpp)

A modular, config-driven **Android ARM64 (`arm64-v8a`) C++17** Dear ImGui 1.92 overlay and 3D-engine memory translation engine for **Unreal Engine 4** and custom **Y-Up / Row-Major** engines.

Designed with strict separation between **Engine / Memory Translation** and **Rendering**, `esp4all_core` adapts to new targets, builds, and offset layouts via an external hot-reloadable JSONC config — **without recompiling**.

> **Disclaimer:** This project is developed for **educational, reverse-engineering research, and controlled Android graphics/memory instrumentation** purposes on authorized test environments and software you own or have permission to analyze.

---

## Key Features

- **Zero-Recompile Config-Driven Engine (`JSONC`)**
  - Supports `//` and `/* */` comments, arbitrary multi-hop pointer chains (`["0xOFFSET", "0x20", "+0x270"]`), legacy scalar aliases, and VS Code autocomplete via [`esp4all.schema.json`](esp4all.schema.json).
  - Built-in non-throwing schema & semantic validator (`ConfigReport`) catching typos (`W_UNKNOWN_KEY` with Levenshtein suggestions), inactive blocks (`W_UNUSED_KEY`), missing required offsets (`E_REQUIRED_UNSET`), type/enum errors (`E_BAD_TYPE`), scalar/chain conflicts, and accidental `0x0` ELF-header chain starts.

- **Android 5–17 (API 21–37+) Symbol-Probing SurfaceFlinger Layer (`Window`)**
  - Resolves `libgui.so` / `libutils.so` entry points **by actual symbol presence** (14 `createSurface` / `createSurfaceChecked` variants ordered newest → oldest, including Android 16 QPR / **Android 17** `const android::gui::LayerMetadata&` and `enum TrustedOverlay` overloads) instead of fragile Android version switches.
  - Hardened ABI bridge: starts `ABinderProcess_startThreadPool()`, prefers public NDK `ASurfaceTransaction_create()`, guards in-place allocations with a **32-byte `0xA5` canary**, validates `android_native_base_t::magic` (`'_wnd'`), and pins **16 KB ELF page alignment** (`-Wl,-z,max-page-size=16384`).
  - **3 Capture Modes:** `Visible` (normal), `Hidden` (`eSkipScreenshot` / `LayerMetadata` window type, Android 8.1–17), and **`Secure`** (`FlagSecure = 0x80`, supported all the way down to Android 5–8.0).

- **Dear ImGui 1.92.7 + Native Vulkan & OpenGL ES 3/2 Backends (`1.3 MB` Lean Binary)**
  - Clean Dear ImGui `v1.92.7` with dynamic font atlas (`ImGuiBackendFlags_RendererHasTextures` / `ImTextureData`), Android system TTF loading (`/system/fonts/Roboto*`), real display DPI scaling (`FontScaleDpi`), touch-friendly UX (`ImGuiConfigFlags_IsTouchScreen`, `ConfigWindowsMoveFromTitleBarOnly`), and **0 compiler warnings** across the entire build.
  - Self-contained **Vulkan 1.0** backend loaded dynamically via `dlopen("libvulkan.so")` (`VK_NO_PROTOTYPES` — zero link-time `-lvulkan` or `-lGLESv3` dependency) with automatic fallback to **OpenGL ES 3 / ES 2** (`highp` UV precision, premultiplied alpha output for SurfaceFlinger composition).

- **Multi-Protocol Touch & Optional `/dev/uinput` Control Mode (`Input`)**
  - Single cleanly joinable `poll()` + `inotify` worker thread (0% idle CPU, automatic hotplug, zero detached threads) supporting **Multitouch Protocol B** (slotted, palm rejection), **Protocol A** (tracked & untracked frames), **Single-touch**, **emulator absolute pointers/wheel**, `.idc` configuration files, and all **4 display rotations** (`0°/90°/180°/270°`).
  - **Touch Intercept (`/dev/uinput` Control Mode):** optional toggle in `DebugUI` that grabs the physical touchscreen only when idle, forwards outside/in-game fingers through a virtual touchscreen, and keeps UI touches strictly inside Dear ImGui — **menu taps no longer click through to the game underneath**, and in-game aim drags never pull the menu. Safely rewinds axes (`AxisState::Rewind`) and releases `EVIOCGRAB` on disable or `SIGINT`/`SIGTERM`, falling back to `Read-Only` if `/dev/uinput` is unavailable.
  - Built-in **inertial touch scrolling** and **window confinement** within live display bounds.

- **Frame Efficiency & Seamless Lifecycle**
  - Allocates the SurfaceFlinger layer at **exact display dimensions** (eliminating `max(w, h)²` square swapchain memory waste).
  - Live display rotation/resize (`Resize()` + `Rebind()`) and live `CaptureMode` switching (`Visible` / `Hidden` / `Secure`) **preserve the ImGui context** (open headers, window positions, and scroll state stay intact).
  - **FNV-1a Dirty-Frame Idle Skip & Empty-Layer Hide:** when ESP is paused and the UI is static, skips redundant GPU submissions and wakes in `< 1 ms` on touch input; when ESP is running, always submits immediately for minimum latency.
  - **Dynamic `AChoreographer` VSync Pacing:** aligns frame submission to native display refresh cadence via `libandroid.so` (Android 7+) when `Target FPS` matches or exceeds panel refresh, falling back to precision `timer.h` pacing otherwise.

- **Multiple Entity Providers, Zero-Allocation Pool & Dual-Mode (External + Internal `.so`) Architecture**
  - `WorldActors` (UE4 `UWorld → PersistentLevel / Levels[] → TArray<AActor*>`), `WorldActors` (`EntityList_chain` for custom Y-Up / Row-Major / `end_ptr` / `null_terminated` engines), and `GameStatePlayer` (`GameState → PlayerArray → player_state.PawnPrivate`).
  - **NetDriver LocalPlayer Chain (External & Internal):** supports standard UE4 `UWorld → NetDriver → ServerConnection → PlayerController → AcknowledgedPawn` (`world.NetDriver` + `local_player.NetDriver_ServerConnection`), resolving `LocalController`, `LocalPawn`, and `PlayerCameraManager` even when `OwningGameInstance → LocalPlayers` is encrypted or unavailable.
  - Pre-allocated lock-free `DoubleBufferedEntityPool` (256 slots), canonical ARM64 TBI/MTE pointer tag stripping, optional single-syscall **15-bone `FTransform` Skeleton ESP**, and **0 heap allocations** per frame in `EspEngine::Update()`.
  - **Dual Build Targets sharing `libesp4all_core.a`:**
    1. **External Executable (`libs/arm64-v8a/core`):** standalone process using `MemoryTool` (`process_vm_readv` / `pread64` / kernel driver).
    2. **Data-Driven Internal Shared Library (`libs/arm64-v8a/libesp4all_internal.so`):** in-process host using `DirectMemoryReader` (`SAFE_SELF_VM` SIGSEGV-proof default / `DIRECT_MEMCPY`) + `InternalEngineBridge`, capable of invoking optional JSON-configured native UE4 getters (`internal.GNames_getter`, `internal.GetActorArray_fn`) and resolving `GameViewport → World` via `internal.GUObjectArray` while remaining **100% generic and SDK-free**.

---

## Architecture

```text
Main.cpp (SIGINT / SIGTERM clean teardown)
  └── OverlayApp (pure composition root)
        ├── ImGuiCfg        (Surface lifecycle, Rebind, FNV-1a idle skip, AChoreographer VSync)
        │     ├── Window    (LibGui symbol-probing Android 5–17 + RAII NativeWindow + CaptureMode)
        │     ├── Graphics  (GraphicsManager + IGraphicsBackend [Vulkan 1.0 | OpenGL ES 3/2] + FrameHasher)
        │     └── Input     (TouchManager poll/inotify + TouchDevice/Parser/Router + uinput + inertial scroll)
        ├── MemoryTool      (IMemoryReader: process_vm_readv | pread64 | kernel driver)
        ├── EspSettings     (Versioned binary UI settings v5 with automatic v2/v3/v4 migration)
        ├── DebugUI         (Config Validation + Live Chain Debugger + Capture/Touch/Gfx controls)
        └── EspEngine       (Platform-agnostic — compiled as reusable libesp4all_core.a)
              ├── ConfigManager   (nlohmann/json JSONC loader + ConfigReport validator + DumpEffective)
              ├── UE4Context      (Per-frame UWorld/Camera/Names/Filters/Bones + InternalEngineBridge)
              └── IEntityProvider
                    ├── WorldActorsEntityProvider     (UWorld -> PersistentLevel/Levels[] or EntityList_chain)
                    └── GameStatePlayerEntityProvider (UWorld -> GameState -> PlayerArray -> PawnPrivate)
```

---

## Templates (`templates/`)

Start with the minimal template that matches your target's provider — every template is lint-checked and verified end-to-end against synthetic ARM64 memory:

| Template | Provider / Mode | Minimum Offsets | Best For |
|---|---|---|---|
| [`templates/minimal_worldactors.jsonc`](templates/minimal_worldactors.jsonc) | `WorldActors` (UE4) | **6** (`GWorld`, `PersistentLevel`, `LevelActors`, `RootComponent`, `WorldPosition`, `view_matrix_chain`) | Fastest bring-up to draw Boxes in standard UE4 (includes commented `CameraCache` + `heuristic_pc_scan` alternative) |
| [`templates/minimal_gamestateplayer.jsonc`](templates/minimal_gamestateplayer.jsonc) | `GameStatePlayer` (UE4) | **7** (`GWorld`, `GameState`, `PlayerArray`, `player_state.PawnPrivate`, `RootComponent`, `WorldPosition`, `view_matrix_chain`) | Clean player-only iteration without props/trees or class filters |
| [`templates/minimal_entitylist_yup.jsonc`](templates/minimal_entitylist_yup.jsonc) | `WorldActors` (`EntityList_chain`) | **4–5** (`EntityList_chain`, `LevelActors` opt., `RootComponent`, `WorldPosition`, `view_matrix_chain`) | Non-UE4 engines (`Y-Up`, `row_major`, `end_ptr` / `null_terminated`, meters) |
| [`templates/full_worldactors.jsonc`](templates/full_worldactors.jsonc) | `WorldActors` (UE4 Full) | Full | `FNamePool` + `class_private` + `local_player` + dual `auto` camera + attributes + commented `GNames`, `sentinel`, `bones` |
| [`templates/full_gamestateplayer.jsonc`](templates/full_gamestateplayer.jsonc) | `GameStatePlayer` (UE4 Full) | Full | Full UE4 `GameStatePlayer` cleanly separating `actor` and `player_state` attributes |

---

## Building

### Prerequisites
- **Android NDK r26+** (verified with NDK r28, `arm64-v8a`, `android-21`, `c++_static`, 16 KB ELF alignment)
- **Host compiler (`clang++` / `g++` with C++17)** for running the unit & golden test suite on macOS/Linux

### 1. Build Android ARM64 Binaries & Static Library
```bash
$ANDROID_NDK_HOME/ndk-build -B NDK_PROJECT_PATH=. -j4
```
Outputs (all 0 warnings, 16 KB ELF aligned, zero `libvulkan.so` / `libGLESv3.so` ELF deps):
- External standalone overlay executable (`~1.3 MB`): `libs/arm64-v8a/core`
- Data-driven internal shared library (`~1.7 MB`): `libs/arm64-v8a/libesp4all_internal.so`
- Reusable platform-agnostic static library: `obj/local/arm64-v8a/libesp4all_core.a`

### 2. Run Host C++17 Test Suite (No Device Required)
Validates all 5 `.jsonc` templates on disk, synthetic ARM64 memory golden tests (`drawnPlayers > 0`, **0 heap allocations** on frame 2+), negative validator rules, pool overflow (`droppedByPoolCap`), `InternalEngineBridge`, `EspSettings` v2/v3/v4→v5 migration, `TouchDevice` / `TouchParser` (Protocol B, A, Single, `VirtualFingers`) / `TouchRouter` / `EventQueue`, and `FrameHasher`:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -Ijni/include \
  tests/test_config_and_templates.cpp \
  jni/src/UE4/UE4Context.cpp \
  jni/src/ESP/WorldActorsEntityProvider.cpp \
  jni/src/ESP/GameStatePlayerEntityProvider.cpp \
  jni/src/ESP/EspEngine.cpp \
  jni/src/Input/TouchDevice.cpp \
  jni/src/Input/TouchParser.cpp \
  jni/src/Input/TouchRouter.cpp \
  -o /tmp/esp4all_test && /tmp/esp4all_test
```

---

## Quick Start on Android Device

1. Pick a minimal template from [`templates/`](templates/) and fill in the `null` / `[]` placeholders for your test target.
2. Push the binary and config to the device (comments in `.jsonc` are natively supported by `ConfigManager` — rename to `ue4_config.json` on device):
   ```bash
   adb push libs/arm64-v8a/core /data/local/tmp/esp4all_core
   adb push templates/minimal_worldactors.jsonc /data/local/tmp/ue4_config.json
   adb shell "su -c 'chmod +x /data/local/tmp/esp4all_core && /data/local/tmp/esp4all_core'"
   ```
3. Open **ESP4All Core — Chain Debugger**:
   - Check **Config Validation** for any `E_REQUIRED_UNSET`, `E_BAD_TYPE`, or `W_*` diagnostics.
   - Verify each pointer chain link top-to-bottom (`PID → Base → UWorld → Level/GS → Actors → Camera`).
   - Edit `/data/local/tmp/ue4_config.json` and tap **Reload JSON** for instant hot-reload.
   - Switch **Capture** (`Visible` / `Hidden` / `Secure`), toggle **Touch Intercept (uinput)**, or **Switch Gfx (`Vulkan` / `OpenGL ES`)** live in the panel.

---

## Documentation

| Document | Language | Contents |
|---|---|---|
| [`documentation.md`](documentation.md) | PT-BR | Complete user/developer guide: JSONC reference, units & defaults, `PositionSource`, UE4 SDK offset mapping, and Chain Debugger troubleshooting |
| [`context.md`](context.md) | EN | Architectural invariants, directory/module map, and build/test commands for contributors & agents |
| [`future_internal_and_symbols.md`](future_internal_and_symbols.md) | PT-BR | Technical roadmap for UE4 dump symbols (`Vworld`, `GetActorArray`/`GetXX`, `ProcessEvent`) and in-process `InternalEngineBridge` extensions |
| [`plano_melhorias_new2.md`](plano_melhorias_new2.md) | PT-BR | Completed 4-phase engineering plan covering Android 5–17, ImGui 1.92.7, multi-protocol Touch + `uinput`, and frame efficiency |
| [`esp4all.schema.json`](esp4all.schema.json) | EN | Draft-07 JSON Schema for VS Code autocomplete and validation |

---

## Project Layout

```text
esp4all_core/
├── README.md
├── documentation.md
├── context.md
├── future_internal_and_symbols.md
├── plano_melhorias_new2.md
├── esp4all.schema.json
├── templates/                   # 3 minimal (per provider/mode) + 2 full clean .jsonc templates
├── tests/                       # Host C++17 test suite (337 assertions, synthetic ARM64 memory, 0-alloc)
└── jni/
    ├── Android.mk / Application.mk
    ├── include/
    │   ├── Config/              # ConfigManager (JSONC + ConfigReport + DumpEffective)
    │   ├── Window/              # CaptureMode, LibGui (Android 5–17 symbol probing), NativeWindow + AChoreographer
    │   ├── Graphics/            # GraphicsManager, IGraphicsBackend, Vulkan 1.0 & OpenGL ES 3/2, FrameHasher
    │   ├── Input/               # TouchDevice, TouchParser (B/A/Single), TouchRouter, TouchManager (uinput + scroll)
    │   ├── Draw/                # ImGuiCfg (exact-size surface, live Rebind, idle skip, VSync pacing)
    │   ├── ImGui/               # Clean Dear ImGui 1.92.7 core + imconfig.h
    │   ├── UE4/                 # UE4Context, UE4Math, UE4NameResolver, UE4Structures, InternalBridge
    │   ├── ESP/                 # EspEngine, EspRenderer, EspSettings (v5), EntityProviders, EntityPool
    │   └── UI/                  # DebugUI (Config Validation + Live Chain Debugger)
    └── src/                     # Implementation files
```

---

## License

See repository license terms. Third-party components and architectural inspirations retain their original licenses:
- **Dear ImGui** (MIT License)
- **nlohmann/json** (MIT License)
- SurfaceFlinger symbol-probing, ImGui 1.92 backend, and evdev/`uinput` touch designs inspired by open-source Android overlay research (`android-imgui`).
