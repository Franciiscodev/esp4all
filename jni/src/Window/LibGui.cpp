#include "Window/LibGui.hpp"

#include <algorithm>
#include <android/log.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <utility>

namespace esp4all {

namespace {

constexpr const char* kLogTag = "ESP4AllWindow";

constexpr int32_t kFormatRgba8888 = 1;
constexpr uint32_t kFlagSkipScreenshot = 0x40;
constexpr uint32_t kFlagSecure = 0x80;
constexpr int32_t kWindowTypeDontScreenshot = 441731;
constexpr uint32_t kMetadataWindowType = 2;
constexpr int32_t kTrustedOverlayEnabled = 2;

constexpr size_t kInPlaceObjectSize = 4096;
constexpr size_t kInPlaceGuardSize = 32;
constexpr unsigned char kGuardByte = 0xA5;

template <size_t N>
struct AlignedStorage {
    alignas(16) unsigned char bytes[N] = {};

    GuiPtr Data() noexcept { return bytes; }
};

void SetError(std::string* outError, const std::string& msg) {
    if (outError) {
        *outError = msg;
    }
}

std::string FormatStatusError(const char* prefix, int32_t status) {
    char buf[192] = {};
    std::snprintf(buf, sizeof(buf), "%s (status %d)", prefix, status);
    return buf;
}

bool ReadPropertyString(const char* propName, std::string& outValue) {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get(propName, value);
    if (len <= 0) {
        return false;
    }
    const size_t safeLen = std::min<size_t>(static_cast<size_t>(len), PROP_VALUE_MAX);
    outValue.assign(value, safeLen);
    return true;
}

bool ReadPropertyDouble(const char* propName, double& outValue) {
    std::string text;
    if (!ReadPropertyString(propName, text)) {
        return false;
    }
    char* end = nullptr;
    const double val = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(val)) {
        return false;
    }
    outValue = val;
    return true;
}

void* OpenSystemLibrary(const char* name, std::string* outError = nullptr) {
    void* handle = dlopen(name, RTLD_NOW);
    if (!handle && outError) {
        const char* err = dlerror();
        *outError = std::string("dlopen \"") + name + "\" failed: " + (err ? err : "unknown error");
    }
    return handle;
}

} // namespace

// Ordered newest -> oldest (Android 17 / API 37 down to Android 5 / API 21).
// Includes both const-ref (Android 16 QPR / 17) and by-value LayerMetadata variants.
const LibGui::CreateVariant LibGui::kCreateVariants[] = {
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEiRKNS4_INS_7IBinderEEERKNS_3gui13LayerMetadataEPj",
     CreateKind::CheckedMetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEiRKNS4_INS_7IBinderEEENS_3gui13LayerMetadataEPj",
     CreateKind::CheckedMetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjRKNS4_INS_7IBinderEEENS_13LayerMetadataEPj",
     CreateKind::CheckedMetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_NS_13LayerMetadataEPj",
     CreateKind::CheckedMetadataHint, ParentKind::Control, 0},
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_NS_13LayerMetadataE",
     CreateKind::CheckedMetadata, ParentKind::Control, 0},
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_ii",
     CreateKind::CheckedWindowType, ParentKind::Control, -1},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEERKNS_3gui13LayerMetadataEPj",
     CreateKind::MetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj",
     CreateKind::MetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEENS_13LayerMetadataEPj",
     CreateKind::MetadataHint, ParentKind::Handle, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataEPj",
     CreateKind::MetadataHint, ParentKind::Control, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataE",
     CreateKind::Metadata, ParentKind::Control, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEii",
     CreateKind::WindowType, ParentKind::Control, -1},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEjj",
     CreateKind::WindowType, ParentKind::Control, 0},
    {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8Ejjij",
     CreateKind::Plain, ParentKind::Control, 0}
};

const size_t LibGui::kCreateVariantCount = sizeof(LibGui::kCreateVariants) / sizeof(LibGui::kCreateVariants[0]);

class LibGui::Resolver {
public:
    Resolver(void* handle, std::vector<SymbolPick>& picks)
        : handle_(handle), picks_(picks) {}

    void* Find(const char* symbol) const {
        return dlsym(handle_, symbol);
    }

