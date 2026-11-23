# Technical Context — `esp4all_core` (Agent Handoff)

> **Purpose:** Concise architectural reference, invariants, build/test commands, and roadmap for future engineers and AI agents working on `esp4all_core`.

---

## 1. Project Summary

`esp4all_core` is a modular, config-driven Android ARM64 (`arm64-v8a`) C++17 Dear ImGui overlay and external UE4 / 3D-engine memory translation engine. All 4 refactoring phases from [`full_audit_report.md`](full_audit_report.md) and the template/validator overhaul from [`plano_de_melhoria_v2.md`](plano_de_melhoria_v2.md) are **complete** and verified against automated C++17 host tests (240 assertions in [`tests/test_config_and_templates.cpp`](tests/test_config_and_templates.cpp)) and clean Android NDK r28 builds (`-Wall -Wextra`, 0 warnings in all project files).

- **Target ABI / Platform:** `arm64-v8a`, `android-21`, `APP_STL := c++_static` ([`jni/Application.mk`](jni/Application.mk)).
- **Runtime Artifacts on Device:**
  - Executable: `/data/local/tmp/esp4all_core` (compiled to `libs/arm64-v8a/core`)
  - JSON config (hot-reloadable): `/data/local/tmp/ue4_config.json`
  - UI settings binary: `/data/local/tmp/esp4all_ui.cfg` (`EspSettings::MAGIC = 0x45535034`, `VERSION = 3`, backwards-compatible with `VERSION = 2`)

---

## 2. Directory & Module Map

