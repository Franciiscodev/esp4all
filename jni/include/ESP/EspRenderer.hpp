#pragma once
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <imgui.h>
#include "ESP/EspEngine.hpp"
#include "ESP/EspSettings.hpp"
#include "UE4/UE4Math.hpp"

/**
 * Pure ImGui 1.92 ESP Renderer consuming pre-projected slots from EspEngine::GetPool().
 * Supports Box, SnapLine, Name, Health, Distance, World Items, and 15-bone Skeleton ESP.
 * Uses Dear ImGui 1.92 dynamic font sizing (ImFont::CalcTextSizeA) with DPI scaling.
 * Performs zero memory reads and zero heap allocations during rendering.
 */
class EspRenderer {
public:
    static ImVec2 MeasureText(const char* text, float fontSize) {
        if (!text || text[0] == '\0') {
            return ImVec2(0.0f, fontSize);
        }
        ImFont* font = ImGui::GetFont();
        return font ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text) : ImVec2(0.0f, fontSize);
    }

    static void Render(
        ImDrawList* draw,
        const EspEngine& engine,
        const EspSettings& settings,
        int screenW,
        int screenH)
    {
        if (!draw || screenW <= 0 || screenH <= 0) {
            return;
        }

        const auto& pool = engine.GetPool();
        const auto& stats = engine.GetStats();
        const auto& boneCfg = engine.GetConfig().GetBoneConfig();
        const bool hasLocalPos = engine.GetContext().HasLocalPlayerPosition();

        ImFont* font = ImGui::GetFont();
        const float dp = std::clamp(ImGui::GetStyle().FontScaleDpi, 1.0f, 5.0f);
        const float labelFontSize = 11.0f * dp;
        const float counterFontSize = 20.0f * dp;
        const float barWidth = 44.0f * dp;
        const float barHeight = std::max(4.0f, 3.0f * dp);
        const float barGap = std::max(5.0f, 3.5f * dp);

        for (const auto& entity : pool) {
            if (!entity.isValid) {
                continue;
            }

            // Mapped vehicles / loot items
            if (entity.isWorldItem) {
                if (!entity.onScreen || !settings.drawWorldItems) {
                    continue;
                }
                char labelBuf[96] = {};
                const char* baseName = entity.hasItemLabel ? entity.itemLabel : entity.className;
                if (hasLocalPos) {
                    std::snprintf(labelBuf, sizeof(labelBuf), "%s %dM",
                                  baseName, static_cast<int>(entity.distanceMeters));
                } else {
                    std::snprintf(labelBuf, sizeof(labelBuf), "%s", baseName);
                }
                const ImVec2 textSize = MeasureText(labelBuf, labelFontSize);
                draw->AddText(font, labelFontSize,
                              {entity.screenPoint.x - textSize.x * 0.5f, entity.screenPoint.y},
                              ImColor(0, 255, 0), labelBuf);
                continue;
            }

            // Optional 15-bone Skeleton ESP (each bone Z-clipped individually; segment length guarded)
            if (settings.drawSkeleton && entity.hasSkeleton) {
                const ImColor boneColor = entity.isBot
                    ? ImColor(255, 220, 80)
                    : ImColor(255, 255, 255);
                const float maxSeg = boneCfg.maxSegmentPixels;

                for (const auto& edge : kSkeletonEdges) {
                    const uint8_t a = edge.first;
                    const uint8_t b = edge.second;
                    if (!entity.boneOnScreen[a] || !entity.boneOnScreen[b]) {
                        continue;
                    }
                    const Vec3& pA = entity.boneScreen[a];
                    const Vec3& pB = entity.boneScreen[b];
                    if (maxSeg > 0.0f && UE4Math::Distance2D(pA.x, pA.y, pB.x, pB.y) > maxSeg) {
                        continue;
                    }
                    draw->AddLine({pA.x, pA.y}, {pB.x, pB.y}, boneColor, 1.8f);
                }
            }

            if (!entity.onScreen || !entity.box.valid) {
                continue;
            }

            const auto& box = entity.box;

            // Box ESP
            if (settings.drawBox) {
                ImColor boxColor = entity.isBot ? ImColor(255, 200, 0) : ImColor(255, 0, 0);
                draw->AddRect({box.left, box.top}, {box.right, box.bottom}, boxColor, 0.0f, 0, 2.0f);
            }

            // Snap Line ESP
            if (settings.drawLine) {
                draw->AddLine({screenW * 0.5f, 0.0f}, {box.centerX, box.top}, ImColor(255, 0, 0), 2.0f);
            }

            // Name ESP
            if (settings.drawName) {
                const char* label = entity.hasPlayerName ? entity.playerName : entity.className;
                if (label && label[0] != '\0') {
                    const ImVec2 textSize = MeasureText(label, labelFontSize);
                    draw->AddText(font, labelFontSize,
                                  {box.centerX - textSize.x * 0.5f, box.top - textSize.y - barHeight - barGap - 2.0f},
                                  ImColor(255, 255, 255), label);
                }
            }

            // Health Bar ESP
            if (settings.drawHealth && entity.hasHealth) {
                const float maxHp = (entity.maxHealth > 0.0f) ? entity.maxHealth : 100.0f;
                const float hpRatio = std::clamp(entity.health / maxHp, 0.0f, 1.0f);
                ImColor hpColor = (hpRatio <= 0.4f)
                    ? ImColor(235, 0, 0)
                    : (hpRatio <= 0.7f) ? ImColor(220, 220, 0) : ImColor(0, 255, 0);

                const float barLeft = box.centerX - barWidth * 0.5f;
                const float barBottom = box.top - barGap;
                draw->AddRectFilled({barLeft, barBottom - barHeight},
                                    {barLeft + barWidth * hpRatio, barBottom},
                                    hpColor, 4.0f);
            }

            // Distance ESP
            if (settings.drawDistance && hasLocalPos) {
                char distBuf[32] = {};
                std::snprintf(distBuf, sizeof(distBuf), "%dM", static_cast<int>(entity.distanceMeters));
                const ImVec2 textSize = MeasureText(distBuf, labelFontSize);
                draw->AddText(font, labelFontSize,
                              {box.centerX - textSize.x * 0.5f, box.bottom + 2.0f},
                              ImColor(220, 220, 0), distBuf);
            }
        }

        // Top-center drawn player counter
        char countBuf[32] = {};
        std::snprintf(countBuf, sizeof(countBuf), "%u", stats.drawnPlayers);
        const ImVec2 countSize = MeasureText(countBuf, counterFontSize);
        draw->AddText(font, counterFontSize,
                      {screenW * 0.5f - countSize.x * 0.5f, 24.0f * dp},
                      ImColor(0, 255, 0), countBuf);
    }
};