    void Note(const char* what, const char* symbol) {
        picks_.push_back(SymbolPick{what, symbol});
    }

    template <typename F>
    bool Optional(F& target, const char* what, std::initializer_list<const char*> symbols) {
        for (const char* symbol : symbols) {
            if (void* address = Find(symbol)) {
                target = reinterpret_cast<F>(address);
                Note(what, symbol);
                return true;
            }
        }
        return false;
    }

    template <typename F>
    bool Required(F& target, const char* what, std::initializer_list<const char*> symbols, std::string& outError) {
        if (Optional(target, what, symbols)) {
            return true;
        }
        outError = std::string(what) + " is missing from the system library";
        return false;
    }

private:
    void* handle_;
    std::vector<SymbolPick>& picks_;
};

uint32_t LibGui::ReadSystemApiLevel() {
    double sdk = 0.0;
    if (!ReadPropertyDouble("ro.build.version.sdk", sdk) || sdk <= 0.0) {
        return 0U;
    }
    return static_cast<uint32_t>(sdk);
}

float LibGui::ReadSystemDensity() {
    double dpi = 0.0;
    if (!ReadPropertyDouble("ro.sf.lcd_density", dpi) || dpi <= 0.0) {
        if (!ReadPropertyDouble("qemu.sf.lcd_density", dpi) || dpi <= 0.0) {
            dpi = 160.0;
        }
    }
    return std::clamp(static_cast<float>(dpi / 160.0), 0.5f, 8.0f);
}

template <typename ConstructFn>
GuiPtr LibGui::ConstructInPlace(const char* what, ConstructFn construct, std::string* outError) {
    void* object = nullptr;
    if (::posix_memalign(&object, 16, kInPlaceObjectSize + kInPlaceGuardSize) != 0 || !object) {
        SetError(outError, "out of memory allocating in-place libgui object");
        return nullptr;
    }
    std::memset(object, 0, kInPlaceObjectSize);
    auto* guard = static_cast<unsigned char*>(object) + kInPlaceObjectSize;
    std::memset(guard, kGuardByte, kInPlaceGuardSize);

    construct(object);

    for (size_t i = 0; i < kInPlaceGuardSize; ++i) {
        if (guard[i] != kGuardByte) {
            SetError(outError, std::string(what) + " exceeded in-place storage guard on this system");
            return nullptr;
        }
    }
    return object;
}

void LibGui::StartBinderThreads() {
    void* binder = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!binder) {
        return;
    }
    using StartPoolFn = void (*)();
    auto startFn = reinterpret_cast<StartPoolFn>(dlsym(binder, "ABinderProcess_startThreadPool"));
    if (startFn) {
        startFn();
    }
}

const LibGui* LibGui::Get(std::string* outError) {
    struct CachedInstance {
        bool loaded = false;
        LibGui gui;
        std::string error;

        CachedInstance() {
            loaded = LibGui::Load(gui, error);
        }
    };

    // Intentionally immortal so late teardown never runs after libgui/libutils unload.
    static const CachedInstance* instance = new CachedInstance();
    if (!instance->loaded) {
        SetError(outError, instance->error);
        return nullptr;
    }
    return &instance->gui;
}