```text
esp4all_core/
├── documentation.md                        # User/developer JSONC, schema semantics & UE4 SDK offset guide
├── context.md                              # This technical handoff document
├── full_audit_report.md                    # Historical audit & 4-phase refactoring plan (all done)
├── plano_de_melhoria_v2.md                 # Templates, validator & test suite plan (v3, all 4 phases done)
├── future_internal_and_symbols.md          # Technical roadmap: UE4 dump symbols (Vworld/GetXX/ProcessEvent) & internal mode extensions
├── esp4all.schema.json                     # Draft-07 JSON Schema for VS Code autocomplete/validation
├── templates/
│   ├── minimal_worldactors.jsonc           # Minimal 6-offset UE4 WorldActors template (Box only + CameraCache comment)
│   ├── minimal_gamestateplayer.jsonc       # Minimal 7-offset UE4 GameStatePlayer template (player_state.PawnPrivate)
│   ├── minimal_entitylist_yup.jsonc        # Minimal non-UE4 EntityList_chain + Y-Up + row_major + end_ptr template
│   ├── full_worldactors.jsonc              # Clean full UE4 WorldActors template (FNamePool, ClassPrivate, optional Bones)
│   └── full_gamestateplayer.jsonc          # Clean full UE4 GameStatePlayer template (player_state.* attributes)
├── tests/
│   └── test_config_and_templates.cpp       # Host C++17 test suite (template lint, 5 golden synthetic images, 0-alloc, pool cap)
└── jni/
    ├── Android.mk                          # -std=c++17 -Wall -Wextra -fno-rtti -fexceptions
    ├── Application.mk                      # arm64-v8a, android-21, c++_static
    ├── include/
    │   ├── IMemoryReader.hpp               # IMemoryReader interface + MemoryUtils (TBI, IsValidPtr, UTF-16, maps parser)
    │   ├── Memory.hpp                      # External backends: ProcessVmMemoryReader (default), ProcMemReader, KernelDriverReader, MemoryTool
    │   ├── DirectMemoryReader.hpp          # Internal/in-process IMemoryReader (DIRECT_MEMCPY or SIGSEGV-safe SAFE_SELF_VM + /proc/self/maps)
    │   ├── OverlayApp.hpp                  # Top-level composition root (replaces legacy MyDraw)
    │   ├── OnlyTouch.h                     # Non-blocking /dev/input multitouch reader -> lock-free TouchSync::Pack
    │   ├── ThirdParty/json.hpp             # nlohmann/json v3.11.3 (header-only)
    │   ├── Config/ConfigManager.hpp        # Non-throwing JSON/JSONC loader, pointer-chain parser, ConfigReport validator, DumpEffective
    │   ├── Draw/draw.h                     # ImGuiCfg (SurfaceFlinger Android 5–16 + IGraphicsBackend + TouchSync + RecreateOverlay)
    │   ├── Graphics/
    │   │   ├── GraphicsBackend.hpp         # GraphicsBackendType (Auto/Vulkan/OpenGL) + IGraphicsBackend interface
    │   │   ├── GraphicsManager.hpp         # Factory selecting VulkanGraphicsBackend or OpenGLGraphicsBackend
    │   │   ├── OpenGLGraphicsBackend.hpp   # EGL / OpenGL ES 3 backend
    │   │   ├── VulkanGraphicsBackend.hpp   # Dynamic libvulkan.so backend (non-aborting, transparent overlay)
    │   │   └── vulkan_wrapper.h            # Dynamic Vulkan function loader (VK_NO_PROTOTYPES)
    │   ├── Fun/timer.h                     # Monotonic FPS limiter & CPU affinity
    │   ├── UE4/
    │   │   ├── UE4Structures.hpp           # Vec3, FQuat, FTransform (0x30), BoneSlot(15), PointerChain, ChainDiagnostics (+ droppedByPoolCap)
    │   │   ├── UE4Math.hpp                 # WorldToScreenMatrix (Col/Row), WorldToScreenTrig (Z/Y-Up), FTransform bone math
    │   │   ├── UE4NameResolver.hpp         # Cached INameResolver + FNamePoolResolver (stride*2) + GNamesResolver
    │   │   ├── InternalBridge.hpp          # Lock-free SeqLock InternalEngineBridge (pushed Camera/World/LocalPawn/UClass* overrides)
    │   │   └── UE4Context.hpp              # Unified UE4 Translation Layer (single source of truth)
    │   ├── ESP/
    │   │   ├── IEntityProvider.hpp         # PooledEntity, EntityPool (256), DoubleBufferedEntityPool, Entity, IEntityProvider
    │   │   ├── WorldActorsEntityProvider.hpp
    │   │   ├── GameStatePlayerEntityProvider.hpp
    │   │   ├── EspEngine.hpp               # Platform-agnostic engine (Context + Provider + EntityPool + Stats w/ droppedByPoolCap)
    │   │   ├── EspRenderer.hpp             # Pure ImGui renderer over pre-projected EntityPool
    │   │   └── EspSettings.hpp             # Versioned binary UI settings
    │   └── UI/DebugUI.hpp                  # Live Config Validation, Chain Debugger & control panel
    └── src/
        ├── Main.cpp                        # Minimal 9-line entry point instantiating OverlayApp
        ├── OverlayApp.cpp                  # Composition root; ONLY TU that includes <OnlyTouch.h>
        ├── Draw/draw.cpp                   # ImGuiCfg + ANativeWindowCreator (Android 5–16, skipScreenshot) + Vulkan->OpenGL fallback
        ├── Graphics/
        │   ├── GraphicsManager.cpp
        │   ├── OpenGLGraphicsBackend.cpp
        │   ├── VulkanGraphicsBackend.cpp
        │   └── vulkan_wrapper.cpp
        ├── UE4/UE4Context.cpp              # Per-frame resolution, heuristic bootstrap, batch Bone ESP
        ├── ESP/EspEngine.cpp               # Frame Update() + per-bone Z-clipped W2S + external ConfigReport gate
        ├── ESP/WorldActorsEntityProvider.cpp
        ├── ESP/GameStatePlayerEntityProvider.cpp
        └── UI/DebugUI.cpp
```

---

## 3. Architectural Invariants (DO NOT BREAK)

1. **Pure Composition — No Diamond Inheritance:**
   - `OverlayApp` composes `ImGuiCfg`, `MemoryTool`, `EspEngine`, `DebugUI`, and `EspSettings`.
   - `EspEngine`, `UE4Context`, `ConfigManager`, `IMemoryReader`, and both `IEntityProvider`s have **zero dependency** on Android EGL, `ANativeWindow`, or ImGui — keep them 100% host-compilable and unit-testable with `clang++`.

2. **Zero-Allocation Frame Loop:**
   - After frame 1 warmup (FName/ClassPrivate cache population), `EspEngine::Update()` MUST perform **0 heap allocations** (verified by overriding `operator new` in host tests).
   - Never add `std::string` or dynamic containers to `PooledEntity`; keep fixed `char[]` / `std::array` buffers and reuse member buffers (`levelBuffer_`, `ptrBuffer_`, `boneRawBuf_`).
   - Always use `ReadObjectNameInto()`, `ReadUtf16StringInto()`, `ReadAnsiStringInto()`, and `INameResolver::ResolveInto()` on hot paths.

