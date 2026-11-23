// =============================================================================
// ESP4All Core — Host C++17 Config Validator & Template Golden Test Suite
// =============================================================================
// Build & run (macOS / Linux):
//   clang++ -std=c++17 -Wall -Wextra -Werror \
//     -Ijni/include \
//     tests/test_config_and_templates.cpp \
//     jni/src/UE4/UE4Context.cpp \
//     jni/src/ESP/WorldActorsEntityProvider.cpp \
//     jni/src/ESP/GameStatePlayerEntityProvider.cpp \
//     jni/src/ESP/EspEngine.cpp \
//     jni/src/Input/TouchDevice.cpp \
//     jni/src/Input/TouchParser.cpp \
//     jni/src/Input/TouchRouter.cpp \
//     -o /tmp/test_config_and_templates && /tmp/test_config_and_templates
// =============================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config/ConfigManager.hpp"
#include "DirectMemoryReader.hpp"
#include "ESP/EspEngine.hpp"
#include "ESP/EspSettings.hpp"
#include "Graphics/FrameHasher.hpp"
#include "IMemoryReader.hpp"
#include "Input/TouchDevice.hpp"
#include "Input/TouchParser.hpp"
#include "Input/TouchRouter.hpp"
#include "UE4/InternalBridge.hpp"
#include "UE4/UE4Math.hpp"

// =============================================================================
// Global heap allocation tracker (verifies 0 heap allocations in EspEngine::Update)
// =============================================================================
static std::atomic<bool> gTrackAllocs{false};
static std::atomic<size_t> gAllocCount{0};

void* operator new(std::size_t size) {
    if (gTrackAllocs.load(std::memory_order_relaxed)) {
        gAllocCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

// =============================================================================
// Lightweight assertion harness
// =============================================================================
static size_t gAssertionsPassed = 0;
static size_t gAssertionsFailed = 0;

#define EXPECT_TRUE(cond) do { \
    if (cond) { \
        ++gAssertionsPassed; \
    } else { \
        ++gAssertionsFailed; \
        std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " EXPECT_TRUE(" #cond ")\n"; \
    } \
} while (0)

#define EXPECT_EQ(a, b) do { \
    const auto _va = (a); \
    const auto _vb = (b); \
    if (_va == _vb) { \
        ++gAssertionsPassed; \
    } else { \
        ++gAssertionsFailed; \
        std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT_EQ(" #a ", " #b ") got (" << _va << " != " << _vb << ")\n"; \
    } \
} while (0)

#define EXPECT_NEAR(a, b, eps) do { \
    const float _va = static_cast<float>(a); \
    const float _vb = static_cast<float>(b); \
    if (std::fabs(_va - _vb) <= (eps)) { \
        ++gAssertionsPassed; \
    } else { \
        ++gAssertionsFailed; \
        std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT_NEAR(" #a ", " #b ") got (" << _va << " vs " << _vb << ")\n"; \
    } \
} while (0)

// =============================================================================
// Synthetic ARM64 Memory Image (paged MockMemoryReader)
// =============================================================================
class MockMemoryReader final : public IMemoryReader {
public:
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr uint64_t TBI_TAG = 0xB400000000000000ULL;

    explicit MockMemoryReader(uint64_t modBase = 0x7000000000ULL, uint32_t pid = 4242)
        : moduleBase_(modBase), pid_(pid) {}

    bool ReadBuffer(uint64_t address, void* buffer, size_t size) override {
        if (!buffer || size == 0) return false;
        const uint64_t clean = MemoryUtils::StripTag(address);
        auto* out = static_cast<uint8_t*>(buffer);
        size_t remaining = size;
        uint64_t cur = clean;

        while (remaining > 0) {
            const uint64_t pageKey = cur & ~static_cast<uint64_t>(PAGE_SIZE - 1);
            const size_t pageOff = static_cast<size_t>(cur - pageKey);
            const size_t chunk = std::min<size_t>(remaining, PAGE_SIZE - pageOff);

            auto it = pages_.find(pageKey);
            if (it == pages_.end()) {
                return false;
            }
            std::memcpy(out, it->second.data() + pageOff, chunk);
            out += chunk;
            cur += chunk;
            remaining -= chunk;
        }
        return true;
    }

    uint64_t GetModuleBase() const override { return moduleBase_; }
    uint32_t GetPid() const override { return pid_; }

    void WriteBytes(uint64_t address, const void* data, size_t size) {
        const uint64_t clean = MemoryUtils::StripTag(address);
        const auto* src = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        uint64_t cur = clean;

        while (remaining > 0) {
            const uint64_t pageKey = cur & ~static_cast<uint64_t>(PAGE_SIZE - 1);
            const size_t pageOff = static_cast<size_t>(cur - pageKey);
            const size_t chunk = std::min<size_t>(remaining, PAGE_SIZE - pageOff);

            auto& page = pages_[pageKey];
            if (page.empty()) {
                page.assign(PAGE_SIZE, 0);
            }
            std::memcpy(page.data() + pageOff, src, chunk);
            src += chunk;
            cur += chunk;
            remaining -= chunk;
        }
    }

    template<typename T>
    void Write(uint64_t address, const T& value) {
        WriteBytes(address, &value, sizeof(T));
    }

    void WritePointer(uint64_t address, uint64_t ptr, bool withTbiTag = false) {
        const uint64_t stored = withTbiTag ? (ptr | TBI_TAG) : ptr;
        Write<uint64_t>(address, stored);
    }

    void WriteUtf16(uint64_t address, const std::string& ascii) {
        std::vector<uint16_t> buf(ascii.size() + 1, 0);
        for (size_t i = 0; i < ascii.size(); ++i) {
            buf[i] = static_cast<uint16_t>(static_cast<unsigned char>(ascii[i]));
        }
        WriteBytes(address, buf.data(), buf.size() * sizeof(uint16_t));
    }

    void WriteFNamePoolEntry(
        uint64_t poolBase,
        uint64_t blockPtr,
        uint32_t nameIndex,
        const std::string& name)
    {
        const uint32_t blockIdx = nameIndex >> 16;
        const uint32_t blockOff = nameIndex & 0xFFFFU;
        WritePointer(poolBase + 0x10 + static_cast<uint64_t>(blockIdx) * 8ULL, blockPtr);
        const uint64_t entryAddr = blockPtr + static_cast<uint64_t>(blockOff) * 2ULL;
        const uint16_t header = static_cast<uint16_t>((name.size() & 0x3FFU) << 6);
        Write<uint16_t>(entryAddr, header);
        WriteBytes(entryAddr + 2, name.data(), name.size());
    }

private:
    uint64_t moduleBase_;
    uint32_t pid_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pages_;
};

// =============================================================================
// Helpers for locating & patching templates on disk
// =============================================================================
static std::string ResolveTemplatesDir() {
    std::string selfPath = __FILE__;
    const size_t pos = selfPath.rfind("/tests/");
    if (pos != std::string::npos) {
        return selfPath.substr(0, pos) + "/templates/";
    }
    return "templates/";
}

static nlohmann::json LoadTemplateJson(const std::string& dir, const std::string& filename) {
    std::ifstream file(dir + filename);
    EXPECT_TRUE(file.is_open());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    EXPECT_TRUE(!j.is_discarded() && j.is_object());
    return j;
}

static bool HasIssue(const ConfigReport& rep, const std::string& code, const std::string& path = "") {
    for (const auto& i : rep.issues) {
        if (i.code == code && (path.empty() || i.path == path)) {
            return true;
        }
    }
    return false;
}

static size_t RunEngineZeroAllocCheck(EspEngine& engine, int w = 1920, int h = 1080) {
    // Frame 1: warmup (populates FName / ClassPrivate caches if enabled)
    engine.Update(w, h);

    // Frames 2..4: strictly 0 heap allocations
    gAllocCount.store(0, std::memory_order_relaxed);
    gTrackAllocs.store(true, std::memory_order_release);
    engine.Update(w, h);
    engine.Update(w, h);
    engine.Update(w, h);
    gTrackAllocs.store(false, std::memory_order_release);
    return gAllocCount.load(std::memory_order_relaxed);
}

// =============================================================================
// Suite 1: Lint all 5 templates directly from disk
// =============================================================================
static void TestTemplateLint(const std::string& tplDir) {
    std::cout << "--- Suite 1: Template Lint (templates/*.jsonc) ---\n";

    struct ExpectSpec {
        const char* file;
        std::vector<std::string> expectedUnsetPaths;
    };

    const std::vector<ExpectSpec> specs = {
        {
            "minimal_worldactors.jsonc",
            {"globals.GWorld_chain", "actor.RootComponent", "actor.WorldPosition", "camera.view_matrix_chain"}
        },
        {
            "minimal_gamestateplayer.jsonc",
            {"globals.GWorld_chain", "world.GameState", "world.PlayerArray", "player_state.PawnPrivate",
             "actor.RootComponent", "actor.WorldPosition", "camera.view_matrix_chain"}
        },
        {
            "minimal_entitylist_yup.jsonc",
            {"world.EntityList_chain", "actor.RootComponent", "actor.WorldPosition", "camera.view_matrix_chain"}
        },
        {
            "full_worldactors.jsonc",
            {"globals.GWorld_chain", "actor.RootComponent", "actor.WorldPosition",
             "camera.view_matrix_chain", "globals.FNamePool_chain"}
        },
        {
            "full_gamestateplayer.jsonc",
            {"globals.GWorld_chain", "world.GameState", "world.PlayerArray", "player_state.PawnPrivate",
             "actor.RootComponent", "actor.WorldPosition", "camera.view_matrix_chain"}
        }
    };

    for (const auto& s : specs) {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.Load(tplDir + s.file));
        EXPECT_TRUE(cfg.IsLoaded());
        EXPECT_TRUE(cfg.GetPackageName().rfind("com.example.", 0) == 0);

        const ConfigReport& rep = cfg.GetReport();
        EXPECT_EQ(rep.warnCount(), 0U);
        EXPECT_EQ(rep.errorCount(), s.expectedUnsetPaths.size());

        std::unordered_set<std::string> actualPaths;
        for (const auto& issue : rep.issues) {
            EXPECT_EQ(issue.code, std::string("E_REQUIRED_UNSET"));
            actualPaths.insert(issue.path);
        }
        for (const auto& p : s.expectedUnsetPaths) {
            EXPECT_TRUE(actualPaths.count(p) == 1);
        }

        // DumpEffective must always serialize valid JSON
        nlohmann::json eff = nlohmann::json::parse(cfg.DumpEffective(), nullptr, false);
        EXPECT_TRUE(!eff.is_discarded() && eff.is_object());

        // External EspEngine refuses to start on placeholder config without crashing
        MockMemoryReader mem;
        EspEngine engine(mem);
        EXPECT_TRUE(engine.LoadConfig(tplDir + s.file));
        EXPECT_TRUE(!engine.Initialize());
        EXPECT_TRUE(engine.GetLastError().find("E_REQUIRED_UNSET") != std::string::npos);

        std::cout << "  [OK] " << s.file << " (0 warnings, " << rep.errorCount()
                  << " expected placeholder E_REQUIRED_UNSET)\n";
    }
}

// =============================================================================
// Suite 2: Golden End-to-End Synthetic Memory Tests (all 5 templates + variants)
// =============================================================================
static void TestGoldenMinimalWorldActors(const std::string& tplDir) {
    std::cout << "--- Suite 2.1: Golden minimal_worldactors.jsonc (Matrix + Heuristic CameraCache) ---\n";

    constexpr uint64_t BASE = 0x7000000000ULL;
    constexpr uint64_t UWORLD = 0x7100000000ULL;
    constexpr uint64_t LEVEL = 0x7100010000ULL;
    constexpr uint64_t ACTORS_DATA = 0x7100020000ULL;
    constexpr uint64_t VM_OBJ = 0x7100030000ULL;
    constexpr uint64_t VM_INNER = 0x7100031000ULL;

    MockMemoryReader mem(BASE);
    mem.WritePointer(BASE + 0x10000, UWORLD, /*withTbiTag=*/true);
    mem.WritePointer(UWORLD + 0x30, LEVEL, true);
    mem.WritePointer(LEVEL + 0x98, ACTORS_DATA, true);
    mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 5);

    // Camera at origin looking +X (UE4 Z-Up, FOV 90)
    FMinimalViewInfo cam;
    cam.Location = Vec3(0.0f, 0.0f, 100.0f);
    cam.Rotation = Vec3(0.0f, 0.0f, 0.0f);
    cam.FOV = 90.0f;
    const auto vm = UE4Math::BuildUE4ViewProjectionMatrix(cam, 1920, 1080);

    mem.WritePointer(BASE + 0x20000, VM_OBJ);
    mem.WritePointer(VM_OBJ + 0x20, VM_INNER);
    mem.WriteBytes(VM_INNER + 0x270, vm.data(), sizeof(vm));

    const Vec3 positions[5] = {
        Vec3(500.0f, 0.0f, 50.0f),     // visible
        Vec3(1200.0f, 200.0f, 50.0f),  // visible
        Vec3(2000.0f, -300.0f, 50.0f), // visible
        Vec3(-500.0f, 0.0f, 50.0f),    // behind camera (Z-clipped)
        Vec3(0.0f, 0.0f, 0.0f)         // skipped by skip_zero_pos
    };
    for (int i = 0; i < 5; ++i) {
        const uint64_t actor = 0x7200000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        const uint64_t root = 0x7210000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        mem.WritePointer(ACTORS_DATA + static_cast<uint64_t>(i) * 8ULL, actor, true);
        mem.WritePointer(actor + 0x1B0, root, true);
        mem.Write<Vec3>(root + 0x11C, positions[i]);
    }

    // Load ACTUAL template from disk and fill only the 4 placeholders
    nlohmann::json j = LoadTemplateJson(tplDir, "minimal_worldactors.jsonc");
    j["globals"]["GWorld_chain"] = {"0x10000"};
    j["camera"]["view_matrix_chain"] = {"0x20000", "0x20", "+0x270"};
    j["actor"]["RootComponent"] = "0x1B0";
    j["actor"]["WorldPosition"] = "0x11C";

    EspEngine engine(mem);
    EXPECT_TRUE(engine.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engine.GetConfig().GetReport().ok());
    EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engine.Initialize());

    const size_t allocs = RunEngineZeroAllocCheck(engine);
    EXPECT_EQ(allocs, 0U);
    EXPECT_EQ(engine.GetStats().totalRawActors, 5U);
    EXPECT_EQ(engine.GetStats().filteredEntities, 4U); // 4 non-zero actors
    EXPECT_EQ(engine.GetStats().drawnPlayers, 3U);     // 3 in front of camera
    EXPECT_EQ(engine.GetStats().droppedByPoolCap, 0U);

    // Sub-case 2.1b: Commented CameraCache + heuristic_pc_scan alternative from minimal_worldactors.jsonc
    constexpr uint64_t PC_ACTOR = 0x7200050000ULL;
    constexpr uint64_t CAM_MGR = 0x7220000000ULL;
    constexpr uint64_t PC_CLASS = 0x7300000100ULL;
    mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 6);
    mem.WritePointer(ACTORS_DATA + 5 * 8ULL, PC_ACTOR);
    mem.WritePointer(PC_ACTOR + 0x10, PC_CLASS);
    mem.WritePointer(PC_ACTOR + 0x2B8, CAM_MGR);
    mem.Write<Vec3>(CAM_MGR + 0x1AE0 + 0x10 + 0x0, cam.Location);
    mem.Write<Vec3>(CAM_MGR + 0x1AE0 + 0x10 + 0xC, cam.Rotation);
    mem.Write<float>(CAM_MGR + 0x1AE0 + 0x10 + 0x18, cam.FOV);

    nlohmann::json jCam = LoadTemplateJson(tplDir, "minimal_worldactors.jsonc");
    jCam["globals"]["GWorld_chain"] = {"0x10000"};
    jCam["local_player"] = {{"PlayerController_CameraManager", "0x2B8"}};
    jCam["camera"] = {
        {"mode", "camera_cache"},
        {"up_axis", "Z"},
        {"heuristic_pc_scan", true},
        {"camera_cache", {{"CameraCache", "0x1AE0"}, {"POV", "0x10"}}}
    };
    jCam["actor"]["RootComponent"] = "0x1B0";
    jCam["actor"]["WorldPosition"] = "0x11C";

    EspEngine engineCam(mem);
    EXPECT_TRUE(engineCam.LoadConfigFromString(jCam.dump()));
    EXPECT_TRUE(engineCam.GetConfig().GetReport().ok());
    EXPECT_EQ(engineCam.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engineCam.Initialize());
    EXPECT_EQ(RunEngineZeroAllocCheck(engineCam), 0U);
    EXPECT_TRUE(engineCam.GetContext().GetDiagnostics().usedHeuristicCameraPC);
    EXPECT_EQ(engineCam.GetStats().drawnPlayers, 3U);

    std::cout << "  [OK] minimal_worldactors (Matrix drawn=3, Heuristic CameraCache drawn=3, 0 allocs)\n";
}

