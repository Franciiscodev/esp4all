#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include "Window/CaptureMode.hpp"

/**
 * Runtime ESP & Overlay settings (plain composition struct, no inheritance).
 * Persists safely with a magic + version header to /data/local/tmp/esp4all_ui.cfg.
 * Version 5 adds touchControl (uinput menu touch interception) while migrating v2/v3/v4 cleanly.
 */
struct EspSettings {
    static constexpr uint32_t MAGIC = 0x45535034U; // "ESP4"
    static constexpr uint32_t VERSION = 5U;

    bool drawBox = true;
    bool drawLine = true;
    bool drawName = true;
    bool drawHealth = true;
    bool drawDistance = true;
    bool drawWorldItems = true;
    bool drawSkeleton = true;
    bool showChainDebugger = true;
    float targetFps = 60.0f;

    // 0 = Auto (prefer Vulkan, fallback to OpenGL ES), 1 = Vulkan, 2 = OpenGL ES
    uint8_t graphicsBackend = 0;
    // 0 = Visible, 1 = Hidden (eSkipScreenshot / Android 8.1+), 2 = Secure (FlagSecure / Android 5+)
    uint8_t captureMode = static_cast<uint8_t>(CaptureMode::Visible);
    // When true, intercepts menu touches via /dev/uinput so taps on DebugUI do not click the game underneath
    bool touchControl = false;

    std::string ue4ConfigPath = "/data/local/tmp/ue4_config.json";
    std::string uiSettingsPath = "/data/local/tmp/esp4all_ui.cfg";

    CaptureMode GetCaptureMode() const noexcept {
        return SanitizeCaptureMode(captureMode);
    }

    void SetCaptureMode(CaptureMode mode) noexcept {
        captureMode = static_cast<uint8_t>(mode);
    }

    struct PersistedDataV2 {
        uint32_t magic;
        uint32_t version;
        uint8_t drawBox;
        uint8_t drawLine;
        uint8_t drawName;
        uint8_t drawHealth;
        uint8_t drawDistance;
        uint8_t drawWorldItems;
        uint8_t drawSkeleton;
        uint8_t showChainDebugger;
        float targetFps;
    };

    struct PersistedDataV3 {
        uint32_t magic;
        uint32_t version;
        uint8_t drawBox;
        uint8_t drawLine;
        uint8_t drawName;
        uint8_t drawHealth;
        uint8_t drawDistance;
        uint8_t drawWorldItems;
        uint8_t drawSkeleton;
        uint8_t showChainDebugger;
        float targetFps;
        uint8_t graphicsBackend;
        uint8_t permeateRecord;
        uint8_t reserved[2];
    };

    struct PersistedDataV4 {
        uint32_t magic;
        uint32_t version;
        uint8_t drawBox;
        uint8_t drawLine;
        uint8_t drawName;
        uint8_t drawHealth;
        uint8_t drawDistance;
        uint8_t drawWorldItems;
        uint8_t drawSkeleton;
        uint8_t showChainDebugger;
        float targetFps;
        uint8_t graphicsBackend;
        uint8_t captureMode;
        uint8_t reserved[2];
    };

    struct PersistedData {
        uint32_t magic;
        uint32_t version;
        uint8_t drawBox;
        uint8_t drawLine;
        uint8_t drawName;
        uint8_t drawHealth;
        uint8_t drawDistance;
        uint8_t drawWorldItems;
        uint8_t drawSkeleton;
        uint8_t showChainDebugger;
        float targetFps;
        uint8_t graphicsBackend;
        uint8_t captureMode;
        uint8_t touchControl;
        uint8_t reserved;
    };