3. **ARM64 Memory & Pointer Safety:**
   - Every pointer read from target memory MUST go through `IMemoryReader::ReadPointer()` (which strips ARM64 TBI/MTE top byte `& 0x00FFFFFFFFFFFFFFULL`) and `MemoryUtils::IsValidPtr()` (`0x10000000 .. 0x7FFFFFFFFFFF`).
   - Never return uninitialized stack memory on failed reads — use `ReadInto<T>()` or zero-initialized `Read<T>()`.
   - `MemoryTool` defaults to `MemoryAccessMode::PROCESS_VM_READV` (`SYS_process_vm_readv = 270` on `aarch64`), with `ProcMemReader` (`pread64`) and `KernelDriverReader` (`ret == 0`) as runtime fallbacks.
   - `GetBaseAddress()` defaults to `"r-xp"` (lowest executable segment in `/proc/<pid>/maps`), configurable via `game.segment` / `lib.segment`.

4. **Dynamic Per-Frame World Resolution:**
   - `UE4Context::BeginFrame()` re-resolves `UWorld` (GWorld chain → GEngine fallback), `PersistentLevel`, `GameState`, `LocalController`, `LocalPawn`, and `Camera` **every frame**. Never cache `UWorld*` or `LocalPawn*` permanently across Lobby ↔ Match transitions.
   - `UGameInstance::LocalPlayers` is an inline `TArray<ULocalPlayer*>` — reading `LocalPlayers[0]` takes **2 dereferences**, not 3.

5. **Overlay Concurrency & Lifecycle:**
   - **No `dumpsys` background thread:** Display rotation/dimensions are queried via `ANativeWindowCreator::GetDisplayInfo()` every 500 ms on the main render thread and mirrored to `std::atomic<int>`.
   - **No direct `ImGui::GetIO()` writes from touch thread:** `OnlyTouch.h` packs `(x, y, isDown, valid)` into a single lock-free `std::atomic<uint64_t>` (`TouchSync::Pack`/`Unpack`), consumed by `ImGuiCfg::applyPendingTouchInput()` on the render thread right before `ImGui::NewFrame()`.
   - **Single-TU rule for `OnlyTouch.h`:** `OnlyTouch.h` defines `ImGuiCfg` touch methods inline in a header; include it ONLY in [`jni/src/OverlayApp.cpp`](jni/src/OverlayApp.cpp).
   - **Clean shutdown:** `StopTouch()` uses `poll(50ms)` + `touchThread_.join()`, and `shutdown()` MUST call both `ANativeWindow_release()` and `ANativeWindowCreator::Destroy()`.
   - Surface size is `max(w, h) × max(w, h)` (not `(w + h)²`).