static void TestGoldenMinimalGameStatePlayer(const std::string& tplDir) {
    std::cout << "--- Suite 2.2: Golden minimal_gamestateplayer.jsonc ---\n";

    constexpr uint64_t BASE = 0x7000000000ULL;
    constexpr uint64_t UWORLD = 0x7100000000ULL;
    constexpr uint64_t GAMESTATE = 0x7100010000ULL;
    constexpr uint64_t PLAYER_ARRAY = 0x7100020000ULL;
    constexpr uint64_t VM_OBJ = 0x7100030000ULL;

    MockMemoryReader mem(BASE);
    mem.WritePointer(BASE + 0x10000, UWORLD);
    mem.WritePointer(UWORLD + 0x140, GAMESTATE);
    mem.WritePointer(GAMESTATE + 0x240, PLAYER_ARRAY);
    mem.Write<int32_t>(GAMESTATE + 0x240 + 0x8, 3);

    FMinimalViewInfo cam;
    cam.Location = Vec3(0.0f, 0.0f, 100.0f);
    cam.Rotation = Vec3(0.0f, 0.0f, 0.0f);
    cam.FOV = 90.0f;
    const auto vm = UE4Math::BuildUE4ViewProjectionMatrix(cam, 1920, 1080);
    mem.WritePointer(BASE + 0x20000, VM_OBJ);
    mem.WriteBytes(VM_OBJ + 0x290, vm.data(), sizeof(vm));

    for (int i = 0; i < 3; ++i) {
        const uint64_t ps = 0x7200000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        const uint64_t pawn = 0x7210000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        const uint64_t root = 0x7220000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        mem.WritePointer(PLAYER_ARRAY + static_cast<uint64_t>(i) * 8ULL, ps);
        mem.WritePointer(ps + 0x2A8, pawn);
        mem.WritePointer(pawn + 0x158, root);
        mem.Write<Vec3>(root + 0x11C, Vec3(600.0f + i * 300.0f, (i - 1) * 120.0f, 40.0f));
    }

    nlohmann::json j = LoadTemplateJson(tplDir, "minimal_gamestateplayer.jsonc");
    j["globals"]["GWorld_chain"] = {"0x10000"};
    j["world"]["GameState"] = "0x140";
    j["world"]["PlayerArray"] = "0x240";
    j["player_state"]["PawnPrivate"] = "0x2A8";
    j["camera"]["view_matrix_chain"] = {"0x20000", "+0x290"};
    j["actor"]["RootComponent"] = "0x158";
    j["actor"]["WorldPosition"] = "0x11C";

    EspEngine engine(mem);
    EXPECT_TRUE(engine.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engine.GetConfig().GetReport().ok());
    EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engine.Initialize());

    EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
    EXPECT_EQ(engine.GetStats().drawnPlayers, 3U);

    std::cout << "  [OK] minimal_gamestateplayer (drawn=3, 0 allocs)\n";
}

static void TestGoldenMinimalEntityListYUp(const std::string& tplDir) {
    std::cout << "--- Suite 2.3: Golden minimal_entitylist_yup.jsonc (RowMajor + Y-Up + end_ptr) ---\n";

    constexpr uint64_t BASE = 0x7000000000ULL;
    constexpr uint64_t HOP1 = 0x7100010000ULL;
    constexpr uint64_t CONTAINER = 0x7100020000ULL;
    constexpr uint64_t BEGIN_PTR = 0x7100030000ULL;
    constexpr uint64_t VM_OBJ = 0x7100040000ULL;
    constexpr uint64_t VM_INNER = 0x7100041000ULL;

    MockMemoryReader mem(BASE);
    // Multi-hop EntityList_chain -> CONTAINER; LevelActors = 0xF0 -> [beginPtr, endPtr]
    mem.WritePointer(BASE + 0x12000, HOP1);
    mem.WritePointer(HOP1 + 0x28, CONTAINER);
    mem.WritePointer(CONTAINER + 0xF0, BEGIN_PTR);
    mem.WritePointer(CONTAINER + 0xF0 + 0x8, BEGIN_PTR + 2 * sizeof(uint64_t));

    // Y-Up camera at (0, 1.7, 0) looking +Z (Yaw=0, Pitch=0), RowMajor matrix
    FMinimalViewInfo cam;
    cam.Location = Vec3(0.0f, 1.7f, 0.0f);
    cam.Rotation = Vec3(0.0f, 0.0f, 0.0f);
    cam.FOV = 90.0f;
    const auto vm = UE4Math::BuildViewProjectionMatrix(
        cam, 1920, 1080, MatrixLayout::RowMajor, UpAxis::Y);

    mem.WritePointer(BASE + 0x22000, VM_OBJ);
    mem.WritePointer(VM_OBJ + 0x0, VM_INNER); // step 1 = 0x0 (legit intermediate deref!)
    mem.WriteBytes(VM_INNER + 0x490, vm.data(), sizeof(vm));

    for (int i = 0; i < 2; ++i) {
        const uint64_t actor = 0x7200000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        const uint64_t root = 0x7210000000ULL + static_cast<uint64_t>(i) * 0x1000ULL;
        mem.WritePointer(BEGIN_PTR + static_cast<uint64_t>(i) * 8ULL, actor);
        mem.WritePointer(actor + 0x40, root);
        // Meters in Y-Up (+Z forward, +Y up)
        mem.Write<Vec3>(root + 0x7C, Vec3((i == 0 ? -2.0f : 2.0f), 0.5f, 15.0f + i * 5.0f));
    }

    nlohmann::json j = LoadTemplateJson(tplDir, "minimal_entitylist_yup.jsonc");
    j["world"]["EntityList_chain"] = {"0x12000", "0x28"};
    j["world"]["LevelActors"] = "0xF0";
    j["camera"]["view_matrix_chain"] = {"0x22000", "0x0", "+0x490"};
    j["actor"]["RootComponent"] = "0x40";
    j["actor"]["WorldPosition"] = "0x7C";

    EspEngine engine(mem);
    EXPECT_TRUE(engine.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engine.GetConfig().GetReport().ok());
    EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engine.Initialize());

    EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
    EXPECT_TRUE(engine.GetContext().GetDiagnostics().usedEntityListChain);
    EXPECT_EQ(engine.GetStats().drawnPlayers, 2U);

    std::cout << "  [OK] minimal_entitylist_yup (EntityList_chain + RowMajor + Y-Up drawn=2, 0 allocs)\n";
}