bool LibGui::Load(LibGui& gui, std::string& outError) {
    gui.apiLevel_ = ReadSystemApiLevel();

    void* utilsHandle = OpenSystemLibrary("libutils.so", &outError);
    if (!utilsHandle) {
        return false;
    }
    void* guiHandle = OpenSystemLibrary("libgui.so", &outError);
    if (!guiHandle) {
        return false;
    }

    Resolver utils(utilsHandle, gui.picks_);
    Resolver lib(guiHandle, gui.picks_);

    if (!utils.Required(gui.retain_, "RefBase::incStrong",
                        {"_ZNK7android7RefBase9incStrongEPKv"}, outError) ||
        !utils.Required(gui.release_, "RefBase::decStrong",
                        {"_ZNK7android7RefBase9decStrongEPKv"}, outError) ||
        !utils.Required(gui.string8New_, "String8::String8",
                        {"_ZN7android7String8C1EPKc", "_ZN7android7String8C2EPKc"}, outError) ||
        !utils.Required(gui.string8Drop_, "String8::~String8",
                        {"_ZN7android7String8D1Ev", "_ZN7android7String8D2Ev"}, outError)) {
        return false;
    }

    lib.Optional(gui.clientDefault_, "SurfaceComposerClient::getDefault",
                 {"_ZN7android21SurfaceComposerClient10getDefaultEv"});
    lib.Optional(gui.clientNew_, "SurfaceComposerClient::SurfaceComposerClient",
                 {"_ZN7android21SurfaceComposerClientC1Ev", "_ZN7android21SurfaceComposerClientC2Ev"});
    if (!gui.clientDefault_ && !gui.clientNew_) {
        outError = "SurfaceComposerClient cannot be constructed on this system";
        return false;
    }
    lib.Optional(gui.clientCheck_, "SurfaceComposerClient::initCheck",
                 {"_ZNK7android21SurfaceComposerClient9initCheckEv"});

    for (size_t i = 0; i < kCreateVariantCount; ++i) {
        const CreateVariant& variant = kCreateVariants[i];
        if (void* address = lib.Find(variant.symbol)) {
            gui.createFn_ = address;
            gui.createVariant_ = variant;
            lib.Note("SurfaceComposerClient::createSurface", variant.symbol);
            break;
        }
    }
    if (!gui.createFn_) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf),
                      "no known SurfaceComposerClient::createSurface variant (API %u)",
                      gui.apiLevel_);
        outError = buf;
        return false;
    }

    lib.Optional(gui.metadataNew_, "LayerMetadata::LayerMetadata",
                 {"_ZN7android3gui13LayerMetadataC1Ev",
                  "_ZN7android3gui13LayerMetadataC2Ev",
                  "_ZN7android13LayerMetadataC1Ev",
                  "_ZN7android13LayerMetadataC2Ev"});
    lib.Optional(gui.metadataSet_, "LayerMetadata::setInt32",
                 {"_ZN7android3gui13LayerMetadata8setInt32Eji",
                  "_ZN7android13LayerMetadata8setInt32Eji"});

    const CreateKind kind = gui.createVariant_.kind;
    const bool needsMetadata =
        (kind == CreateKind::Metadata ||
         kind == CreateKind::MetadataHint ||
         kind == CreateKind::CheckedMetadata ||
         kind == CreateKind::CheckedMetadataHint);
    if (needsMetadata && !gui.metadataNew_) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf), "LayerMetadata constructor is missing (API %u)", gui.apiLevel_);
        outError = buf;
        return false;
    }

    lib.Optional(gui.validate_, "SurfaceControl::validate",
                 {"_ZNK7android14SurfaceControl8validateEv"});
    if (!lib.Required(gui.getSurface_, "SurfaceControl::getSurface",
                      {"_ZN7android14SurfaceControl10getSurfaceEv",
                       "_ZNK7android14SurfaceControl10getSurfaceEv"}, outError)) {
        return false;
    }
    lib.Optional(gui.disconnect_, "SurfaceControl::disconnect",
                 {"_ZN7android14SurfaceControl10disconnectEv"});
    lib.Optional(gui.updateSize_, "SurfaceControl::updateDefaultBufferSize",
                 {"_ZN7android14SurfaceControl23updateDefaultBufferSizeEjj"});

    if (!gui.ResolveToken(lib, outError)) {
        return false;
    }

    gui.displayQueryIsModern_ = lib.Optional(
        gui.displayQuery_,
        "SurfaceComposerClient::getDisplayState",
        {"_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE"});
    if (!gui.displayQueryIsModern_) {
        if (!lib.Required(
                gui.displayQuery_,
                "SurfaceComposerClient::getDisplayInfo",
                {"_ZN7android21SurfaceComposerClient14getDisplayInfoERKNS_2spINS_7IBinderEEEPNS_11DisplayInfoE"},
                outError)) {
            return false;
        }
    }

    if (!gui.ResolveOps(lib, outError)) {
        return false;
    }

    return true;
}