6. **W2S & Bone Projection:**
   - Every `WorldToScreenMatrix()` (`w < 0.001f`) and `WorldToScreenTrig()` (`localZ < 1.0f`) call enforces explicit Z-clipping.
   - Bones MUST be projected individually per bone (never reuse the root actor's `w`).

7. **Clean Build Hygiene:**
   - `jni/Android.mk` uses `-std=c++17 -Wall -Wextra` (no `-w`, no `-fpermissive`). All project files under `jni/src/` and `jni/include/` (excluding third-party `ImGui/` and `ThirdParty/`) MUST build with **0 warnings**.
   - Never put `using namespace std;` in headers.

---

## 4. Build & Verification Commands

### Android NDK (`arm64-v8a`) Build
```bash
# Incremental build
/Users/frank/Library/Android/sdk/ndk/28.2.13676358/ndk-build \
  -C /Users/frank/Documents/android_imgui/esp4all_core/jni \
  NDK_PROJECT_PATH=/Users/frank/Documents/android_imgui/esp4all_core \
  NDK_APPLICATION_MK=/Users/frank/Documents/android_imgui/esp4all_core/jni/Application.mk \
  -j4

# Full clean rebuild (-B)
/Users/frank/Library/Android/sdk/ndk/28.2.13676358/ndk-build -B \
  -C /Users/frank/Documents/android_imgui/esp4all_core/jni \
  NDK_PROJECT_PATH=/Users/frank/Documents/android_imgui/esp4all_core \
  NDK_APPLICATION_MK=/Users/frank/Documents/android_imgui/esp4all_core/jni/Application.mk \
  -j4
```
Output binary: `esp4all_core/libs/arm64-v8a/core` (`ELF 64-bit LSB pie executable, ARM aarch64`).

### Host C++17 Unit & Warning Check (macOS / Linux)
Run the persisted config validator, template lint, golden synthetic memory, pool overflow, and 0-allocation test suite:
```bash
clang++ -std=c++17 -Wall -Wextra -Werror \
  -I/Users/frank/Documents/android_imgui/esp4all_core/jni/include \
  /Users/frank/Documents/android_imgui/esp4all_core/tests/test_config_and_templates.cpp \
  /Users/frank/Documents/android_imgui/esp4all_core/jni/src/UE4/UE4Context.cpp \
  /Users/frank/Documents/android_imgui/esp4all_core/jni/src/ESP/WorldActorsEntityProvider.cpp \
  /Users/frank/Documents/android_imgui/esp4all_core/jni/src/ESP/GameStatePlayerEntityProvider.cpp \
  /Users/frank/Documents/android_imgui/esp4all_core/jni/src/ESP/EspEngine.cpp \
  -o /tmp/esp4all_test && /tmp/esp4all_test
```

---

## 5. Known Boundaries & Potential Future Extensions

1. **Unreal Engine 5 (Large World Coordinates — `double` precision):**
   - Current `Vec3`, `FQuat`, and `FTransform` assume UE4 32-bit `float` (`Vec3` = 12B, `FTransform` = `0x30`). UE5.0+ uses 64-bit `double` (`FVector3d` = 24B, `FTransform3d` = `0x60`). Adding a `"vector_precision": "float" | "double"` option in `ConfigManager` + `UE4Context` is the clean path if UE5 targets are ever needed.
2. **Vulkan Graphics Backend — IMPLEMENTED:**
   - Implemented dynamic-loaded **Vulkan** (`VulkanGraphicsBackend` + `vulkan_wrapper.h/.cpp` + `imgui_impl_vulkan`, zero link-time `-lvulkan` dependency so `android-21` compatibility is preserved) with automatic clean fallback to **OpenGL ES 3** (`OpenGLGraphicsBackend`) via `GraphicsManager` + `IGraphicsBackend`.
   - Upgraded [`ANativeWindowCreator.h`](jni/include/ImGui/native_surface/ANativeWindowCreator.h) to support **Android 5–16+** with null-guarded `libgui`/`libutils` resolution and runtime **Anti-Screenshot / Hide from Recording** (`permeate_record` / `skipScreenshot`).
   - Added live **Switch Gfx (Vulkan / OpenGL ES 3)** and **Anti-Screenshot (Hide Record)** controls in `DebugUI` with safe deferred between-frame `ImGuiCfg::RecreateOverlay()` (touch thread stays alive, zero mid-frame ImGui teardown), persisted in `EspSettings` v3.
3. **Internal Mode (`IMemoryReader` Swap) — IMPLEMENTED:**
   - [`DirectMemoryReader`](jni/include/DirectMemoryReader.hpp) implements `IMemoryReader` in-process (`DIRECT_MEMCPY` sub-microsecond read or kernel-validated `SAFE_SELF_VM`, plus `AttachSelfModule()` parsing `/proc/self/maps`).
   - [`InternalEngineBridge`](jni/include/UE4/InternalBridge.hpp) provides lock-free SeqLock snapshots (`PushCameraPOV`, `PushViewMatrix`, `PushUWorld`, `PushLocalController`, `PushLocalPawn`, `RegisterCharacterClass`) with staleness expiration and automatic fallback to JSON offsets.
   - `EspEngine` uses [`DoubleBufferedEntityPool`](jni/include/ESP/IEntityProvider.hpp) for lock-free concurrent tick/render threads, and [`jni/Android.mk`](jni/Android.mk) exports the reusable static library `libesp4all_core.a` (`obj/local/arm64-v8a/libesp4all_core.a`) alongside the standalone executable `libs/arm64-v8a/core`.
   - Detailed future integration notes for UE4 dump symbols (`Vworld_Offsets`, `GetActorArray_Offset` / `GetXX_Offset`, `ProcessEvent_Offset` Main/Child, `CanvasMap`, `GNativeAndroidApp`), decryption hooks, and Game-Thread ↔ Render-Thread visibility checks are documented in [`future_internal_and_symbols.md`](future_internal_and_symbols.md).
4. **Controlled On-Device Integration Harness:**
   - All engine logic has been validated against simulated ARM64 memory on host and the overlay binary has been verified launching on a real Android device. Next recommended validation step is a small controlled test target process on Android exposing known synthetic structs so `process_vm_readv` + `/proc/<pid>/maps` + live W2S rendering can be verified end-to-end on hardware.