static void TestGoldenFullWorldActors(const std::string& tplDir) {
    std::cout << "--- Suite 2.4: Golden full_worldactors.jsonc (FNamePool + ClassPrivate + Attrs + Bones) ---\n";

    constexpr uint64_t BASE = 0x7000000000ULL;
    constexpr uint64_t UWORLD = 0x7100000000ULL;
    constexpr uint64_t LEVEL = 0x7100010000ULL;
    constexpr uint64_t ACTORS_DATA = 0x7100020000ULL;
    constexpr uint64_t GAME_INSTANCE = 0x7100030000ULL;
    constexpr uint64_t LOCAL_PLAYERS_DATA = 0x7100031000ULL;
    constexpr uint64_t LOCAL_PLAYER = 0x7100032000ULL;
    constexpr uint64_t LOCAL_PC = 0x7100033000ULL;
    constexpr uint64_t VM_OBJ = 0x7100040000ULL;
    constexpr uint64_t FNAME_POOL = BASE + 0x50000ULL; // inline FNamePool (+0x50000)
    constexpr uint64_t FNAME_BLOCK0 = 0x7100060000ULL;

    constexpr uint64_t CLASS_PAWN = 0x7300001000ULL;
    constexpr uint64_t CLASS_PROP = 0x7300002000ULL;

    MockMemoryReader mem(BASE);
    mem.WritePointer(BASE + 0x10000, UWORLD);
    mem.WritePointer(UWORLD + 0x30, LEVEL);
    mem.WritePointer(UWORLD + 0x180, GAME_INSTANCE);
    mem.WritePointer(LEVEL + 0x98, ACTORS_DATA);
    mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 5);

    // LocalPlayer chain (2 derefs on inline TArray LocalPlayers)
    mem.WritePointer(GAME_INSTANCE + 0x38, LOCAL_PLAYERS_DATA);
    mem.Write<int32_t>(GAME_INSTANCE + 0x38 + 0x8, 1);
    mem.WritePointer(LOCAL_PLAYERS_DATA, LOCAL_PLAYER);
    mem.WritePointer(LOCAL_PLAYER + 0x30, LOCAL_PC);

    // FNamePool entries
    mem.WriteFNamePoolEntry(FNAME_POOL, FNAME_BLOCK0, 10, "BP_PlayerPawn_C");
    mem.WriteFNamePoolEntry(FNAME_POOL, FNAME_BLOCK0, 40, "StaticMeshActor");
    mem.Write<uint32_t>(CLASS_PAWN + 0x18, 10);
    mem.Write<uint32_t>(CLASS_PROP + 0x18, 40);

    // Camera
    FMinimalViewInfo cam;
    cam.Location = Vec3(0.0f, 0.0f, 100.0f);
    cam.Rotation = Vec3(0.0f, 0.0f, 0.0f);
    cam.FOV = 90.0f;
    const auto vm = UE4Math::BuildUE4ViewProjectionMatrix(cam, 1920, 1080);
    mem.WritePointer(BASE + 0x20000, VM_OBJ);
    mem.WriteBytes(VM_OBJ + 0x270, vm.data(), sizeof(vm));

    auto writeActor = [&](int idx, uint64_t cls, uint32_t fnameIdx, Vec3 pos,
                          float hp, float maxHp, int32_t team, const std::string& name, uint8_t isBot) -> uint64_t {
        const uint64_t actor = 0x7200000000ULL + static_cast<uint64_t>(idx) * 0x2000ULL;
        const uint64_t root = 0x7210000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;
        const uint64_t nameBuf = 0x7220000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;
        mem.WritePointer(ACTORS_DATA + static_cast<uint64_t>(idx) * 8ULL, actor);
        mem.WritePointer(actor + 0x10, cls);
        mem.Write<uint32_t>(actor + 0x18, fnameIdx);
        mem.WritePointer(actor + 0x1B0, root);
        mem.Write<Vec3>(root + 0x11C, pos);
        mem.Write<float>(actor + 0x500, hp);
        mem.Write<float>(actor + 0x504, maxHp);
        mem.Write<int32_t>(actor + 0x508, team);
        mem.WritePointer(actor + 0x510, nameBuf);
        mem.WriteUtf16(nameBuf, name);
        mem.Write<uint8_t>(actor + 0x518, isBot);
        return actor;
    };

    const uint64_t localPawn = writeActor(0, CLASS_PAWN, 10, Vec3(0.0f, 0.0f, 50.0f), 100.0f, 100.0f, 1, "LocalMe", 0);
    mem.WritePointer(LOCAL_PC + 0x2A0, localPawn);

    // 1: Alive enemy in range (should draw)
    const uint64_t enemy1 = writeActor(1, CLASS_PAWN, 10, Vec3(1000.0f, 0.0f, 50.0f), 85.0f, 100.0f, 7, "Alpha", 0);
    // 2: Alive bot in range (should draw)
    writeActor(2, CLASS_PAWN, 10, Vec3(1500.0f, 200.0f, 50.0f), 60.0f, 100.0f, 8, "BotOne", 1);
    // 3: Dead enemy (skipped by skip_dead: true)
    writeActor(3, CLASS_PAWN, 10, Vec3(1200.0f, -100.0f, 50.0f), 0.0f, 100.0f, 9, "DeadGuy", 0);
    // 4: Prop StaticMeshActor (rejected by class_private)
    writeActor(4, CLASS_PROP, 40, Vec3(800.0f, 50.0f, 50.0f), 0.0f, 0.0f, 0, "Tree", 0);

    nlohmann::json j = LoadTemplateJson(tplDir, "full_worldactors.jsonc");
    j["globals"]["GWorld_chain"] = {"0x10000"};
    j["globals"]["FNamePool_chain"] = {"+0x50000"};
    j["world"]["OwningGameInstance"] = "0x180";
    j["local_player"]["PlayerController_Pawn"] = "0x2A0";
    j["camera"]["view_matrix_chain"] = {"0x20000", "+0x270"};
    j["actor"]["RootComponent"] = "0x1B0";
    j["actor"]["WorldPosition"] = "0x11C";
    j["actor"]["Health"] = "0x500";
    j["actor"]["MaxHealth"] = "0x504";
    j["actor"]["TeamId"] = "0x508";
    j["actor"]["PlayerName"] = "0x510";
    j["actor"]["IsBot"] = "0x518";

    EspEngine engine(mem);
    EXPECT_TRUE(engine.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engine.GetConfig().GetReport().ok());
    EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engine.Initialize());

    EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
    EXPECT_EQ(engine.GetStats().drawnPlayers, 2U);
    EXPECT_TRUE(engine.GetContext().GetDiagnostics().acceptedClassCount >= 1U);
    EXPECT_TRUE(engine.GetContext().GetDiagnostics().rejectedClassCount >= 1U);

    const auto& pool = engine.GetPool();
    EXPECT_EQ(pool.Count(), 2U);
    EXPECT_EQ(pool[0].address, enemy1);
    EXPECT_TRUE(pool[0].hasHealth);
    EXPECT_NEAR(pool[0].health, 85.0f, 0.01f);
    EXPECT_NEAR(pool[0].maxHealth, 100.0f, 0.01f);
    EXPECT_EQ(pool[0].teamId, 7);
    EXPECT_TRUE(pool[0].hasPlayerName);
    EXPECT_EQ(std::string(pool[0].playerName), std::string("Alpha"));
    EXPECT_TRUE(!pool[0].isBot);
    EXPECT_TRUE(pool[1].isBot);

    // Sub-case 2.4b: Enable Bones + ComponentToWorld position source
    constexpr uint64_t MESH1 = 0x7230000000ULL;
    constexpr uint64_t BONES1 = 0x7230010000ULL;
    const uint64_t root1 = mem.ReadPointer(enemy1 + 0x1B0);
    FTransform rootC2W{};
    rootC2W.Translation = Vec3(1000.0f, 0.0f, 50.0f);
    mem.Write<FTransform>(root1 + 0x1D0, rootC2W);

    mem.WritePointer(enemy1 + 0x280, MESH1);
    mem.Write<FTransform>(MESH1 + 0x1D0, rootC2W);
    mem.WritePointer(MESH1 + 0x480, BONES1);
    mem.Write<int32_t>(MESH1 + 0x488, 20);
    for (int b = 0; b < 20; ++b) {
        FTransform bt{};
        bt.Translation = Vec3(0.0f, 0.0f, static_cast<float>(b) * 8.0f);
        mem.Write<FTransform>(BONES1 + static_cast<uint64_t>(b) * sizeof(FTransform), bt);
    }

    j["actor"]["PositionSource"] = "ComponentToWorld";
    j["actor"]["ComponentToWorld"] = "0x1D0";
    j["actor"].erase("WorldPosition");
    j["bones"] = {
        {"enabled", true},
        {"Mesh", "0x280"},
        {"ComponentToWorld", "0x1D0"},
        {"BoneArray", "0x480"},
        {"BoneCountOffset", "0x488"},
        {"default_profile", {14, 10, 5, 11, 12, 8, 9, 6, 7, 3, 4, 1, 2, 0, 0}}
    };

    // Also add rootC2W for localPawn and enemy2 so they resolve under ComponentToWorld
    const uint64_t rootLocal = mem.ReadPointer(localPawn + 0x1B0);
    FTransform localC2W{};
    localC2W.Translation = Vec3(0.0f, 0.0f, 50.0f);
    mem.Write<FTransform>(rootLocal + 0x1D0, localC2W);

    EspEngine engineBones(mem);
    EXPECT_TRUE(engineBones.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engineBones.GetConfig().GetReport().ok());
    EXPECT_EQ(engineBones.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engineBones.Initialize());
    EXPECT_EQ(RunEngineZeroAllocCheck(engineBones), 0U);
    EXPECT_TRUE(engineBones.GetPool().Count() >= 1U);
    EXPECT_TRUE(engineBones.GetPool()[0].hasSkeleton);
    EXPECT_EQ(engineBones.GetPool()[0].boneOnScreen[BONE_HEAD], static_cast<uint8_t>(1));

    std::cout << "  [OK] full_worldactors (FNamePool + ClassPrivate + Attrs + Bones + C2W, 0 allocs)\n";
}

