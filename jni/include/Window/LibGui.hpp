#pragma once

#include "Window/CaptureMode.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace esp4all {

using GuiPtr = void*;

struct RawDisplayState {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t rotation = 0;
    float refreshRate = 0.0f;
};

struct SymbolPick {
    const char* what = "";
    const char* symbol = "";
};

/**
 * Symbol-probing bridge to Android's private SurfaceFlinger client library (libgui.so / libutils.so).
 *
 * Resolves every ABI entry point by actual symbol presence (newest -> oldest) rather than brittle
 * Android release-number switches, supporting Android 5 through Android 17 (API 21–37+) and OEM ROMs.
 */
class LibGui {
public:
    static const LibGui* Get(std::string* outError = nullptr);

    uint32_t GetApiLevel() const noexcept { return apiLevel_; }
    const std::vector<SymbolPick>& GetPicks() const noexcept { return picks_; }

    void Retain(GuiPtr object) const;
    void Release(GuiPtr object) const;

    GuiPtr Connect(std::string* outError = nullptr) const;
    GuiPtr GetDisplayToken(uint64_t* outPhysicalId = nullptr, std::string* outError = nullptr) const;
    bool GetDisplayState(GuiPtr token, RawDisplayState& outState, std::string* outError = nullptr) const;
    GuiPtr NewTransaction(std::string* outError = nullptr) const;

    GuiPtr CreateSurface(
        GuiPtr client,
        const char* name,
        uint32_t width,
        uint32_t height,
        CaptureMode capture,
        std::string* outError = nullptr) const;

    GuiPtr GetSurface(GuiPtr control, std::string* outError = nullptr) const;
    void Disconnect(GuiPtr control) const;

    bool Present(GuiPtr transaction, GuiPtr control, int32_t zOrder, std::string* outError = nullptr) const;
    bool SetVisible(GuiPtr transaction, GuiPtr control, bool visible, std::string* outError = nullptr) const;

    // Resizes a layer in place when supported by the system's libgui.
    // Sets outResized=false when the system lacks an in-place resize call.
    bool Resize(
        GuiPtr transaction,
        GuiPtr control,
        uint32_t width,
        uint32_t height,
        bool& outResized,
        std::string* outError = nullptr) const;

    static uint32_t ReadSystemApiLevel();
    static float ReadSystemDensity();

private:
    // Non-trivial C++ return types (sp<T>, std::vector<T>) use indirect sret (x8 on ARM64).
    // Padding to >16 bytes guarantees indirect return on ARM64/ARM32/x86_64/x86 Android ABIs.
    template <typename T>
    struct Returned {
        T value;
        size_t pad[3];
    };

    using RetainCall = void (*)(const void*, const void*);
    using Method = void (*)(GuiPtr);
    using StatusCall = int32_t (*)(GuiPtr);
    using GetShared = Returned<GuiPtr> (*)();
    using SharedMethod = Returned<GuiPtr> (*)(GuiPtr);
    using TxControl = GuiPtr (*)(GuiPtr, const GuiPtr*);
    using TxInt = GuiPtr (*)(GuiPtr, const GuiPtr*, int32_t);
    using TxSize = GuiPtr (*)(GuiPtr, const GuiPtr*, uint32_t, uint32_t);
    using TxBool = GuiPtr (*)(GuiPtr, const GuiPtr*, bool);
    using DisplayIdsCall = Returned<std::array<GuiPtr, 3>> (*)();
    using QueryCall = int32_t (*)(const GuiPtr*, uint8_t*);

    enum class ParentKind : uint8_t { Control, Handle };
    enum class CreateKind : uint8_t {
        Plain,
        WindowType,
        Metadata,
        MetadataHint,
        CheckedWindowType,
        CheckedMetadata,
        CheckedMetadataHint
    };

    struct CreateVariant {
        const char* symbol;
        CreateKind kind;
        ParentKind parent;
        int32_t defaultWindowType;
    };

    struct TokenOps {
        GetShared internal = nullptr;
        DisplayIdsCall ids = nullptr;
        Returned<GuiPtr> (*physical)(uint64_t) = nullptr;
        Returned<GuiPtr> (*builtIn)(int32_t) = nullptr;
    };

    struct TransactOps {
        GuiPtr (*createNdk)() = nullptr;
        Method construct = nullptr;
        void (*constructCopy)(GuiPtr, GuiPtr) = nullptr;
        TxInt setLayer = nullptr;
        TxControl show = nullptr;
        TxControl hide = nullptr;
        TxSize setSize = nullptr;
        TxBool trustedFlag = nullptr;
        TxInt trustedMode = nullptr;
        int32_t (*apply)(GuiPtr, bool) = nullptr;
        int32_t (*applyOneWay)(GuiPtr, bool, bool) = nullptr;
    };

    struct GlobalOps {
        void (*open)() = nullptr;
        void (*close)(bool) = nullptr;
        int32_t (*setLayer)(GuiPtr, int32_t) = nullptr;
        StatusCall show = nullptr;
        StatusCall hide = nullptr;
        int32_t (*setSize)(GuiPtr, uint32_t, uint32_t) = nullptr;
    };

    class Resolver;

    LibGui() = default;
    static bool Load(LibGui& outGui, std::string& outError);
    static void StartBinderThreads();

    template <typename ConstructFn>
    static GuiPtr ConstructInPlace(const char* what, ConstructFn construct, std::string* outError);

    bool ResolveToken(Resolver& gui, std::string& outError);
    bool ResolveOps(Resolver& gui, std::string& outError);
    bool ApplyTransaction(GuiPtr transaction, bool wait, std::string* outError) const;

    static const CreateVariant kCreateVariants[];
    static const size_t kCreateVariantCount;

    uint32_t apiLevel_ = 0;
    std::vector<SymbolPick> picks_;

    RetainCall retain_ = nullptr;
    RetainCall release_ = nullptr;
    void (*string8New_)(GuiPtr, const char*) = nullptr;
    Method string8Drop_ = nullptr;

    GetShared clientDefault_ = nullptr;
    Method clientNew_ = nullptr;
    StatusCall clientCheck_ = nullptr;

    void* createFn_ = nullptr;
    CreateVariant createVariant_{nullptr, CreateKind::Plain, ParentKind::Control, 0};
    Method metadataNew_ = nullptr;
    void (*metadataSet_)(GuiPtr, uint32_t, int32_t) = nullptr;

    StatusCall validate_ = nullptr;
    SharedMethod getSurface_ = nullptr;
    Method disconnect_ = nullptr;
    void (*updateSize_)(GuiPtr, uint32_t, uint32_t) = nullptr;

    TokenOps token_{};
    QueryCall displayQuery_ = nullptr;
    bool displayQueryIsModern_ = false;

    bool hasTransact_ = false;
    TransactOps transact_{};
    bool hasGlobal_ = false;
    GlobalOps global_{};
};

} // namespace esp4all
