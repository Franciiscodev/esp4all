#include "UI/DebugUI.hpp"
#include <cstdio>
#include <imgui.h>

void DebugUI::DrawStatusBadge(bool ok, const char* okText, const char* failText) {
    if (ok) {
        ImGui::TextColored(ImVec4(0.2f, 0.95f, 0.2f, 1.0f), "[%s]", okText);
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f), "[%s]", failText);
    }
}

void DebugUI::DrawChainLink(const char* label, uint64_t ptr, const char* extra) {
    const bool ok = MemoryUtils::IsValidPtr(ptr, 1);
    DrawStatusBadge(ok);
    ImGui::SameLine();
    if (extra && extra[0] != '\0') {
        ImGui::Text("%s: 0x%llX (%s)", label, static_cast<unsigned long long>(ptr), extra);
    } else {
        ImGui::Text("%s: 0x%llX", label, static_cast<unsigned long long>(ptr));
    }
}

DebugUIActions DebugUI::Render(
    const IMemoryReader& memory,
    const char* memoryModeName,
    const EspEngine& engine,
    EspSettings& settings,
    bool espRunning,
    const char* graphicsBackendName,
    const std::string& touchStatus,
    const std::string& statusMessage,
    const char* hostModeTag)
{
    DebugUIActions actions;

    const float dp = (ImGui::GetStyle().FontScaleDpi > 0.5f) ? ImGui::GetStyle().FontScaleDpi : 2.5f;
    const float btnH = ImGui::GetFrameHeight() * 1.35f;

    ImGui::SetNextWindowSize(ImVec2(340.0f * dp, 430.0f * dp), ImGuiCond_Once);
    if (!ImGui::Begin("ESP4All Core — Chain Debugger")) {
        ImGui::End();
        return actions;
    }

    const auto& cfg = engine.GetConfig();
    const auto& ctx = engine.GetContext();
    const auto& diag = ctx.GetDiagnostics();
    const auto& stats = engine.GetStats();
    const auto& camCfg = cfg.GetCameraConfig();
    const auto& filterCfg = cfg.GetEntityFilterConfig();

    // Header & status line
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f),
                       "Host: %s | Provider: %s | Gfx: %s | Cap: %s | Mem: %s | Axis: %s | Mat: %s | Bones: %s",
                       (hostModeTag && hostModeTag[0] != '\0') ? hostModeTag : "External",
                       engine.IsConfigLoaded() ? cfg.GetProviderType().c_str() : "UNLOADED",
                       (graphicsBackendName && graphicsBackendName[0] != '\0') ? graphicsBackendName : "Unknown",
                       CaptureModeToString(settings.GetCaptureMode()),
                       (memoryModeName && memoryModeName[0] != '\0') ? memoryModeName : "Unknown",
                       (camCfg.upAxis == UpAxis::Y) ? "Y-Up" : "Z-Up",
                       (camCfg.matrixLayout == MatrixLayout::RowMajor) ? "Row" : "Col",
                       diag.bonesEnabled ? "ON" : "OFF");

    if (!statusMessage.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Status: %s", statusMessage.c_str());
    }
    if (!engine.GetLastError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", engine.GetLastError().c_str());
    }

    ImGui::Separator();

    // Quick Action Controls
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float thirdW = (ImGui::GetContentRegionAvail().x - 2.0f * spacingX) / 3.0f;
    if (ImGui::Button("Reload JSON", ImVec2(thirdW, btnH))) {
        actions.reloadJson = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reattach", ImVec2(thirdW, btnH))) {
        actions.reattachProcess = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cycle Mem Mode", ImVec2(-1.0f, btnH))) {
        actions.cycleMemoryMode = true;
    }

    if (!engine.IsInitialized()) {
        if (ImGui::Button("Initialize Engine", ImVec2(-1.0f, btnH * 1.1f))) {
            actions.initializeEngine = true;
        }
    } else {
        const char* espBtnLabel = espRunning ? "Stop ESP" : "Start ESP";
        if (ImGui::Button(espBtnLabel, ImVec2(-1.0f, btnH * 1.1f))) {
            actions.toggleEsp = true;
        }
    }

    ImGui::Separator();

    // Config Validation Panel (auto-opens when errors or warnings are present)
    {
        const auto& report = cfg.GetReport();
        const size_t errCount = report.errorCount();
        const size_t warnCount = report.warnCount();
        const ImGuiTreeNodeFlags cfgFlags = (!report.ok() || warnCount > 0)
            ? ImGuiTreeNodeFlags_DefaultOpen
            : 0;

        char cfgHeader[96] = {};
        if (!engine.IsConfigLoaded()) {
            std::snprintf(cfgHeader, sizeof(cfgHeader), "Config Validation [UNLOADED]###ConfigValidation");
        } else if (errCount > 0) {
            std::snprintf(cfgHeader, sizeof(cfgHeader), "Config Validation [%zu ERR / %zu WARN]###ConfigValidation",
                          errCount, warnCount);
        } else if (warnCount > 0) {
            std::snprintf(cfgHeader, sizeof(cfgHeader), "Config Validation [OK - %zu WARN]###ConfigValidation",
                          warnCount);
        } else {
            std::snprintf(cfgHeader, sizeof(cfgHeader), "Config Validation [OK]###ConfigValidation");
        }

        if (ImGui::CollapsingHeader(cfgHeader, cfgFlags)) {
            if (!engine.IsConfigLoaded() && report.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f),
                                   "[ERR] Config not loaded: %s", engine.GetLastError().c_str());
            } else {
                if (errCount == 0 && warnCount == 0) {
                    DrawStatusBadge(true, "OK");
                    ImGui::SameLine();
                    ImGui::Text("Schema & provider requirements satisfied (0 errors, 0 warnings)");
                } else {
                    if (errCount > 0) {
                        ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f),
                                           "[ERR] %zu error(s), %zu warning(s)", errCount, warnCount);
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                           "[WARN] 0 errors, %zu warning(s)", warnCount);
                    }
                }

                if (engine.IsConfigLoaded()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy Effective JSON")) {
                        ImGui::SetClipboardText(cfg.DumpEffective().c_str());
                    }
                }

                for (const auto& issue : report.issues) {
                    ImVec4 color(0.4f, 0.9f, 1.0f, 1.0f);
                    const char* tag = "INFO";
                    if (issue.level == ConfigIssue::Level::Error) {
                        color = ImVec4(0.95f, 0.28f, 0.28f, 1.0f);
                        tag = "ERR";
                    } else if (issue.level == ConfigIssue::Level::Warn) {
                        color = ImVec4(1.0f, 0.82f, 0.2f, 1.0f);
                        tag = "WARN";
                    }
                    ImGui::TextColored(color, "[%s] %s (%s)", tag, issue.code.c_str(), issue.path.c_str());
                    ImGui::Indent(16.0f);
                    ImGui::TextWrapped("%s", issue.msg.c_str());
                    ImGui::Unindent(16.0f);
                }
            }
        }
    }

    ImGui::Separator();

    // Real-Time Chain Debugger Panel
    if (ImGui::CollapsingHeader("Chain Debugger (Live)", ImGuiTreeNodeFlags_DefaultOpen)) {
        const uint32_t pid = memory.GetPid();
        DrawStatusBadge(pid != 0);
        ImGui::SameLine();
        ImGui::Text("PID: %u (%s)",
                    pid,
                    engine.IsConfigLoaded() ? cfg.GetPackageName().c_str() : "none");

        const uint64_t base = memory.GetModuleBase();
        const std::string segLabel = engine.IsConfigLoaded()
            ? (cfg.GetModuleName() + " [" + cfg.GetSegmentFilter() + "]")
            : "none";
        DrawChainLink("Base", base, segLabel.c_str());

        if (diag.usedEntityListChain) {
            DrawChainLink("EntityContainer", diag.persistentLevel, "EntityList_chain");
        } else {
            const char* worldSrc = diag.usedBridgeWorld
                ? "InternalBridge"
                : (diag.usedGEngineFallback ? "GEngine fallback" : "GWorld");
            DrawChainLink("UWorld", diag.uWorld, worldSrc);
            DrawChainLink("PersistentLevel", diag.persistentLevel);
            DrawChainLink("GameState", diag.gameState);
        }

        const bool actorsOk = MemoryUtils::IsValidPtr(diag.actorsData) && diag.actorsCount > 0;
        DrawStatusBadge(actorsOk);
        ImGui::SameLine();
        if (diag.levelsCount > 1) {
            ImGui::Text("Actors/Players: Data=0x%llX | Num=%d (Levels=%u)%s",
                        static_cast<unsigned long long>(diag.actorsData),
                        diag.actorsCount,
                        diag.levelsCount,
                        diag.usedBridgeActors ? " [Bridge]" : "");
        } else {
            ImGui::Text("Actors/Players: Data=0x%llX | Num=%d%s",
                        static_cast<unsigned long long>(diag.actorsData),
                        diag.actorsCount,
                        diag.usedBridgeActors ? " [Bridge]" : "");
        }

        if (!diag.nameSystemEnabled) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[OFF] NameSystem: disabled");
        } else {
            DrawStatusBadge(MemoryUtils::IsValidPtr(diag.namePoolPtr));
            ImGui::SameLine();
            ImGui::Text("NamePool: 0x%llX | Cache=%zu%s",
                        static_cast<unsigned long long>(diag.namePoolPtr),
                        diag.nameCacheEntries,
                        diag.usedBridgeNamePool ? " [Bridge]" : "");
        }

        if (filterCfg.mode == EntityFilterConfig::Mode::ClassPrivate) {
            DrawStatusBadge(diag.acceptedClassCount > 0, "CACHE", "WAIT");
            ImGui::SameLine();
            ImGui::Text("ClassPrivate: +%zu / -%zu %s",
                        diag.acceptedClassCount,
                        diag.rejectedClassCount,
                        diag.usedHeuristicClassBootstrap ? "(Heuristic)" : "(Named)");
        }

        const char* ctrlSrc = diag.usedNetDriverLocalPlayer
            ? "NetDriver"
            : (diag.usedHeuristicCameraPC ? "Heuristic PC" : nullptr);
        DrawChainLink("LocalController", diag.localController, ctrlSrc);
        DrawChainLink("LocalPawn", diag.localPawn);
        if (diag.hasLocalPos) {
            ImGui::Text("  LocalPos: (%.1f, %.1f, %.1f)",
                        diag.localPos.x, diag.localPos.y, diag.localPos.z);
        }

        const bool camOk = diag.hasViewMatrix || diag.hasCameraPOV;
        DrawStatusBadge(camOk);
        ImGui::SameLine();
        if (diag.usingMatrixCamera && diag.hasViewMatrix) {
            const auto& m = ctx.GetViewMatrix();
            ImGui::Text("Camera: ViewMatrix @ 0x%llX%s",
                        static_cast<unsigned long long>(diag.matrixAddress),
                        diag.usedBridgeCamera ? " [Bridge]" : "");
            ImGui::Text("  Matrix[0..3]:   [%.3f, %.3f, %.3f, %.3f]",
                        m[0], m[1], m[2], m[3]);
            ImGui::Text("  Matrix[12..15]: [%.3f, %.3f, %.3f, %.3f]",
                        m[12], m[13], m[14], m[15]);
        } else if (diag.hasCameraPOV) {
            const auto& pov = ctx.GetCameraPOV();
            ImGui::Text("Camera: CameraCache/Trig (CamMgr=0x%llX)%s%s",
                        static_cast<unsigned long long>(diag.cameraManager),
                        diag.usedBridgeCamera ? " [Bridge]" : "",
                        diag.usedHeuristicCameraPC ? " [Heuristic]" : "");
            ImGui::Text("  CamPos: (%.1f, %.1f, %.1f) | FOV: %.1f",
                        pov.Location.x, pov.Location.y, pov.Location.z, pov.FOV);
            ImGui::Text("  CamRot: (P=%.1f, Y=%.1f, R=%.1f)",
                        pov.Rotation.x, pov.Rotation.y, pov.Rotation.z);
        } else {
            ImGui::Text("Camera: Waiting for valid ViewMatrix or CameraCache");
        }

        ImGui::Separator();
        if (stats.droppedByPoolCap > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "Raw: %u | Filtered (Pool): %u/%u | Dropped: %u | Drawn: %u | Items: %u | %.2f ms",
                               stats.totalRawActors,
                               stats.filteredEntities,
                               stats.poolCapacity,
                               stats.droppedByPoolCap,
                               stats.drawnPlayers,
                               stats.drawnWorldItems,
                               stats.updateMs);
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                               "[WARN] Pool cheio (%u/%u) — %u ator(es) descartado(s). Configure entity_filter ou local_player + max_distance.",
                               stats.filteredEntities,
                               stats.poolCapacity,
                               stats.droppedByPoolCap);
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.8f, 1.0f),
                               "Raw: %u | Filtered (Pool): %u/%u | Drawn: %u | Items: %u | %.2f ms",
                               stats.totalRawActors,
                               stats.filteredEntities,
                               stats.poolCapacity,
                               stats.drawnPlayers,
                               stats.drawnWorldItems,
                               stats.updateMs);
        }
    }

    ImGui::Separator();

    // ESP Toggles & Overlay Settings
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "ESP Options:");
    ImGui::Checkbox("Box", &settings.drawBox);
    ImGui::SameLine();
    ImGui::Checkbox("Line", &settings.drawLine);
    ImGui::SameLine();
    ImGui::Checkbox("Name", &settings.drawName);
    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &settings.drawSkeleton);

    ImGui::Checkbox("Health", &settings.drawHealth);
    ImGui::SameLine();
    ImGui::Checkbox("Distance", &settings.drawDistance);
    ImGui::SameLine();
    ImGui::Checkbox("World Items", &settings.drawWorldItems);

    ImGui::SliderFloat("Target FPS", &settings.targetFps, 30.0f, 120.0f, "%.0f", 0);

    // Overlay Capture Mode (Visible / Hidden / Secure) & Graphics Switcher
    int captureIdx = static_cast<int>(settings.GetCaptureMode());
    ImGui::Text("Capture:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Visible", &captureIdx, static_cast<int>(CaptureMode::Visible))) {
        settings.SetCaptureMode(CaptureMode::Visible);
        actions.captureModeChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Hidden", &captureIdx, static_cast<int>(CaptureMode::Hidden))) {
        settings.SetCaptureMode(CaptureMode::Hidden);
        actions.captureModeChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Secure", &captureIdx, static_cast<int>(CaptureMode::Secure))) {
        settings.SetCaptureMode(CaptureMode::Secure);
        actions.captureModeChanged = true;
    }

    // Touch Interception (/dev/uinput control mode) toggle + live status
    if (ImGui::Checkbox("Touch Intercept (uinput)", &settings.touchControl)) {
        actions.toggleTouchControl = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(
        settings.touchControl ? ImVec4(0.2f, 0.95f, 0.2f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
        "[%s]",
        touchStatus.empty() ? "Read-Only" : touchStatus.c_str());

    char gfxBtnLabel[64] = {};
    std::snprintf(gfxBtnLabel, sizeof(gfxBtnLabel), "Switch Gfx (%s)",
                  (graphicsBackendName && graphicsBackendName[0] != '\0') ? graphicsBackendName : "Auto");
    if (ImGui::Button(gfxBtnLabel, ImVec2(-1.0f, btnH))) {
        actions.cycleGraphicsBackend = true;
    }

    const float halfW = (ImGui::GetContentRegionAvail().x - spacingX) * 0.5f;
    if (ImGui::Button("Save UI Settings", ImVec2(halfW, btnH))) {
        actions.saveSettings = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit", ImVec2(-1.0f, btnH))) {
        actions.requestExit = true;
    }

    ImGui::End();
    return actions;
}