bool LibGui::ResolveToken(Resolver& gui, std::string& outError) {
    gui.Optional(token_.ids, "SurfaceComposerClient::getPhysicalDisplayIds",
                 {"_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv"});
    if (gui.Optional(token_.internal, "SurfaceComposerClient::getInternalDisplayToken",
                     {"_ZN7android21SurfaceComposerClient23getInternalDisplayTokenEv"})) {
        return true;
    }
    if (token_.ids &&
        gui.Optional(token_.physical, "SurfaceComposerClient::getPhysicalDisplayToken",
                     {"_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE"})) {
        return true;
    }
    return gui.Required(token_.builtIn, "SurfaceComposerClient::getBuiltInDisplay",
                        {"_ZN7android21SurfaceComposerClient17getBuiltInDisplayEi"}, outError);
}

bool LibGui::ResolveOps(Resolver& gui, std::string& outError) {
    TransactOps transact{};
    if (!gui.Optional(transact.setLayer, "Transaction::setLayer",
                      {"_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi"})) {
        GlobalOps global{};
        if (!gui.Required(global.open, "SurfaceComposerClient::openGlobalTransaction",
                          {"_ZN7android21SurfaceComposerClient21openGlobalTransactionEv"}, outError) ||
            !gui.Required(global.close, "SurfaceComposerClient::closeGlobalTransaction",
                          {"_ZN7android21SurfaceComposerClient22closeGlobalTransactionEb"}, outError) ||
            !gui.Required(global.setLayer, "SurfaceControl::setLayer",
                          {"_ZN7android14SurfaceControl8setLayerEi",
                           "_ZN7android14SurfaceControl8setLayerEj"}, outError) ||
            !gui.Required(global.show, "SurfaceControl::show",
                          {"_ZN7android14SurfaceControl4showEv"}, outError) ||
            !gui.Required(global.hide, "SurfaceControl::hide",
                          {"_ZN7android14SurfaceControl4hideEv"}, outError)) {
            return false;
        }
        gui.Optional(global.setSize, "SurfaceControl::setSize",
                     {"_ZN7android14SurfaceControl7setSizeEjj"});
        global_ = global;
        hasGlobal_ = true;
        return true;
    }

    if (void* androidLib = dlopen("libandroid.so", RTLD_NOW)) {
        transact.createNdk = reinterpret_cast<GuiPtr (*)()>(dlsym(androidLib, "ASurfaceTransaction_create"));
        if (transact.createNdk) {
            gui.Note("Transaction::create", "ASurfaceTransaction_create");
        }
    }
    if (!transact.createNdk &&
        !gui.Optional(transact.construct, "Transaction::Transaction",
                      {"_ZN7android21SurfaceComposerClient11TransactionC1Ev",
                       "_ZN7android21SurfaceComposerClient11TransactionC2Ev"})) {
        if (!gui.Required(transact.constructCopy, "Transaction::Transaction(const Transaction&)",
                          {"_ZN7android21SurfaceComposerClient11TransactionC1ERKS1_",
                           "_ZN7android21SurfaceComposerClient11TransactionC2ERKS1_"}, outError)) {
            return false;
        }
    }

    if (!gui.Optional(transact.applyOneWay, "Transaction::apply",
                      {"_ZN7android21SurfaceComposerClient11Transaction5applyEbb"})) {
        if (!gui.Required(transact.apply, "Transaction::apply",
                          {"_ZN7android21SurfaceComposerClient11Transaction5applyEb"}, outError)) {
            return false;
        }
    }

    if (!gui.Optional(transact.trustedFlag, "Transaction::setTrustedOverlay",
                      {"_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEEb"})) {
        gui.Optional(transact.trustedMode, "Transaction::setTrustedOverlay",
                     {"_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEENS_3gui14TrustedOverlayE"});
    }

    if (!gui.Required(transact.show, "Transaction::show",
                      {"_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE"}, outError) ||
        !gui.Required(transact.hide, "Transaction::hide",
                      {"_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_2spINS_14SurfaceControlEEE"}, outError)) {
        return false;
    }

    gui.Optional(transact.setSize, "Transaction::setSize",
                 {"_ZN7android21SurfaceComposerClient11Transaction7setSizeERKNS_2spINS_14SurfaceControlEEEjj"});

    transact_ = transact;
    hasTransact_ = true;
    return true;
}