    bool Save() const {
        std::ofstream file(uiSettingsPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        PersistedData data{};
        data.magic = MAGIC;
        data.version = VERSION;
        data.drawBox = drawBox ? 1 : 0;
        data.drawLine = drawLine ? 1 : 0;
        data.drawName = drawName ? 1 : 0;
        data.drawHealth = drawHealth ? 1 : 0;
        data.drawDistance = drawDistance ? 1 : 0;
        data.drawWorldItems = drawWorldItems ? 1 : 0;
        data.drawSkeleton = drawSkeleton ? 1 : 0;
        data.showChainDebugger = showChainDebugger ? 1 : 0;
        data.targetFps = std::clamp(targetFps, 30.0f, 120.0f);
        data.graphicsBackend = (graphicsBackend <= 2) ? graphicsBackend : 0;
        data.captureMode = static_cast<uint8_t>(GetCaptureMode());
        data.touchControl = touchControl ? 1 : 0;

        file.write(reinterpret_cast<const char*>(&data), sizeof(data));
        return file.good();
    }

    bool Load() {
        std::ifstream file(uiSettingsPath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        uint32_t header[2] = {};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!file.good() || header[0] != MAGIC) {
            return false;
        }

        file.seekg(0, std::ios::beg);
        if (header[1] == VERSION) {
            PersistedData data{};
            file.read(reinterpret_cast<char*>(&data), sizeof(data));
            if (!file.good()) {
                return false;
            }
            drawBox = (data.drawBox != 0);
            drawLine = (data.drawLine != 0);
            drawName = (data.drawName != 0);
            drawHealth = (data.drawHealth != 0);
            drawDistance = (data.drawDistance != 0);
            drawWorldItems = (data.drawWorldItems != 0);
            drawSkeleton = (data.drawSkeleton != 0);
            showChainDebugger = (data.showChainDebugger != 0);
            targetFps = std::clamp(data.targetFps, 30.0f, 120.0f);
            graphicsBackend = (data.graphicsBackend <= 2) ? data.graphicsBackend : 0;
            captureMode = static_cast<uint8_t>(SanitizeCaptureMode(data.captureMode));
            touchControl = (data.touchControl != 0);
            return true;
        }

        if (header[1] == 4U) {
            PersistedDataV4 data{};
            file.read(reinterpret_cast<char*>(&data), sizeof(data));
            if (!file.good()) {
                return false;
            }
            drawBox = (data.drawBox != 0);
            drawLine = (data.drawLine != 0);
            drawName = (data.drawName != 0);
            drawHealth = (data.drawHealth != 0);
            drawDistance = (data.drawDistance != 0);
            drawWorldItems = (data.drawWorldItems != 0);
            drawSkeleton = (data.drawSkeleton != 0);
            showChainDebugger = (data.showChainDebugger != 0);
            targetFps = std::clamp(data.targetFps, 30.0f, 120.0f);
            graphicsBackend = (data.graphicsBackend <= 2) ? data.graphicsBackend : 0;
            captureMode = static_cast<uint8_t>(SanitizeCaptureMode(data.captureMode));
            touchControl = false;
            return true;
        }

        if (header[1] == 3U) {
            PersistedDataV3 data{};
            file.read(reinterpret_cast<char*>(&data), sizeof(data));
            if (!file.good()) {
                return false;
            }
            drawBox = (data.drawBox != 0);
            drawLine = (data.drawLine != 0);
            drawName = (data.drawName != 0);
            drawHealth = (data.drawHealth != 0);
            drawDistance = (data.drawDistance != 0);
            drawWorldItems = (data.drawWorldItems != 0);
            drawSkeleton = (data.drawSkeleton != 0);
            showChainDebugger = (data.showChainDebugger != 0);
            targetFps = std::clamp(data.targetFps, 30.0f, 120.0f);
            graphicsBackend = (data.graphicsBackend <= 2) ? data.graphicsBackend : 0;
            captureMode = static_cast<uint8_t>(
                (data.permeateRecord != 0) ? CaptureMode::Hidden : CaptureMode::Visible);
            touchControl = false;
            return true;
        }

        if (header[1] == 2U) {
            PersistedDataV2 data{};
            file.read(reinterpret_cast<char*>(&data), sizeof(data));
            if (!file.good()) {
                return false;
            }
            drawBox = (data.drawBox != 0);
            drawLine = (data.drawLine != 0);
            drawName = (data.drawName != 0);
            drawHealth = (data.drawHealth != 0);
            drawDistance = (data.drawDistance != 0);
            drawWorldItems = (data.drawWorldItems != 0);
            drawSkeleton = (data.drawSkeleton != 0);
            showChainDebugger = (data.showChainDebugger != 0);
            targetFps = std::clamp(data.targetFps, 30.0f, 120.0f);
            graphicsBackend = 0;
            captureMode = static_cast<uint8_t>(CaptureMode::Visible);
            touchControl = false;
            return true;
        }

        return false;
    }
};