static void TestGoldenFullGameStatePlayer(const std::string& tplDir) {
    std::cout << "--- Suite 2.5: Golden full_gamestateplayer.jsonc (PlayerState attrs + CameraCache fallback) ---\n";

    constexpr uint64_t BASE = 0x7000000000ULL;
    constexpr uint64_t UWORLD = 0x7100000000ULL;
    constexpr uint64_t GAMESTATE = 0x7100010000ULL;
    constexpr uint64_t PLAYER_ARRAY = 0x7100020000ULL;
    constexpr uint64_t GAME_INSTANCE = 0x7100030000ULL;
    constexpr uint64_t LOCAL_PLAYERS_DATA = 0x7100031000ULL;
    constexpr uint64_t LOCAL_PLAYER = 0x7100032000ULL;
    constexpr uint64_t LOCAL_PC = 0x7100033000ULL;
    constexpr uint64_t CAM_MGR = 0x7100034000ULL;

    MockMemoryReader mem(BASE);
    mem.WritePointer(BASE + 0x10000, UWORLD);
    mem.WritePointer(UWORLD + 0x140, GAMESTATE);
    mem.WritePointer(UWORLD + 0x1C0, GAME_INSTANCE);
    mem.WritePointer(GAMESTATE + 0x240, PLAYER_ARRAY);
    mem.Write<int32_t>(GAMESTATE + 0x240 + 0x8, 4);

    // LocalPlayer -> PlayerController -> CameraManager (CameraCache POV)
    mem.WritePointer(GAME_INSTANCE + 0x38, LOCAL_PLAYERS_DATA);
    mem.Write<int32_t>(GAME_INSTANCE + 0x38 + 0x8, 1);
    mem.WritePointer(LOCAL_PLAYERS_DATA, LOCAL_PLAYER);
    mem.WritePointer(LOCAL_PLAYER + 0x30, LOCAL_PC);
    mem.WritePointer(LOCAL_PC + 0x2F8, CAM_MGR);

    mem.Write<Vec3>(CAM_MGR + 0x2B50 + 0x10 + 0x0, Vec3(0.0f, 0.0f, 100.0f));
    mem.Write<Vec3>(CAM_MGR + 0x2B50 + 0x10 + 0xC, Vec3(0.0f, 0.0f, 0.0f));
    mem.Write<float>(CAM_MGR + 0x2B50 + 0x10 + 0x18, 90.0f);

    auto writePlayer = [&](int idx, Vec3 pos, float hp, float maxHp, int32_t team, const std::string& name) -> uint64_t {
        const uint64_t ps = 0x7200000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;
        const uint64_t pawn = 0x7210000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;
        const uint64_t root = 0x7220000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;
        const uint64_t nameBuf = 0x7230000000ULL + static_cast<uint64_t>(idx) * 0x1000ULL;

        mem.WritePointer(PLAYER_ARRAY + static_cast<uint64_t>(idx) * 8ULL, ps);
        mem.WritePointer(ps + 0x528, pawn);
        mem.WritePointer(pawn + 0x268, ps);
        mem.WritePointer(pawn + 0x158, root);
        mem.Write<Vec3>(root + 0x11C, pos);

        // Attributes stored ONLY on APlayerState
        mem.Write<float>(ps + 0x618, hp);
        mem.Write<float>(ps + 0x61C, maxHp);
        mem.Write<int32_t>(ps + 0x700, team);
        mem.WritePointer(ps + 0x408, nameBuf);
        mem.WriteUtf16(nameBuf, name);
        return pawn;
    };

    const uint64_t localPawn = writePlayer(0, Vec3(0.0f, 0.0f, 50.0f), 100.0f, 100.0f, 3, "Me");
    mem.WritePointer(LOCAL_PC + 0x2A0, localPawn);

    // 1: Teammate (team 3 -> skipped when skip_same_team = true)
    writePlayer(1, Vec3(800.0f, -100.0f, 50.0f), 100.0f, 100.0f, 3, "Ally");
    // 2: Enemy 1 (should draw)
    writePlayer(2, Vec3(1000.0f, 0.0f, 50.0f), 72.5f, 100.0f, 9, "Bravo");
    // 3: Enemy 2 (should draw)
    writePlayer(3, Vec3(1600.0f, 250.0f, 50.0f), 40.0f, 100.0f, 9, "Charlie");

    nlohmann::json j = LoadTemplateJson(tplDir, "full_gamestateplayer.jsonc");
    j["globals"]["GWorld_chain"] = {"0x10000"};
    j["world"]["GameState"] = "0x140";
    j["world"]["PlayerArray"] = "0x240";
    j["world"]["OwningGameInstance"] = "0x1C0";
    j["local_player"]["PlayerController_Pawn"] = "0x2A0";
    j["local_player"]["PlayerController_CameraManager"] = "0x2F8";
    // Leave view_matrix_chain empty so camera.mode = "auto" uses CameraCache fallback
    j["camera"]["camera_cache"]["CameraCache"] = "0x2B50";
    j["entity_filter"]["skip_same_team"] = true;
    j["actor"]["RootComponent"] = "0x158";
    j["actor"]["WorldPosition"] = "0x11C";
    j["actor"]["PlayerState"] = "0x268";
    j["player_state"]["PawnPrivate"] = "0x528";
    j["player_state"]["Health"] = "0x618";
    j["player_state"]["MaxHealth"] = "0x61C";
    j["player_state"]["TeamId"] = "0x700";
    j["player_state"]["PlayerName"] = "0x408";

    EspEngine engine(mem);
    EXPECT_TRUE(engine.LoadConfigFromString(j.dump()));
    EXPECT_TRUE(engine.GetConfig().GetReport().ok());
    EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
    EXPECT_TRUE(engine.Initialize());

    EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
    EXPECT_TRUE(engine.GetContext().GetDiagnostics().hasCameraPOV);
    EXPECT_TRUE(!engine.GetContext().IsUsingViewMatrix());
    EXPECT_EQ(engine.GetStats().drawnPlayers, 2U);

    const auto& pool = engine.GetPool();
    EXPECT_EQ(pool.Count(), 2U);
    EXPECT_NEAR(pool[0].health, 72.5f, 0.01f);
    EXPECT_EQ(pool[0].teamId, 9);
    EXPECT_EQ(std::string(pool[0].playerName), std::string("Bravo"));

    std::cout << "  [OK] full_gamestateplayer (PlayerState attrs + CameraCache fallback + skip_same_team, 0 allocs)\n";
}