void LibGui::Retain(GuiPtr object) const {
    if (object && retain_) {
        retain_(object, object);
    }
}

void LibGui::Release(GuiPtr object) const {
    if (object && release_) {
        release_(object, object);
    }
}

GuiPtr LibGui::Connect(std::string* outError) const {
    StartBinderThreads();

    GuiPtr client = clientDefault_ ? clientDefault_().value : nullptr;
    if (!client) {
        if (!clientNew_) {
            SetError(outError, "SurfaceComposerClient::getDefault returned null");
            return nullptr;
        }
        client = ConstructInPlace("SurfaceComposerClient", clientNew_, outError);
        if (!client) {
            return nullptr;
        }
        Retain(client);
    }

    if (clientCheck_) {
        const int32_t status = clientCheck_(client);
        if (status != 0) {
            SetError(outError, FormatStatusError("SurfaceFlinger connection failed", status));
            return nullptr;
        }
    }
    return client;
}

GuiPtr LibGui::GetDisplayToken(uint64_t* outPhysicalId, std::string* outError) const {
    auto readMainPhysicalId = [this](uint64_t& idOut) -> bool {
        if (!token_.ids) {
            return false;
        }
        const std::array<GuiPtr, 3> ids = token_.ids().value;
        bool found = false;
        if (ids[0] && ids[0] != ids[1]) {
            std::memcpy(&idOut, ids[0], sizeof(idOut));
            found = true;
        }
        std::free(ids[0]);
        return found;
    };

    GuiPtr token = nullptr;
    uint64_t physId = 0;
    bool hasPhysId = false;

    if (token_.internal) {
        token = token_.internal().value;
        hasPhysId = readMainPhysicalId(physId);
    } else if (token_.physical) {
        hasPhysId = readMainPhysicalId(physId);
        if (!hasPhysId) {
            SetError(outError, "SurfaceFlinger reported no physical displays");
            return nullptr;
        }
        token = token_.physical(physId).value;
    } else if (token_.builtIn) {
        token = token_.builtIn(0).value;
    }

    if (!token) {
        SetError(outError, "SurfaceFlinger returned no display token");
        return nullptr;
    }
    if (outPhysicalId && hasPhysId) {
        *outPhysicalId = physId;
    }
    return token;
}

bool LibGui::GetDisplayState(GuiPtr token, RawDisplayState& outState, std::string* outError) const {
    if (!token || !displayQuery_) {
        SetError(outError, "Display query unavailable");
        return false;
    }

    alignas(8) uint8_t data[512] = {};
    auto readU32 = [&data](size_t offset) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, data + offset, sizeof(v));
        return v;
    };

    if (displayQueryIsModern_) {
        const int32_t status = displayQuery_(&token, data);
        if (status != 0) {
            SetError(outError, FormatStatusError("getDisplayState failed", status));
            return false;
        }
        outState.width = readU32(8);
        outState.height = readU32(12);
        outState.rotation = static_cast<uint8_t>(readU32(4) & 3U);
        outState.refreshRate = 0.0f;
        return true;
    }

    const int32_t status = displayQuery_(&token, data);
    if (status != 0) {
        SetError(outError, FormatStatusError("getDisplayInfo failed", status));
        return false;
    }

    const uint8_t rotation = static_cast<uint8_t>(data[24] & 3U);
    uint32_t w = readU32(0);
    uint32_t h = readU32(4);
    if ((rotation & 1U) != 0U) {
        std::swap(w, h);
    }
    float rate = 0.0f;
    std::memcpy(&rate, data + 16, sizeof(rate));

    outState.width = w;
    outState.height = h;
    outState.rotation = rotation;
    outState.refreshRate = (std::isfinite(rate) && rate >= 1.0f) ? rate : 0.0f;
    return true;
}

GuiPtr LibGui::NewTransaction(std::string* outError) const {
    if (!hasTransact_) {
        return nullptr;
    }
    if (transact_.createNdk) {
        GuiPtr tx = transact_.createNdk();
        if (!tx) {
            SetError(outError, "ASurfaceTransaction_create returned null");
            return nullptr;
        }
        return tx;
    }
    if (transact_.construct) {
        return ConstructInPlace("Transaction", transact_.construct, outError);
    }
    return ConstructInPlace(
        "Transaction",
        [this](GuiPtr object) { transact_.constructCopy(object, object); },
        outError);
}

GuiPtr LibGui::CreateSurface(
    GuiPtr client,
    const char* name,
    uint32_t width,
    uint32_t height,
    CaptureMode capture,
    std::string* outError) const
{
    if (!client || !createFn_) {
        SetError(outError, "SurfaceComposerClient not connected");
        return nullptr;
    }

    const CreateKind kind = createVariant_.kind;
    const ParentKind parentKind = createVariant_.parent;
    const bool hidden = (capture == CaptureMode::Hidden);
    uint32_t flags = (capture == CaptureMode::Secure) ? kFlagSecure : 0U;

    AlignedStorage<512> metadata;
    AlignedStorage<128> name8;

    const bool hinted =
        (kind == CreateKind::MetadataHint || kind == CreateKind::CheckedMetadataHint);
    const bool byMetadata =
        (kind == CreateKind::Metadata ||
         kind == CreateKind::CheckedMetadata ||
         (hinted && parentKind == ParentKind::Control));
    const bool byFlag = (hinted && parentKind == ParentKind::Handle);

    if (metadataNew_) {
        metadataNew_(metadata.Data());
    }
    if (hidden && byMetadata) {
        if (metadataSet_) {
            metadataSet_(metadata.Data(), kMetadataWindowType, kWindowTypeDontScreenshot);
        } else {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "CaptureMode::Hidden unavailable: LayerMetadata::setInt32 missing");
        }
    }
    if (hidden && byFlag) {
        flags |= kFlagSkipScreenshot;
    }
    if (hidden && apiLevel_ > 0 && apiLevel_ < 27) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "CaptureMode::Hidden is not supported before Android 8.1 (API %u); use CaptureMode::Secure",
                            apiLevel_);
    }

    string8New_(name8.Data(), (name && name[0] != '\0') ? name : "ESP4AllOverlay");
    const GuiPtr title = name8.Data();
    GuiPtr noHandle = nullptr;
    const GuiPtr parent = (parentKind == ParentKind::Control) ? nullptr : static_cast<GuiPtr>(&noHandle);
    GuiPtr control = nullptr;
    GuiPtr* const outControl = &control;
    const GuiPtr meta = metadata.Data();
    const int32_t fallbackType = createVariant_.defaultWindowType;
    const int32_t windowType = hidden ? kWindowTypeDontScreenshot : fallbackType;
    const int32_t format = kFormatRgba8888;

    int32_t status = 0;
    switch (kind) {
        case CreateKind::Plain:
            control = reinterpret_cast<Returned<GuiPtr> (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t)>(createFn_)(
                client, title, width, height, format, flags).value;
            break;

        case CreateKind::WindowType:
            control = reinterpret_cast<Returned<GuiPtr> (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, int32_t, int32_t)>(createFn_)(
                client, title, width, height, format, flags, nullptr, windowType, fallbackType).value;
            break;

        case CreateKind::Metadata:
            control = reinterpret_cast<Returned<GuiPtr> (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, GuiPtr)>(createFn_)(
                client, title, width, height, format, flags, nullptr, meta).value;
            break;

        case CreateKind::MetadataHint:
            control = reinterpret_cast<Returned<GuiPtr> (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, GuiPtr, uint32_t*)>(createFn_)(
                client, title, width, height, format, flags, parent, meta, nullptr).value;
            break;

        case CreateKind::CheckedWindowType:
            status = reinterpret_cast<int32_t (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, int32_t, int32_t)>(createFn_)(
                client, title, width, height, format, outControl, flags, nullptr,
                hidden ? kWindowTypeDontScreenshot : -1, -1);
            break;

        case CreateKind::CheckedMetadata:
            status = reinterpret_cast<int32_t (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, GuiPtr)>(createFn_)(
                client, title, width, height, format, outControl, flags, nullptr, meta);
            break;

        case CreateKind::CheckedMetadataHint:
            status = reinterpret_cast<int32_t (*)(
                GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, GuiPtr, uint32_t*)>(createFn_)(
                client, title, width, height, format, outControl, flags, parent, meta, nullptr);
            break;
    }

    string8Drop_(title);

    if (status != 0 || !control) {
        char buf[160] = {};
        std::snprintf(buf, sizeof(buf),
                      "SurfaceFlinger refused to create %ux%u layer (status %d)",
                      width, height, status);
        SetError(outError, buf);
        return nullptr;
    }

    if (validate_) {
        const int32_t valid = validate_(control);
        if (valid != 0) {
            Release(control);
            SetError(outError, FormatStatusError("Created SurfaceControl failed validation", valid));
            return nullptr;
        }
    }

    return control;
}