// =============================================================================
// Suite 3: Negative Validator Tests, Pool Overflow (droppedByPoolCap), Bridge & Fuzz
// =============================================================================
static void TestValidatorNegativesAndPoolOverflow() {
    std::cout << "--- Suite 3: Negative Validator Tests, Pool Cap & Robustness ---\n";

    // 3.1 Unknown key typo + Levenshtein suggestion
    {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.LoadFromString(R"({
            "game": {"package_name": "com.example.t", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "globals": {"GWrld_chain": ["0x1000"], "GWorld_chain": ["0x1000"]},
            "world": {"PersistentLevel": "0x30", "LevelActors": "0x98"},
            "camera": {"mode": "matrix", "view_matrix_chain": ["0x2000", "+0x10"]},
            "actor": {"RootComponnt": "0x1B0", "RootComponent": "0x1B0", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(cfg.GetReport().ok());
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_UNKNOWN_KEY", "globals.GWrld_chain"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_UNKNOWN_KEY", "actor.RootComponnt"));
    }

    // 3.2 All W_UNUSED_KEY predicates
    {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.LoadFromString(R"({
            "game": {"package_name": "com.example.t", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "globals": {"GWorld_chain": ["0x1000"], "GNames_chain": ["0x2000"]},
            "world": {"PersistentLevel": "0x30", "LevelActors": "0x98", "GameState": "0x140", "PlayerArray": "0x240"},
            "camera": {"mode": "matrix", "view_matrix_chain": ["0x3000", "+0x10"], "camera_cache": {"CameraCache": "0x1AE0"}},
            "name_system": {"enabled": true, "type": "GNames", "allocator": {"Blocks": "0x10"}, "entry": {"HeaderSize": "0x2"}},
            "entity_filter": {"mode": "none", "max_distance": 300.0, "max_z_delta": 5000.0, "sentinel": {"offset": "0x100"}},
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C", "ComponentToWorld": "0x1D0"},
            "bones": {"enabled": false, "Mesh": "0x280"}
        })"));
        const auto& rep = cfg.GetReport();
        EXPECT_TRUE(rep.ok());
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "world.GameState"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "world.PlayerArray"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "camera.camera_cache"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "name_system.allocator"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "name_system.entry"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "entity_filter.sentinel"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "entity_filter.max_distance"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "entity_filter.max_z_delta"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "actor.ComponentToWorld"));
        EXPECT_TRUE(HasIssue(rep, "W_UNUSED_KEY", "bones.Mesh"));
    }

    // 3.3 GameStatePlayer unused keys (PersistentLevel, LevelActors, heuristic_pc_scan)
    {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.LoadFromString(R"({
            "game": {"package_name": "com.example.t", "module_name": "libUE4.so"},
            "entity_provider": {"type": "GameStatePlayer"},
            "globals": {"GWorld_chain": ["0x1000"]},
            "world": {"GameState": "0x140", "PlayerArray": "0x240", "PersistentLevel": "0x30", "LevelActors": "0x98"},
            "player_state": {"PawnPrivate": "0x2A8"},
            "camera": {"mode": "matrix", "heuristic_pc_scan": true, "view_matrix_chain": ["0x2000", "+0x10"]},
            "actor": {"RootComponent": "0x158", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_UNUSED_KEY", "world.PersistentLevel"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_UNUSED_KEY", "world.LevelActors"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_UNUSED_KEY", "camera.heuristic_pc_scan"));
    }

    // 3.4 Conflict scalar/chain + W_ZERO_FIRST_STEP_IN_CHAIN (step 0 vs step 1 vs actor-rooted)
    {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.LoadFromString(R"({
            "game": {"package_name": "com.example.t", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "globals": {"GWorld": "0x1111", "GWorld_chain": ["0x2222"]},
            "world": {"PersistentLevel": "0x30", "LevelActors": "0x98"},
            "local_player": {"LocalPawn_chain": ["0x3000", "0x0", "0x20"]},
            "camera": {"mode": "matrix", "view_matrix_chain": ["0x0", "0x20", "+0x270"]},
            "entity_filter": {"mode": "sentinel", "sentinel": {"offset": "0x100", "chain": ["0x0", "+0xF4"], "type": "int32", "value": 30}},
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_CONFLICT_SCALAR_CHAIN", "globals.GWorld_chain"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_CONFLICT_SCALAR_CHAIN", "entity_filter.sentinel.chain"));
        EXPECT_TRUE(HasIssue(cfg.GetReport(), "W_ZERO_FIRST_STEP_IN_CHAIN", "camera.view_matrix_chain"));
        EXPECT_TRUE(!HasIssue(cfg.GetReport(), "W_ZERO_FIRST_STEP_IN_CHAIN", "local_player.LocalPawn_chain"));
        EXPECT_TRUE(!HasIssue(cfg.GetReport(), "W_ZERO_FIRST_STEP_IN_CHAIN", "entity_filter.sentinel.chain"));
    }

    // 3.5 E_BAD_TYPE (invalid enum, hex, negative unsigned offset, short bone profile, bad section type)
    {
        ConfigManager cfg;
        EXPECT_TRUE(cfg.LoadFromString(R"({
            "game": {"package_name": "com.example.t", "module_name": "libUE4.so"},
            "entity_provider": {"type": "InvalidProvider"},
            "globals": {"GWorld": "0xZZZ", "GWorld_chain": ["not_hex"]},
            "world": {"PersistentLevel": "-0x10", "LevelActors": "0x98"},
            "camera": {"mode": "bad_mode", "view_matrix_chain": "not_array"},
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C", "PositionSource": "InvalidSrc"},
            "bones": {"enabled": true, "default_profile": [1, 2, 3]}
        })"));
        const auto& rep = cfg.GetReport();
        EXPECT_TRUE(!rep.ok());
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "entity_provider.type"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "globals.GWorld"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "globals.GWorld_chain[0]"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "world.PersistentLevel"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "camera.mode"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "camera.view_matrix_chain"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "actor.PositionSource"));
        EXPECT_TRUE(HasIssue(rep, "E_BAD_TYPE", "bones.default_profile"));
        EXPECT_TRUE(HasIssue(rep, "E_REQUIRED_UNSET", "bones.Mesh"));
    }

    // 3.6 Pool overflow: WorldActors with 300 valid actors -> 256 collected + 44 droppedByPoolCap
    {
        constexpr uint64_t BASE = 0x7000000000ULL;
        constexpr uint64_t UWORLD = 0x7100000000ULL;
        constexpr uint64_t LEVEL = 0x7100010000ULL;
        constexpr uint64_t ACTORS_DATA = 0x7100020000ULL;
        constexpr uint64_t VM_OBJ = 0x7100030000ULL;

        MockMemoryReader mem(BASE);
        mem.WritePointer(BASE + 0x10000, UWORLD);
        mem.WritePointer(UWORLD + 0x30, LEVEL);
        mem.WritePointer(LEVEL + 0x98, ACTORS_DATA);
        mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 300);

        FMinimalViewInfo cam;
        cam.Location = Vec3(0.0f, 0.0f, 100.0f);
        cam.FOV = 90.0f;
        const auto vm = UE4Math::BuildUE4ViewProjectionMatrix(cam, 1920, 1080);
        mem.WritePointer(BASE + 0x20000, VM_OBJ);
        mem.WriteBytes(VM_OBJ + 0x100, vm.data(), sizeof(vm));

        for (int i = 0; i < 300; ++i) {
            const uint64_t actor = 0x7200000000ULL + static_cast<uint64_t>(i) * 0x200ULL;
            const uint64_t root = 0x7250000000ULL + static_cast<uint64_t>(i) * 0x200ULL;
            mem.WritePointer(ACTORS_DATA + static_cast<uint64_t>(i) * 8ULL, actor);
            mem.WritePointer(actor + 0x1B0, root);
            mem.Write<Vec3>(root + 0x11C, Vec3(1000.0f + i, 0.0f, 50.0f));
        }

        EspEngine engine(mem);
        EXPECT_TRUE(engine.LoadConfigFromString(R"({
            "game": {"package_name": "com.example.pool", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "globals": {"GWorld_chain": ["0x10000"]},
            "world": {"PersistentLevel": "0x30", "LevelActors": "0x98"},
            "camera": {"mode": "matrix", "view_matrix_chain": ["0x20000", "+0x100"]},
            "entity_filter": {"mode": "none", "skip_zero_pos": true, "skip_local_player": false},
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(engine.Initialize());
        EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
        EXPECT_EQ(engine.GetStats().filteredEntities, 256U);
        EXPECT_EQ(engine.GetStats().droppedByPoolCap, 44U);
        EXPECT_EQ(engine.GetStats().drawnPlayers, 256U);

        // Exactly 256 valid actors + 4 trailing nulls -> droppedByPoolCap MUST be 0
        mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 260);
        for (int i = 256; i < 260; ++i) {
            mem.WritePointer(ACTORS_DATA + static_cast<uint64_t>(i) * 8ULL, 0);
        }
        engine.Update(1920, 1080);
        EXPECT_EQ(engine.GetStats().filteredEntities, 256U);
        EXPECT_EQ(engine.GetStats().droppedByPoolCap, 0U);
    }

    // 3.7 Pool overflow in GameStatePlayer: 270 valid players -> 256 + 14 dropped
    {
        constexpr uint64_t BASE = 0x7000000000ULL;
        constexpr uint64_t UWORLD = 0x7100000000ULL;
        constexpr uint64_t GS = 0x7100010000ULL;
        constexpr uint64_t ARR = 0x7100020000ULL;
        constexpr uint64_t VM_OBJ = 0x7100030000ULL;

        MockMemoryReader mem(BASE);
        mem.WritePointer(BASE + 0x10000, UWORLD);
        mem.WritePointer(UWORLD + 0x140, GS);
        mem.WritePointer(GS + 0x240, ARR);
        mem.Write<int32_t>(GS + 0x240 + 0x8, 270);

        FMinimalViewInfo cam;
        cam.Location = Vec3(0.0f, 0.0f, 100.0f);
        cam.FOV = 90.0f;
        const auto vm = UE4Math::BuildUE4ViewProjectionMatrix(cam, 1920, 1080);
        mem.WritePointer(BASE + 0x20000, VM_OBJ);
        mem.WriteBytes(VM_OBJ + 0x100, vm.data(), sizeof(vm));

        for (int i = 0; i < 270; ++i) {
            const uint64_t ps = 0x7200000000ULL + static_cast<uint64_t>(i) * 0x200ULL;
            const uint64_t pawn = 0x7240000000ULL + static_cast<uint64_t>(i) * 0x200ULL;
            const uint64_t root = 0x7280000000ULL + static_cast<uint64_t>(i) * 0x200ULL;
            mem.WritePointer(ARR + static_cast<uint64_t>(i) * 8ULL, ps);
            mem.WritePointer(ps + 0x2A8, pawn);
            mem.WritePointer(pawn + 0x158, root);
            mem.Write<Vec3>(root + 0x11C, Vec3(1000.0f + i, 0.0f, 50.0f));
        }

        EspEngine engine(mem);
        EXPECT_TRUE(engine.LoadConfigFromString(R"({
            "game": {"package_name": "com.example.gspool", "module_name": "libUE4.so"},
            "entity_provider": {"type": "GameStatePlayer"},
            "globals": {"GWorld_chain": ["0x10000"]},
            "world": {"GameState": "0x140", "PlayerArray": "0x240"},
            "player_state": {"PawnPrivate": "0x2A8"},
            "camera": {"mode": "matrix", "view_matrix_chain": ["0x20000", "+0x100"]},
            "entity_filter": {"mode": "none", "skip_local_player": false},
            "actor": {"RootComponent": "0x158", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(engine.Initialize());
        EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
        EXPECT_EQ(engine.GetStats().filteredEntities, 256U);
        EXPECT_EQ(engine.GetStats().droppedByPoolCap, 14U);
    }

    // 3.8 InternalEngineBridge allows initializing with null GWorld/camera in JSON
    {
        MockMemoryReader mem;
        InternalEngineBridge bridge;
        EspEngine engine(mem);
        engine.SetInternalBridge(&bridge);
        EXPECT_TRUE(engine.LoadConfigFromString(R"({
            "game": {"package_name": "com.example.internal", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "world": {"PersistentLevel": "0x30", "LevelActors": "0x98"},
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(!engine.GetConfig().GetReport().ok());
        EXPECT_TRUE(engine.Initialize());
    }

    // 3.9 Deterministic malformed/fuzz robustness (never throws, never crashes)
    {
        const std::vector<std::string> fuzzInputs = {
            "",
            "{invalid json",
            "[]",
            "null",
            "42",
            "{}",
            R"({"game": 123, "world": [], "camera": true, "actor": "str"})",
            R"({"game": {"package_name": "com.example.f", "module_name": "lib.so"}, "globals": null, "world": null, "camera": null, "actor": null, "bones": null})",
            R"({"game": {"package_name": "com.example.f", "module_name": "lib.so"}, "globals": {"GWorld_chain": [null, true, {}, []]}, "bones": {"enabled": "maybe", "profiles": {"bad": 123}}})"
        };
        for (const auto& s : fuzzInputs) {
            ConfigManager cfg;
            (void)cfg.LoadFromString(s);
            const ConfigReport rep = cfg.Validate();
            EXPECT_TRUE(!rep.ok() || cfg.IsLoaded());
        }
    }

    // 3.10 EspSettings v5 save/load (CaptureMode + touchControl) + v4/v3/v2 migration
    {
        const std::string cfgPath = "/tmp/esp4all_test_settings.cfg";
        {
            EspSettings s;
            s.uiSettingsPath = cfgPath;
            s.drawBox = false;
            s.drawSkeleton = true;
            s.targetFps = 90.0f;
            s.graphicsBackend = 1; // Vulkan
            s.SetCaptureMode(CaptureMode::Secure);
            s.touchControl = true;
            EXPECT_TRUE(s.Save());

            EspSettings loaded;
            loaded.uiSettingsPath = cfgPath;
            EXPECT_TRUE(loaded.Load());
            EXPECT_EQ(loaded.drawBox, false);
            EXPECT_EQ(loaded.drawSkeleton, true);
            EXPECT_NEAR(loaded.targetFps, 90.0f, 0.01f);
            EXPECT_EQ(loaded.graphicsBackend, static_cast<uint8_t>(1));
            EXPECT_EQ(static_cast<uint8_t>(loaded.GetCaptureMode()), static_cast<uint8_t>(CaptureMode::Secure));
            EXPECT_EQ(loaded.touchControl, true);
            EXPECT_EQ(std::string(CaptureModeToString(loaded.GetCaptureMode())), std::string("Secure"));
        }
        // Invalid out-of-range captureMode clamps safely to Visible
        {
            EspSettings::PersistedData bad{};
            bad.magic = EspSettings::MAGIC;
            bad.version = EspSettings::VERSION;
            bad.targetFps = 60.0f;
            bad.graphicsBackend = 99;
            bad.captureMode = 42;
            bad.touchControl = 0;
            std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
            f.close();

            EspSettings clamped;
            clamped.uiSettingsPath = cfgPath;
            EXPECT_TRUE(clamped.Load());
            EXPECT_EQ(clamped.graphicsBackend, static_cast<uint8_t>(0));
            EXPECT_EQ(static_cast<uint8_t>(clamped.GetCaptureMode()), static_cast<uint8_t>(CaptureMode::Visible));
            EXPECT_EQ(clamped.touchControl, false);
        }
        // Write a legacy v4 binary header (captureMode preserved, touchControl defaults to false)
        {
            EspSettings::PersistedDataV4 v4{};
            v4.magic = EspSettings::MAGIC;
            v4.version = 4U;
            v4.drawBox = 1;
            v4.drawLine = 1;
            v4.drawName = 1;
            v4.drawHealth = 1;
            v4.drawDistance = 1;
            v4.drawWorldItems = 1;
            v4.drawSkeleton = 1;
            v4.showChainDebugger = 1;
            v4.targetFps = 90.0f;
            v4.graphicsBackend = 1;
            v4.captureMode = static_cast<uint8_t>(CaptureMode::Secure);
            std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(&v4), sizeof(v4));
            f.close();

            EspSettings migratedV4;
            migratedV4.uiSettingsPath = cfgPath;
            migratedV4.touchControl = true;
            EXPECT_TRUE(migratedV4.Load());
            EXPECT_EQ(migratedV4.graphicsBackend, static_cast<uint8_t>(1));
            EXPECT_EQ(static_cast<uint8_t>(migratedV4.GetCaptureMode()), static_cast<uint8_t>(CaptureMode::Secure));
            EXPECT_EQ(migratedV4.touchControl, false);
        }
        // Write a legacy v3 binary header (permeateRecord=1 -> Hidden; permeateRecord=0 -> Visible)
        {
            EspSettings::PersistedDataV3 v3{};
            v3.magic = EspSettings::MAGIC;
            v3.version = 3U;
            v3.drawBox = 1;
            v3.drawLine = 1;
            v3.drawName = 0;
            v3.drawHealth = 1;
            v3.drawDistance = 1;
            v3.drawWorldItems = 0;
            v3.drawSkeleton = 1;
            v3.showChainDebugger = 1;
            v3.targetFps = 120.0f;
            v3.graphicsBackend = 2; // OpenGL
            v3.permeateRecord = 1;  // -> CaptureMode::Hidden
            std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(&v3), sizeof(v3));
            f.close();

            EspSettings migratedV3;
            migratedV3.uiSettingsPath = cfgPath;
            EXPECT_TRUE(migratedV3.Load());
            EXPECT_EQ(migratedV3.drawName, false);
            EXPECT_EQ(migratedV3.drawWorldItems, false);
            EXPECT_NEAR(migratedV3.targetFps, 120.0f, 0.01f);
            EXPECT_EQ(migratedV3.graphicsBackend, static_cast<uint8_t>(2));
            EXPECT_EQ(static_cast<uint8_t>(migratedV3.GetCaptureMode()), static_cast<uint8_t>(CaptureMode::Hidden));
            EXPECT_EQ(migratedV3.touchControl, false);
        }
        // Write a legacy v2 binary header and ensure v5 loader migrates cleanly
        {
            EspSettings::PersistedDataV2 v2{};
            v2.magic = EspSettings::MAGIC;
            v2.version = 2U;
            v2.drawBox = 1;
            v2.drawLine = 0;
            v2.drawName = 1;
            v2.drawHealth = 1;
            v2.drawDistance = 0;
            v2.drawWorldItems = 1;
            v2.drawSkeleton = 0;
            v2.showChainDebugger = 1;
            v2.targetFps = 75.0f;
            std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(&v2), sizeof(v2));
            f.close();

            EspSettings migrated;
            migrated.uiSettingsPath = cfgPath;
            EXPECT_TRUE(migrated.Load());
            EXPECT_EQ(migrated.drawBox, true);
            EXPECT_EQ(migrated.drawLine, false);
            EXPECT_EQ(migrated.drawDistance, false);
            EXPECT_NEAR(migrated.targetFps, 75.0f, 0.01f);
            EXPECT_EQ(migrated.graphicsBackend, static_cast<uint8_t>(0)); // Auto default
            EXPECT_EQ(static_cast<uint8_t>(migrated.GetCaptureMode()), static_cast<uint8_t>(CaptureMode::Visible));
            EXPECT_EQ(migrated.touchControl, false);
        }
        std::remove(cfgPath.c_str());
    }

    std::cout << "  [OK] Negatives, Pool Overflow (WorldActors + GameStatePlayer), Bridge, Fuzz & Settings v2/v3/v4/v5 passed\n";
}

// =============================================================================
// Suite 4: TouchDevice, TouchParser (Protocol B/A/Single), VirtualFingers & TouchRouter
// =============================================================================
static void TestTouchSubsystem() {
    std::cout << "--- Suite 4: TouchDevice, TouchParser (B/A/Single), VirtualFingers & TouchRouter ---\n";
    using namespace esp4all;
    using namespace std::chrono;

    auto setKeyBit = [](Capabilities& c, uint16_t code) {
        if (c.keys.size() < ev::kKeyCount / 8) {
            c.keys.assign(ev::kKeyCount / 8, 0);
        }
        c.keys[code / 8] |= static_cast<uint8_t>(1U << (code % 8));
    };

    // 4.1 Device classification (Protocol B phone, Protocol A emulator, Single, Mouse, Fingerprint rejection)
    {
        Capabilities phone{};
        phone.name = "fts_ts";
        phone.properties = (1U << ev::kPropDirect);
        phone.axes.assign(ev::kAbsCount, std::nullopt);
        phone.axes[ev::kAbsMtSlot] = Axis{0, 9};
        phone.axes[ev::kAbsMtTrackingId] = Axis{0, 65535};
        phone.axes[ev::kAbsMtPositionX] = Axis{0, 1079};
        phone.axes[ev::kAbsMtPositionY] = Axis{0, 2399};
        setKeyBit(phone, ev::kBtnTouch);

        const DeviceRoles rPhone = DeviceRoles::Classify(phone, IdcConfig{});
        EXPECT_TRUE(rPhone.touch.has_value());
        EXPECT_TRUE(rPhone.touch->protocol == TouchProtocol::Slots());
        EXPECT_TRUE(rPhone.touch->confidence == ScreenConfidence::Certain());

        // Fingerprint sensor named goodix_fp without PROP_DIRECT must be rejected
        Capabilities fp{};
        fp.name = "goodix_fp";
        fp.axes.assign(ev::kAbsCount, std::nullopt);
        fp.axes[ev::kAbsMtPositionX] = Axis{0, 500};
        fp.axes[ev::kAbsMtPositionY] = Axis{0, 500};
        const DeviceRoles rFp = DeviceRoles::Classify(fp, IdcConfig{});
        EXPECT_TRUE(!rFp.touch.has_value());

        // Protocol A emulator (no ABS_MT_SLOT)
        Capabilities emu{};
        emu.name = "qwerty_touch";
        emu.axes.assign(ev::kAbsCount, std::nullopt);
        emu.axes[ev::kAbsMtPositionX] = Axis{0, 32767};
        emu.axes[ev::kAbsMtPositionY] = Axis{0, 32767};
        const DeviceRoles rEmu = DeviceRoles::Classify(emu, IdcConfig{});
        EXPECT_TRUE(rEmu.touch.has_value());
        EXPECT_TRUE(rEmu.touch->protocol.kind == TouchProtocol::Kind::Frames);
        EXPECT_TRUE(!rEmu.touch->protocol.tracked);

        // Emulator absolute mouse (ABS_X/ABS_Y + BTN_LEFT -> pointer, not touch)
        Capabilities mouse{};
        mouse.name = "virtio_tablet";
        mouse.axes.assign(ev::kAbsCount, std::nullopt);
        mouse.axes[ev::kAbsX] = Axis{0, 32767};
        mouse.axes[ev::kAbsY] = Axis{0, 32767};
        setKeyBit(mouse, ev::kBtnLeft);
        setKeyBit(mouse, ev::kBtnTouch);
        const DeviceRoles rMouse = DeviceRoles::Classify(mouse, IdcConfig{});
        EXPECT_TRUE(!rMouse.touch.has_value());
        EXPECT_TRUE(rMouse.pointer.has_value());

        // IDC parsing & filename generation
        const IdcConfig idc = IdcConfig::Parse(
            "# comment\ntouch.deviceType = touchScreen\ntouch.orientation = ORIENTATION_90\ntouch.orientationAware = 1\n");
        EXPECT_TRUE(idc.deviceType.has_value() && *idc.deviceType == "touchScreen");
        EXPECT_EQ(static_cast<int>(idc.orientation), static_cast<int>(DisplayRotation::Deg90));
        EXPECT_TRUE(idc.orientationAware.has_value() && *idc.orientationAware);
    }

    // 4.2 Panel::ToScreen <-> Panel::FromScreen round-trip across all 4 rotations
    {
        TouchPanel panel{};
        panel.orientation = DisplayRotation::Deg0;
        panel.followsDisplay = true;

        const DisplayRotation rots[4] = {
            DisplayRotation::Deg0, DisplayRotation::Deg90, DisplayRotation::Deg180, DisplayRotation::Deg270
        };
        for (DisplayRotation rot : rots) {
            const std::array<float, 2> size =
                (rot == DisplayRotation::Deg90 || rot == DisplayRotation::Deg270)
                    ? std::array<float, 2>{2400.0f, 1080.0f}
                    : std::array<float, 2>{1080.0f, 2400.0f};
            const float u = 0.25f;
            const float v = 0.70f;
            const auto scr = panel.ToScreen(u, v, size, rot);
            const auto back = TouchPanel::FromScreen(scr, size, rot);
            EXPECT_NEAR(back.first, u, 1e-4f);
            EXPECT_NEAR(back.second, v, 1e-4f);
        }
    }

    // 4.3 Protocol B (SlotsParser): multi-finger start/move/end + palm rejection + AxisState::Rewind
    {
        TouchPanel panel{};
        panel.protocol = TouchProtocol::Slots();
        panel.x = Axis{0, 1000};
        panel.y = Axis{0, 2000};
        panel.hasTouchButton = true;
        auto parser = TouchParser::Create(panel);
        std::vector<TouchContact> out;

        // Slot 0 down at (200, 400) with BTN_TOUCH=1
        parser->Feed(RawEvent{ev::kEvKey, ev::kBtnTouch, 1}, nanoseconds(0), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtSlot, 0}, nanoseconds(0), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtTrackingId, 10}, nanoseconds(0), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionX, 200}, nanoseconds(0), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionY, 400}, nanoseconds(0), out);
        parser->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(0), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::Start));
        EXPECT_TRUE(!parser->IsIdle());

        // Slot 1 arrives as palm -> ignored!
        out.clear();
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtSlot, 1}, nanoseconds(1000), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtTrackingId, 11}, nanoseconds(1000), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtToolType, ev::kMtToolPalm}, nanoseconds(1000), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionX, 500}, nanoseconds(1000), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionY, 500}, nanoseconds(1000), out);
        parser->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(1000), out);
        EXPECT_EQ(out.size(), 0U);

        // Slot 0 moves then lifts
        out.clear();
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtSlot, 0}, nanoseconds(2000), out);
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionX, 300}, nanoseconds(2000), out);
        parser->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(2000), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::Move));

        out.clear();
        parser->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtTrackingId, -1}, nanoseconds(3000), out);
        parser->Feed(RawEvent{ev::kEvKey, ev::kBtnTouch, 0}, nanoseconds(3000), out);
        parser->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(3000), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::End));
        EXPECT_TRUE(parser->IsIdle());

        // AxisState::Rewind emits restore sequence + SYN_REPORT
        AxisState before{};
        before.slot = 0;
        before.values.push_back({ev::kAbsMtPositionX, {100, 0}});
        AxisState after{};
        after.slot = 1;
        after.values.push_back({ev::kAbsMtPositionX, {250, 0}});
        std::vector<RawEvent> rewindEv;
        after.Rewind(before, rewindEv);
        EXPECT_TRUE(!rewindEv.empty());
        EXPECT_EQ(rewindEv.back().kind, ev::kEvSyn);
        EXPECT_EQ(rewindEv.back().code, ev::kSynReport);
    }

    // 4.4 Protocol A (FramesParser) untracked fade release + SingleParser
    {
        TouchPanel panelA{};
        panelA.protocol = TouchProtocol::Frames(false);
        panelA.x = Axis{0, 1000};
        panelA.y = Axis{0, 1000};
        auto parserA = TouchParser::Create(panelA);
        std::vector<TouchContact> out;

        parserA->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionX, 500}, milliseconds(0), out);
        parserA->Feed(RawEvent{ev::kEvAbs, ev::kAbsMtPositionY, 500}, milliseconds(0), out);
        parserA->Feed(RawEvent{ev::kEvSyn, ev::kSynMtReport, 0}, milliseconds(0), out);
        parserA->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, milliseconds(0), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::Start));

        // Empty frame starts short fade timeout (40ms), Tick after 50ms ends contact
        out.clear();
        parserA->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, milliseconds(10), out);
        EXPECT_EQ(out.size(), 0U);
        EXPECT_TRUE(parserA->Deadline().has_value());
        parserA->Tick(milliseconds(60), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::End));
        EXPECT_TRUE(parserA->IsIdle());

        // SingleParser
        TouchPanel panelS{};
        panelS.protocol = TouchProtocol::Single();
        panelS.x = Axis{0, 1000};
        panelS.y = Axis{0, 1000};
        panelS.hasTouchButton = true;
        auto parserS = TouchParser::Create(panelS);
        out.clear();
        parserS->Feed(RawEvent{ev::kEvKey, ev::kBtnTouch, 1}, nanoseconds(0), out);
        parserS->Feed(RawEvent{ev::kEvAbs, ev::kAbsX, 250}, nanoseconds(0), out);
        parserS->Feed(RawEvent{ev::kEvAbs, ev::kAbsY, 750}, nanoseconds(0), out);
        parserS->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(0), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::Start));
        out.clear();
        parserS->Feed(RawEvent{ev::kEvKey, ev::kBtnTouch, 0}, nanoseconds(1), out);
        parserS->Feed(RawEvent{ev::kEvSyn, ev::kSynReport, 0}, nanoseconds(1), out);
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(static_cast<int>(out[0].phase), static_cast<int>(TouchPhase::End));
    }

    // 4.5 VirtualFingers + TouchRouter (no menu leak into game; no game aim drag pulling menu) + EventQueue
    {
        VirtualFingers vf;
        std::vector<RawEvent> raw;
        EXPECT_TRUE(vf.Apply(FingerOwner{1, 1}, TouchPhase::Start, {0.5f, 0.5f}, raw));
        EXPECT_TRUE(!vf.IsEmpty());
        EXPECT_TRUE(vf.HoldsDevice(1));
        vf.LiftAll(raw);
        EXPECT_TRUE(vf.IsEmpty());

        InterfaceView view{};
        view.size = {1080.0f, 2400.0f};
        view.regions.push_back(UiRect{100.0f, 100.0f, 600.0f, 800.0f}); // DebugUI box

        TouchRouter router;
        std::vector<InputEvent> routed;

        // Finger 1 lands INSIDE DebugUI -> Interface, stays Interface even if dragged outside!
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{1, TouchPhase::Start, 0, 0}, {200.0f, 200.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Interface));
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{1, TouchPhase::Move, 0, 0}, {900.0f, 1500.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Interface));

        // Finger 2 lands OUTSIDE (in-game aim) -> Outside, stays Outside even if dragged over DebugUI!
        const size_t beforeAim = routed.size();
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{2, TouchPhase::Start, 0, 0}, {950.0f, 1800.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Outside));
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{2, TouchPhase::Move, 0, 0}, {300.0f, 300.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Outside));
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{2, TouchPhase::End, 0, 0}, {300.0f, 300.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Outside));
        // Aim finger emitted ZERO events to Dear ImGui!
        EXPECT_EQ(routed.size(), beforeAim);

        // Lift finger 1
        EXPECT_EQ(static_cast<int>(router.RouteContact(1, TouchContact{1, TouchPhase::End, 0, 0}, {900.0f, 1500.0f}, view, routed)),
                  static_cast<int>(TouchTarget::Interface));

        // EventQueue coalesces consecutive PointerMoved without dropping press/release
        EventQueue q;
        const auto t0 = steady_clock::now();
        q.Push(InputEvent{InputEvent::PointerMoved{{10.0f, 10.0f}}}, t0);
        q.Push(InputEvent{InputEvent::PointerButton{{10.0f, 10.0f}, MouseButton::Left, true}}, t0);
        q.Push(InputEvent{InputEvent::PointerMoved{{20.0f, 20.0f}}}, t0);
        q.Push(InputEvent{InputEvent::PointerMoved{{30.0f, 30.0f}}}, t0);
        q.Push(InputEvent{InputEvent::PointerButton{{30.0f, 30.0f}, MouseButton::Left, false}}, t0);

        // Frame 1 ends right after press so ImGui registers click before drag/release
        std::vector<InputEvent> f1, f2, f3;
        EXPECT_TRUE(q.TakeFrame(f1).has_value());
        EXPECT_EQ(f1.size(), 2U); // Moved(10,10) + ButtonDown
        EXPECT_TRUE(q.TakeFrame(f2).has_value());
        EXPECT_EQ(f2.size(), 1U); // Coalesced Moved(30,30) before release
        EXPECT_TRUE(q.TakeFrame(f3).has_value());
        EXPECT_EQ(f3.size(), 1U); // ButtonUp
        EXPECT_TRUE(q.IsEmpty());
    }

    // 4.6 FrameHasher (FNV-1a dirty-frame fingerprint determinism, sensitivity & non-zero sentinel)
    {
        FrameHasher h1;
        FrameHasher h2;
        const float vtx[4] = {10.0f, 20.0f, 0.5f, 0.25f};
        h1.MixF32(1080.0f);
        h1.MixF32(2400.0f);
        h1.MixBytes(vtx, sizeof(vtx));
        h1.MixU64(42U);

        h2.MixF32(1080.0f);
        h2.MixF32(2400.0f);
        h2.MixBytes(vtx, sizeof(vtx));
        h2.MixU64(42U);

        EXPECT_EQ(h1.Finish(), h2.Finish());
        EXPECT_TRUE(h1.Finish() != 0ULL);

        FrameHasher h3;
        const float vtxChanged[4] = {10.0f, 21.0f, 0.5f, 0.25f};
        h3.MixF32(1080.0f);
        h3.MixF32(2400.0f);
        h3.MixBytes(vtxChanged, sizeof(vtxChanged));
        h3.MixU64(42U);
        EXPECT_TRUE(h3.Finish() != h1.Finish());
        EXPECT_TRUE(h3.Finish() != 0ULL);
    }

    std::cout << "  [OK] TouchDevice, TouchParser (B/A/Single), VirtualFingers, TouchRouter, EventQueue & FrameHasher passed\n";
}

// =============================================================================
// Suite 5: NetDriver LocalPlayer Chain + Data-Driven InternalBridge Getters
// =============================================================================
static void TestNetDriverAndInternalBridge() {
    std::cout << "--- Suite 5: NetDriver Chain & Data-Driven InternalBridge Getters ---\n";

    // 5.1 UWorld -> NetDriver -> ServerConnection -> PlayerController -> Pawn + CameraCache
    // (No OwningGameInstance / LocalPlayers configured — works identically in External & Internal!)
    {
        constexpr uint64_t BASE        = 0x7000000000ULL;
        constexpr uint64_t UWORLD      = 0x7100000000ULL;
        constexpr uint64_t LEVEL       = 0x7100010000ULL;
        constexpr uint64_t ACTORS_DATA = 0x7100020000ULL;
        constexpr uint64_t NET_DRIVER  = 0x7100030000ULL;
        constexpr uint64_t SERVER_CONN = 0x7100040000ULL;
        constexpr uint64_t PC          = 0x7100050000ULL;
        constexpr uint64_t CAM_MGR     = 0x7100060000ULL;
        constexpr uint64_t LOCAL_PAWN  = 0x7100070000ULL;
        constexpr uint64_t LOCAL_ROOT  = 0x7100071000ULL;
        constexpr uint64_t ENEMY_PAWN  = 0x7100080000ULL;
        constexpr uint64_t ENEMY_ROOT  = 0x7100081000ULL;

        MockMemoryReader mem(BASE);
        mem.WritePointer(BASE + 0x10000, UWORLD);
        mem.WritePointer(UWORLD + 0x30, LEVEL);
        mem.WritePointer(LEVEL + 0x98, ACTORS_DATA);
        mem.Write<int32_t>(LEVEL + 0x98 + 0x8, 2);
        mem.WritePointer(ACTORS_DATA + 0x0, LOCAL_PAWN);
        mem.WritePointer(ACTORS_DATA + 0x8, ENEMY_PAWN);

        // NetDriver -> ServerConnection -> PlayerController -> AcknowledgedPawn / CameraManager
        mem.WritePointer(UWORLD + 0x38, NET_DRIVER);
        mem.WritePointer(NET_DRIVER + 0x78, SERVER_CONN);
        mem.WritePointer(SERVER_CONN + 0x30, PC);
        mem.WritePointer(PC + 0x2A0, LOCAL_PAWN);
        mem.WritePointer(PC + 0x2F8, CAM_MGR);

        // CameraCachePrivate + POV
        FMinimalViewInfo pov;
        pov.Location = Vec3(0.0f, 0.0f, 100.0f);
        pov.Rotation = Vec3(0.0f, 0.0f, 0.0f);
        pov.FOV = 90.0f;
        const uint64_t povBase = CAM_MGR + 0x1AE0 + 0x10;
        mem.Write<Vec3>(povBase + 0x0, pov.Location);
        mem.Write<Vec3>(povBase + 0xC, pov.Rotation);
        mem.Write<float>(povBase + 0x18, pov.FOV);

        // Local & Enemy positions
        mem.WritePointer(LOCAL_PAWN + 0x158, LOCAL_ROOT);
        mem.Write<Vec3>(LOCAL_ROOT + 0x11C, Vec3(0.0f, 0.0f, 50.0f));
        mem.WritePointer(ENEMY_PAWN + 0x158, ENEMY_ROOT);
        mem.Write<Vec3>(ENEMY_ROOT + 0x11C, Vec3(1200.0f, 0.0f, 50.0f));

        // Note: ServerConnection_PlayerController omitted intentionally -> falls back to LocalPlayer_PlayerController (0x30)
        EspEngine engine(mem);
        EXPECT_TRUE(engine.LoadConfigFromString(R"({
            "game": {"package_name": "com.example.netdrv", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "globals": {"GWorld_chain": ["0x10000"]},
            "world": {"PersistentLevel": "0x30", "NetDriver": "0x38", "LevelActors": "0x98"},
            "local_player": {
                "LocalPlayer_PlayerController": "0x30",
                "NetDriver_ServerConnection": "0x78",
                "PlayerController_Pawn": "0x2A0",
                "PlayerController_CameraManager": "0x2F8"
            },
            "camera": {
                "mode": "camera_cache",
                "camera_cache": {"CameraCache": "0x1AE0", "POV": "0x10"}
            },
            "entity_filter": {
                "mode": "none",
                "max_distance": 300.0,
                "max_z_delta": 5000.0,
                "skip_local_player": true
            },
            "actor": {"RootComponent": "0x158", "WorldPosition": "0x11C"}
        })"));
        EXPECT_TRUE(engine.GetConfig().GetReport().ok());
        EXPECT_EQ(engine.GetConfig().GetReport().warnCount(), 0U);
        EXPECT_TRUE(engine.Initialize());

        EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);
        const auto& diag = engine.GetContext().GetDiagnostics();
        EXPECT_TRUE(diag.usedNetDriverLocalPlayer);
        EXPECT_EQ(diag.localController, PC);
        EXPECT_EQ(diag.localPawn, LOCAL_PAWN);
        EXPECT_TRUE(diag.hasLocalPos);
        EXPECT_TRUE(diag.hasCameraPOV);
        EXPECT_EQ(engine.GetStats().filteredEntities, 1U); // LocalPawn skipped, Enemy kept
        EXPECT_EQ(engine.GetStats().drawnPlayers, 1U);
    }

    // 5.2 Data-driven InternalEngineBridge: PushUWorld + PushNamePool + SetActorsArrayResolver
    // (Simulates in-process GetGNames() + GetActorArray(ULevel*) + GUObjectArray->World with 0 heap allocs)
    {
        constexpr uint64_t BASE         = 0x7000000000ULL;
        constexpr uint64_t UWORLD       = 0x7100000000ULL;
        constexpr uint64_t LEVEL        = 0x7100010000ULL;
        constexpr uint64_t DECRYPT_DATA = 0x7100020000ULL;
        constexpr uint64_t GNAMES_POOL  = 0x7100030000ULL;
        constexpr uint64_t GNAMES_BLK0  = 0x7100031000ULL;
        constexpr uint64_t ENTRY_PLAYER = 0x7100032000ULL;
        constexpr uint64_t ENTRY_TREE   = 0x7100032100ULL;
        constexpr uint64_t ACTOR_PLAYER = 0x7100040000ULL;
        constexpr uint64_t ROOT_PLAYER  = 0x7100041000ULL;
        constexpr uint64_t ACTOR_TREE   = 0x7100050000ULL;
        constexpr uint64_t ROOT_TREE    = 0x7100051000ULL;

        MockMemoryReader mem(BASE);
        mem.WritePointer(UWORLD + 0x30, LEVEL);

        // Decrypted actor pointer buffer returned by simulated GetActorArray(ULevel*)
        mem.WritePointer(DECRYPT_DATA + 0x0, ACTOR_PLAYER);
        mem.WritePointer(DECRYPT_DATA + 0x8, ACTOR_TREE);

        // Legacy GNames (TNameEntryArray) chunk 0 -> entries 10 ("BP_PlayerPawn_C") and 11 ("BP_Tree_C")
        mem.WritePointer(GNAMES_POOL + 0x0, GNAMES_BLK0);
        mem.WritePointer(GNAMES_BLK0 + 10ULL * 8ULL, ENTRY_PLAYER);
        mem.WritePointer(GNAMES_BLK0 + 11ULL * 8ULL, ENTRY_TREE);
        const char kPlayerName[] = "BP_PlayerPawn_C";
        const char kTreeName[]   = "BP_Tree_C";
        mem.WriteBytes(ENTRY_PLAYER + 0xC, kPlayerName, sizeof(kPlayerName));
        mem.WriteBytes(ENTRY_TREE   + 0xC, kTreeName,   sizeof(kTreeName));

        mem.Write<uint32_t>(ACTOR_PLAYER + 0x18, 10U);
        mem.WritePointer(ACTOR_PLAYER + 0x1B0, ROOT_PLAYER);
        mem.Write<Vec3>(ROOT_PLAYER + 0x11C, Vec3(1400.0f, 50.0f, 50.0f));

        mem.Write<uint32_t>(ACTOR_TREE + 0x18, 11U);
        mem.WritePointer(ACTOR_TREE + 0x1B0, ROOT_TREE);
        mem.Write<Vec3>(ROOT_TREE + 0x11C, Vec3(1000.0f, 0.0f, 50.0f));

        struct FakeGetterCtx {
            uint64_t expectedLevel;
            uint64_t decryptedData;
            int32_t decryptedCount;
        } getterCtx{LEVEL, DECRYPT_DATA, 2};

        auto fakeActorsResolver = [](uint64_t levelPtr,
                                     uint64_t& outData,
                                     int32_t& outCount,
                                     void* userData) noexcept -> bool {
            const auto* ctx = static_cast<const FakeGetterCtx*>(userData);
            if (ctx == nullptr || levelPtr != ctx->expectedLevel) {
                return false;
            }
            outData = ctx->decryptedData;
            outCount = ctx->decryptedCount;
            return true;
        };

        InternalEngineBridge bridge;
        bridge.SetMaxStaleMs(0);
        bridge.PushUWorld(UWORLD);
        bridge.PushNamePool(GNAMES_POOL);
        bridge.SetActorsArrayResolver(fakeActorsResolver, &getterCtx);

        FMinimalViewInfo pov;
        pov.Location = Vec3(0.0f, 0.0f, 100.0f);
        pov.Rotation = Vec3(0.0f, 0.0f, 0.0f);
        pov.FOV = 90.0f;
        bridge.PushCameraPOV(pov);

        EspEngine engine(mem);
        engine.SetInternalBridge(&bridge);
        EXPECT_TRUE(engine.LoadConfigFromString(R"({
            "game": {"package_name": "com.example.internal", "module_name": "libUE4.so"},
            "entity_provider": {"type": "WorldActors"},
            "engine_path": {"ViewportWorld": "0x70"},
            "world": {"PersistentLevel": "0x30", "LevelActors": null},
            "camera": {"mode": "camera_cache", "camera_cache": {"CameraCache": "0x1AE0"}},
            "local_player": {"PlayerController_CameraManager": "0x2F8"},
            "name_system": {"type": "GNames", "ChunkSize": "0x4000", "EntryStride": "0x8", "NameStringOffset": "0xC"},
            "entity_filter": {
                "mode": "class_name",
                "skip_local_player": false,
                "class_name": {"contains": ["PlayerPawn"]}
            },
            "actor": {"RootComponent": "0x1B0", "WorldPosition": "0x11C"},
            "internal": {
                "GNames_getter": "0x123400",
                "GetActorArray_fn": "0x234500",
                "GUObjectArray": "0x345600",
                "safe_memory_read": true
            }
        })"));

        // Schema & semantic validation must pass with 0 errors and 0 warnings (info notes only)
        const auto& rep = engine.GetConfig().GetReport();
        EXPECT_TRUE(rep.ok());
        EXPECT_EQ(rep.warnCount(), 0U);
        EXPECT_TRUE(HasIssue(rep, "I_INTERNAL_ONLY", "internal.GUObjectArray"));
        EXPECT_TRUE(HasIssue(rep, "I_INTERNAL_ONLY", "internal.GetActorArray_fn"));
        EXPECT_TRUE(HasIssue(rep, "I_INTERNAL_ONLY", "internal.GNames_getter"));
        EXPECT_EQ(engine.GetConfig().GetInternalConfig().gNamesGetterOffset, 0x123400ULL);
        EXPECT_EQ(engine.GetConfig().GetInternalConfig().getActorArrayFnOffset, 0x234500ULL);
        EXPECT_EQ(engine.GetConfig().GetInternalConfig().gUObjectArrayOffset, 0x345600ULL);
        EXPECT_TRUE(engine.GetConfig().GetInternalConfig().safeMemoryRead);

        // DumpEffective must include new world.NetDriver, local_player, and internal fields
        const std::string effective = engine.GetConfig().DumpEffective();
        EXPECT_TRUE(effective.find("\"NetDriver\"") != std::string::npos);
        EXPECT_TRUE(effective.find("\"GetActorArray_fn\"") != std::string::npos);

        EXPECT_TRUE(engine.Initialize());
        EXPECT_EQ(RunEngineZeroAllocCheck(engine), 0U);

        const auto& diag = engine.GetContext().GetDiagnostics();
        EXPECT_TRUE(diag.usedBridgeWorld);
        EXPECT_TRUE(diag.usedBridgeNamePool);
        EXPECT_TRUE(diag.usedBridgeActors);
        EXPECT_EQ(engine.GetStats().totalRawActors, 2U);
        EXPECT_EQ(engine.GetStats().filteredEntities, 1U);
        EXPECT_EQ(engine.GetStats().drawnPlayers, 1U);
        EXPECT_EQ(std::string(engine.GetPool()[0].className), std::string("BP_PlayerPawn_C"));
    }

    // 5.3 DirectMemoryReader mode toggle & live in-process memory read
    {
        uint64_t sample = 0x1122334455667788ULL;
        DirectMemoryReader reader(DirectReadMode::SAFE_SELF_VM);
        EXPECT_EQ(std::string(DirectReadModeToString(reader.GetMode())), std::string("SAFE_SELF_VM"));
        EXPECT_EQ(reader.Read<uint64_t>(reinterpret_cast<uintptr_t>(&sample)), sample);

        reader.CycleMode();
        EXPECT_EQ(std::string(DirectReadModeToString(reader.GetMode())), std::string("DIRECT_MEMCPY"));
        EXPECT_EQ(reader.Read<uint64_t>(reinterpret_cast<uintptr_t>(&sample)), sample);

        // Unaligned / unattached address is rejected by IsExecutableAddress safety guard
        EXPECT_TRUE(!reader.IsExecutableAddress(reinterpret_cast<uintptr_t>(&sample)));
    }

    std::cout << "  [OK] NetDriver chain, InternalBridge (GNames + ActorsResolver, 0 allocs) & DirectMemoryReader passed\n";
}

int main(int argc, char** argv) {
    const std::string tplDir = (argc > 1 && argv[1][0] != '\0')
        ? std::string(argv[1])
        : ResolveTemplatesDir();

    TestTemplateLint(tplDir);
    TestGoldenMinimalWorldActors(tplDir);
    TestGoldenMinimalGameStatePlayer(tplDir);
    TestGoldenMinimalEntityListYUp(tplDir);
    TestGoldenFullWorldActors(tplDir);
    TestGoldenFullGameStatePlayer(tplDir);
    TestValidatorNegativesAndPoolOverflow();
    TestTouchSubsystem();
    TestNetDriverAndInternalBridge();

    std::cout << "\n========================================\n"
              << "Assertions passed: " << gAssertionsPassed
              << " | failed: " << gAssertionsFailed << "\n"
              << "========================================\n";

    return gAssertionsFailed == 0 ? 0 : 1;
}