GuiPtr LibGui::GetSurface(GuiPtr control, std::string* outError) const {
    if (!control || !getSurface_) {
        SetError(outError, "SurfaceControl::getSurface unavailable");
        return nullptr;
    }
    GuiPtr surface = getSurface_(control).value;
    if (!surface) {
        SetError(outError, "SurfaceControl::getSurface returned null");
        return nullptr;
    }
    return surface;
}

void LibGui::Disconnect(GuiPtr control) const {
    if (control && disconnect_) {
        disconnect_(control);
    }
}

bool LibGui::Present(GuiPtr transaction, GuiPtr control, int32_t zOrder, std::string* outError) const {
    if (!control) {
        SetError(outError, "Cannot present null SurfaceControl");
        return false;
    }
    if (hasTransact_) {
        if (!transaction) {
            SetError(outError, "Transaction handle is null");
            return false;
        }
        transact_.setLayer(transaction, &control, zOrder);
        if (transact_.trustedFlag) {
            transact_.trustedFlag(transaction, &control, true);
        } else if (transact_.trustedMode) {
            transact_.trustedMode(transaction, &control, kTrustedOverlayEnabled);
        }
        transact_.show(transaction, &control);
        return ApplyTransaction(transaction, false, outError);
    }
    if (hasGlobal_) {
        global_.open();
        global_.setLayer(control, zOrder);
        global_.show(control);
        global_.close(false);
        return true;
    }
    SetError(outError, "No SurfaceFlinger transaction path available");
    return false;
}

bool LibGui::SetVisible(GuiPtr transaction, GuiPtr control, bool visible, std::string* outError) const {
    if (!control) {
        return false;
    }
    if (hasTransact_) {
        if (!transaction) {
            SetError(outError, "Transaction handle is null");
            return false;
        }
        if (visible) {
            transact_.show(transaction, &control);
        } else {
            transact_.hide(transaction, &control);
        }
        return ApplyTransaction(transaction, false, outError);
    }
    if (hasGlobal_) {
        global_.open();
        if (visible) {
            global_.show(control);
        } else {
            global_.hide(control);
        }
        global_.close(false);
        return true;
    }
    return false;
}

bool LibGui::Resize(
    GuiPtr transaction,
    GuiPtr control,
    uint32_t width,
    uint32_t height,
    bool& outResized,
    std::string* outError) const
{
    outResized = false;
    if (!control) {
        SetError(outError, "Cannot resize null SurfaceControl");
        return false;
    }
    if (updateSize_) {
        updateSize_(control, width, height);
        outResized = true;
        return true;
    }
    if (hasTransact_) {
        if (!transact_.setSize || !transaction) {
            return true; // Cleanly signal in-place resize unsupported
        }
        transact_.setSize(transaction, &control, width, height);
        if (!ApplyTransaction(transaction, true, outError)) {
            return false;
        }
        outResized = true;
        return true;
    }
    if (hasGlobal_) {
        if (!global_.setSize) {
            return true;
        }
        global_.open();
        global_.setSize(control, width, height);
        global_.close(true);
        outResized = true;
        return true;
    }
    return true;
}

bool LibGui::ApplyTransaction(GuiPtr transaction, bool wait, std::string* outError) const {
    const int32_t status = transact_.applyOneWay
        ? transact_.applyOneWay(transaction, wait, false)
        : transact_.apply(transaction, wait);
    if (status != 0) {
        SetError(outError, FormatStatusError("SurfaceFlinger rejected transaction", status));
        return false;
    }
    return true;
}

} // namespace esp4all
