#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "ThirdParty/json.hpp"
#include "UE4/UE4Structures.hpp"

/**
 * Single diagnostic finding produced during ConfigManager validation.
 */
struct ConfigIssue {
    enum class Level {
        Info,
        Warn,
        Error
    };

    Level level = Level::Info;
    std::string path;   // e.g. "world.LevelActors"
    std::string code;   // e.g. "E_REQUIRED_UNSET", "W_UNKNOWN_KEY"
    std::string msg;
};

/**
 * Aggregated validation report returned by ConfigManager::Validate() / GetReport().
 */
struct ConfigReport {
    std::vector<ConfigIssue> issues;

    size_t errorCount() const noexcept {
        size_t n = 0;
        for (const auto& i : issues) {
            if (i.level == ConfigIssue::Level::Error) ++n;
        }
        return n;
    }

    size_t warnCount() const noexcept {
        size_t n = 0;
        for (const auto& i : issues) {
            if (i.level == ConfigIssue::Level::Warn) ++n;
        }
        return n;
    }

    bool ok() const noexcept { return errorCount() == 0; }
    bool empty() const noexcept { return issues.empty(); }
};

/**
 * Configuration manager backed by nlohmann/json (JSONC comment support, non-throwing parse).
 * Supports UE4 (Z-Up) and non-UE4 (Y-Up / Row-Major) engines, Bone ESP profiles,
 * heuristic ClassPrivate bootstrap, arbitrary pointer chains, validation diagnostics,
 * and legacy offsets.* keys.
 */
class ConfigManager {
private:
    nlohmann::json rootJson;
    UE4Offsets offsets;
    FNamePoolConfig namePoolConfig;
    CameraConfig cameraConfig;
    EntityFilterConfig filterConfig;
    ActorAttributesConfig actorConfig;
    BoneEspConfig boneConfig;
    InternalConfig internalConfig;

    PointerChain gWorldChain;
    PointerChain gEngineChain;
    PointerChain entityListChain; // Optional direct chain to entity array container (non-UE4 engines)
    PointerChain namePoolChain;
    PointerChain localPawnChain;
    PointerChain localPawnFromWorldChain;

    std::string packageName;
    std::string moduleName;
    std::string segmentFilter = "r-xp";
    std::string providerType = "WorldActors";
    std::string engineVersion;
    std::string lastError;
    ConfigReport report;

    std::unordered_map<std::string, std::string> vehicleMap;
    std::unordered_map<std::string, std::string> lootMap;

    bool loaded = false;

    static std::string Trim(const std::string& s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    static bool ParseInt64String(const std::string& raw, int64_t& out) {
        std::string s = Trim(raw);
        if (s.empty()) return false;
        errno = 0;
        char* end = nullptr;
        long long val = std::strtoll(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
            return false;
        }
        out = static_cast<int64_t>(val);
        return true;
    }

    static bool ParseUInt64String(const std::string& raw, uint64_t& out) {
        std::string s = Trim(raw);
        if (s.empty() || s[0] == '-') return false;
        errno = 0;
        char* end = nullptr;
        unsigned long long val = std::strtoull(s.c_str(), &end, 0);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
            return false;
        }
        out = static_cast<uint64_t>(val);
        return true;
    }

    static uint64_t ExtractUInt64(const nlohmann::json* node, uint64_t defVal = 0) {
        if (!node || node->is_null()) return defVal;
        if (node->is_number_unsigned()) return node->get<uint64_t>();
        if (node->is_number_integer()) {
            int64_t v = node->get<int64_t>();
            return v >= 0 ? static_cast<uint64_t>(v) : defVal;
        }
        if (node->is_string()) {
            uint64_t parsed = defVal;
            if (ParseUInt64String(node->get<std::string>(), parsed)) {
                return parsed;
            }
        }
        return defVal;
    }

    static int64_t ExtractInt64(const nlohmann::json* node, int64_t defVal = 0) {
        if (!node || node->is_null()) return defVal;
        if (node->is_number_integer()) return node->get<int64_t>();
        if (node->is_number_unsigned()) return static_cast<int64_t>(node->get<uint64_t>());
        if (node->is_string()) {
            int64_t parsed = defVal;
            if (ParseInt64String(node->get<std::string>(), parsed)) {
                return parsed;
            }
        }
        return defVal;
    }

    static float ExtractFloat(const nlohmann::json* node, float defVal = 0.0f) {
        if (!node || node->is_null()) return defVal;
        if (node->is_number()) return node->get<float>();
        if (node->is_string()) {
            std::string s = Trim(node->get<std::string>());
            if (s.empty()) return defVal;
            errno = 0;
            char* end = nullptr;
            float v = std::strtof(s.c_str(), &end);
            if (end != s.c_str() && *end == '\0' && errno != ERANGE) {
                return v;
            }
        }
        return defVal;
    }

    static bool ExtractBool(const nlohmann::json* node, bool defVal = false) {
        if (!node || node->is_null()) return defVal;
        if (node->is_boolean()) return node->get<bool>();
        if (node->is_number_integer()) return node->get<int64_t>() != 0;
        if (node->is_string()) {
            std::string s = Trim(node->get<std::string>());
            if (s == "true" || s == "1" || s == "yes") return true;
            if (s == "false" || s == "0" || s == "no") return false;
        }
        return defVal;
    }

    static std::string ExtractString(const nlohmann::json* node, const std::string& defVal = "") {
        if (!node || node->is_null()) return defVal;
        if (node->is_string()) return node->get<std::string>();
        return defVal;
    }

    static const nlohmann::json* FindByPath(const nlohmann::json& root, const std::string& path) {
        if (!root.is_object() || path.empty()) return nullptr;
        const nlohmann::json* cur = &root;
        size_t start = 0;
        while (start <= path.size()) {
            size_t dot = path.find('.', start);
            std::string key = (dot == std::string::npos)
                ? path.substr(start)
                : path.substr(start, dot - start);
            if (!cur->is_object()) return nullptr;
            auto it = cur->find(key);
            if (it == cur->end()) return nullptr;
            cur = &(*it);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return cur;
    }

    const nlohmann::json* FirstPath(std::initializer_list<const char*> paths) const {
        for (const char* p : paths) {
            const nlohmann::json* node = FindByPath(rootJson, p);
            if (node && !node->is_null()) {
                return node;
            }
        }
        return nullptr;
    }

    static bool ParseChainStep(const nlohmann::json& elem, ChainStep& out) {
        if (elem.is_object()) {
            auto offIt = elem.find("offset");
            if (offIt == elem.end()) return false;
            out.offset = ExtractInt64(&(*offIt), 0);
            auto derefIt = elem.find("deref");
            out.deref = (derefIt != elem.end()) ? ExtractBool(&(*derefIt), true) : true;
            return true;
        }
        if (elem.is_number_integer() || elem.is_number_unsigned()) {
            out.offset = ExtractInt64(&elem, 0);
            out.deref = true;
            return true;
        }
        if (elem.is_string()) {
            std::string s = Trim(elem.get<std::string>());
            if (s.empty()) return false;

            bool deref = true;
            if (s[0] == '+') {
                deref = false;
                s = Trim(s.substr(1));
            } else if (s[0] == '&') {
                deref = false;
                s = Trim(s.substr(1));
            } else if (s.rfind("add:", 0) == 0) {
                deref = false;
                s = Trim(s.substr(4));
            } else if (s[0] == '*') {
                deref = true;
                s = Trim(s.substr(1));
            }

            int64_t off = 0;
            if (!ParseInt64String(s, off)) {
                return false;
            }
            out.offset = off;
            out.deref = deref;
            return true;
        }
        return false;
    }

    static PointerChain ParsePointerChain(const nlohmann::json* node) {
        PointerChain chain;
        if (!node || !node->is_array()) return chain;
        for (const auto& elem : *node) {
            ChainStep step;
            if (ParseChainStep(elem, step)) {
                chain.steps.push_back(step);
            }
        }
        return chain;
    }

    static std::vector<std::string> ParseStringList(const nlohmann::json* node) {
        std::vector<std::string> result;
        if (!node || !node->is_array()) return result;
        for (const auto& item : *node) {
            if (item.is_string()) {
                result.push_back(item.get<std::string>());
            }
        }
        return result;
    }

    static void ParseStringMap(const nlohmann::json* node, std::unordered_map<std::string, std::string>& outMap) {
        outMap.clear();
        if (!node || !node->is_object()) return;
        for (auto it = node->begin(); it != node->end(); ++it) {
            if (it.value().is_string()) {
                outMap[it.key()] = it.value().get<std::string>();
            }
        }
    }

    static BoneProfile ParseBoneProfile(const nlohmann::json* node) {
        BoneProfile profile;
        if (!node || !node->is_array() || node->size() < BONE_SLOT_COUNT) {
            return profile;
        }
        for (size_t i = 0; i < BONE_SLOT_COUNT; ++i) {
            profile.indices[i] = static_cast<int32_t>(ExtractInt64(&((*node)[i]), -1));
        }
        profile.valid = true;
        return profile;
    }

    static std::string ToLowerAscii(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    static size_t EditDistance(const std::string& a, const std::string& b) {
        const size_t n = a.size();
        const size_t m = b.size();
        std::vector<size_t> prev(m + 1), cur(m + 1);
        for (size_t j = 0; j <= m; ++j) prev[j] = j;
        for (size_t i = 1; i <= n; ++i) {
            cur[0] = i;
            for (size_t j = 1; j <= m; ++j) {
                const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            }
            prev.swap(cur);
        }
        return prev[m];
    }

    static bool IsValidUInt64Node(const nlohmann::json& node) {
        if (node.is_number_unsigned()) return true;
        if (node.is_number_integer()) return node.get<int64_t>() >= 0;
        if (node.is_string()) {
            uint64_t tmp = 0;
            return ParseUInt64String(node.get<std::string>(), tmp);
        }
        return false;
    }

    static bool IsValidFloatNode(const nlohmann::json& node) {
        if (node.is_number()) return true;
        if (node.is_string()) {
            std::string s = Trim(node.get<std::string>());
            if (s.empty()) return false;
            errno = 0;
            char* end = nullptr;
            (void)std::strtof(s.c_str(), &end);
            return end != s.c_str() && *end == '\0' && errno != ERANGE;
        }
        return false;
    }

    static bool IsValidBoolNode(const nlohmann::json& node) {
        if (node.is_boolean()) return true;
        if (node.is_number_integer()) {
            int64_t v = node.get<int64_t>();
            return v == 0 || v == 1;
        }
        if (node.is_string()) {
            std::string s = Trim(node.get<std::string>());
            return s == "true" || s == "false" || s == "1" || s == "0" || s == "yes" || s == "no";
        }
        return false;
    }

    static std::string HexString(uint64_t v) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << v;
        return oss.str();
    }

    static nlohmann::json ChainToJson(const PointerChain& chain) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& step : chain.steps) {
            std::ostringstream oss;
            if (!step.deref) oss << "+";
            if (step.offset < 0) {
                oss << "-0x" << std::hex << std::uppercase << static_cast<uint64_t>(-step.offset);
            } else {
                oss << "0x" << std::hex << std::uppercase << static_cast<uint64_t>(step.offset);
            }
            arr.push_back(oss.str());
        }
        return arr;
    }

    void AddIssue(ConfigIssue::Level level, const std::string& path,
                  const std::string& code, const std::string& msg) {
        report.issues.push_back(ConfigIssue{level, path, code, msg});
    }

    bool IsPresentNonNull(const char* path) const {
        const nlohmann::json* n = FindByPath(rootJson, path);
        return n != nullptr && !n->is_null();
    }

    void CheckObjectKeys(const nlohmann::json* obj, const std::string& prefix,
                         std::initializer_list<const char*> knownKeys) {
        if (!obj || obj->is_null()) return;
        if (!obj->is_object()) {
            AddIssue(ConfigIssue::Level::Error, prefix.empty() ? "$" : prefix,
                     "E_BAD_TYPE", "Expected JSON object");
            return;
        }
        for (auto it = obj->begin(); it != obj->end(); ++it) {
            const std::string& key = it.key();
            bool known = false;
            for (const char* k : knownKeys) {
                if (key == k) {
                    known = true;
                    break;
                }
            }
            if (known) continue;

            const std::string lowerKey = ToLowerAscii(key);
            size_t bestDist = std::max<size_t>(2, key.size() / 3) + 1;
            const char* bestMatch = nullptr;
            for (const char* k : knownKeys) {
                const size_t d = EditDistance(lowerKey, ToLowerAscii(k));
                if (d < bestDist) {
                    bestDist = d;
                    bestMatch = k;
                }
            }

            std::string fullPath = prefix.empty() ? key : (prefix + "." + key);
            std::string msg = "Unknown key outside schema";
            if (bestMatch) {
                msg += std::string(" (did you mean '") + bestMatch + "'?)";
            }
            AddIssue(ConfigIssue::Level::Warn, fullPath, "W_UNKNOWN_KEY", msg);
        }
    }

    void CheckUInt64Field(const char* path) {
        const nlohmann::json* n = FindByPath(rootJson, path);
        if (!n || n->is_null()) return;
        if (!IsValidUInt64Node(*n)) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE",
                     "Expected hex string (e.g. \"0x30\") or non-negative integer");
        }
    }

    void CheckFloatField(const char* path) {
        const nlohmann::json* n = FindByPath(rootJson, path);
        if (!n || n->is_null()) return;
        if (!IsValidFloatNode(*n)) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE", "Expected numeric value");
        }
    }

    void CheckBoolField(const char* path) {
        const nlohmann::json* n = FindByPath(rootJson, path);
        if (!n || n->is_null()) return;
        if (!IsValidBoolNode(*n)) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE", "Expected boolean (true/false)");
        }
    }

    void CheckEnumField(const char* path, std::initializer_list<const char*> allowed) {
        const nlohmann::json* n = FindByPath(rootJson, path);
        if (!n || n->is_null()) return;
        if (!n->is_string()) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE", "Expected string enum value");
            return;
        }
        const std::string val = n->get<std::string>();
        for (const char* a : allowed) {
            if (val == a) return;
        }
        std::string msg = "Invalid value '" + val + "'; expected one of: ";
        bool first = true;
        for (const char* a : allowed) {
            if (!first) msg += ", ";
            msg += "'";
            msg += a;
            msg += "'";
            first = false;
        }
        AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE", msg);
    }

    void CheckChainField(const char* path, bool moduleRooted) {
        const nlohmann::json* n = FindByPath(rootJson, path);
        if (!n || n->is_null()) return;
        if (!n->is_array()) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE",
                     "Expected array of pointer chain steps");
            return;
        }
        for (size_t i = 0; i < n->size(); ++i) {
            ChainStep step;
            if (!ParseChainStep((*n)[i], step)) {
                AddIssue(ConfigIssue::Level::Error,
                         std::string(path) + "[" + std::to_string(i) + "]",
                         "E_BAD_TYPE",
                         "Invalid pointer chain step (expected hex/int, \"+0x...\", or {offset, deref})");
            } else if (i == 0 && moduleRooted && step.offset == 0) {
                AddIssue(ConfigIssue::Level::Warn, path, "W_ZERO_FIRST_STEP_IN_CHAIN",
                         "First step of module-rooted chain is 0x0 (reads moduleBase+0 ELF header — likely placeholder)");
            }
        }
    }

    void CheckScalarChainConflict(const char* scalarPath, const char* intermPath,
                                  const char* chainPath, bool inlineFNamePool) {
        const nlohmann::json* sNode = FindByPath(rootJson, scalarPath);
        const nlohmann::json* cNode = FindByPath(rootJson, chainPath);
        if (!sNode || sNode->is_null() || !cNode || !cNode->is_array() || cNode->empty()) {
            return;
        }
        const uint64_t scalar = ExtractUInt64(sNode, 0);
        if (scalar == 0) return;

        const uint64_t interm = intermPath ? ExtractUInt64(FindByPath(rootJson, intermPath), 0) : 0;
        PointerChain expected;
        if (inlineFNamePool) {
            if (interm != 0) {
                expected.steps.emplace_back(static_cast<int64_t>(scalar), true);
                expected.steps.emplace_back(static_cast<int64_t>(interm), false);
            } else {
                expected.steps.emplace_back(static_cast<int64_t>(scalar), false);
            }
        } else {
            expected.steps.emplace_back(static_cast<int64_t>(scalar), true);
            if (interm != 0) {
                expected.steps.emplace_back(static_cast<int64_t>(interm), true);
            }
        }

        PointerChain actual = ParsePointerChain(cNode);
        bool same = (actual.steps.size() == expected.steps.size());
        for (size_t i = 0; same && i < actual.steps.size(); ++i) {
            if (actual.steps[i].offset != expected.steps[i].offset ||
                actual.steps[i].deref != expected.steps[i].deref) {
                same = false;
            }
        }
        if (!same) {
            AddIssue(ConfigIssue::Level::Warn, chainPath, "W_CONFLICT_SCALAR_CHAIN",
                     std::string("Both '") + scalarPath + "' and '" + chainPath +
                     "' are configured with diverging values; chain takes precedence");
        }
    }

    void CheckBoneProfileNode(const nlohmann::json* n, const std::string& path) {
        if (!n || n->is_null()) return;
        if (!n->is_array() || n->size() < BONE_SLOT_COUNT) {
            AddIssue(ConfigIssue::Level::Error, path, "E_BAD_TYPE",
                     "Bone profile must be an array of at least 15 bone indices");
            return;
        }
        for (size_t i = 0; i < BONE_SLOT_COUNT; ++i) {
            const auto& el = (*n)[i];
            int64_t tmp = 0;
            const bool valid = el.is_number_integer() || el.is_number_unsigned() ||
                               (el.is_string() && ParseInt64String(el.get<std::string>(), tmp));
            if (!valid) {
                AddIssue(ConfigIssue::Level::Error,
                         path + "[" + std::to_string(i) + "]",
                         "E_BAD_TYPE", "Expected integer bone index");
                break;
            }
        }
    }

    void ValidateInternal(bool bonesEnabledRequested) {
        report.issues.clear();

        // 1. Unknown key checks (with edit-distance suggestions) & section object types
        CheckObjectKeys(&rootJson, "", {
            "$schema", "meta", "game", "lib", "entity_provider", "globals", "engine_path",
            "world", "game_state", "local_player", "player_controller", "camera",
            "name_system", "entity_filter", "actor", "pawn", "uobject_layout",
            "player_state", "bones", "vehicle_map", "loot_map", "internal", "offsets"
        });

        CheckObjectKeys(FindByPath(rootJson, "game"), "game",
                        {"package_name", "module_name", "segment", "engine_version"});
        CheckObjectKeys(FindByPath(rootJson, "lib"), "lib", {"name", "segment"});
        CheckObjectKeys(FindByPath(rootJson, "entity_provider"), "entity_provider", {"type"});
        CheckObjectKeys(FindByPath(rootJson, "globals"), "globals", {
            "GWorld", "GWorld_chain", "GWorldIntermediate", "GWorld_Offset",
            "GEngine", "GEngine_chain", "GEngineIntermediate", "GEngine_Offset",
            "GNames", "GNames_chain", "GNames_Offset",
            "FNamePool", "FNamePool_chain", "FNamePool_Offset",
            "EntityList_chain", "LocalPawn_chain", "ViewMatrix_chain"
        });
        CheckObjectKeys(FindByPath(rootJson, "engine_path"), "engine_path",
                        {"EngineGameViewport", "ViewportWorld"});
        CheckObjectKeys(FindByPath(rootJson, "world"), "world", {
            "PersistentLevel", "Levels", "NetDriver", "LevelActors", "ArrayCountMode",
            "MaxActors", "EntityArrayMaxCount", "EntityList_chain",
            "GameState", "PlayerArray", "OwningGameInstance"
        });
        CheckObjectKeys(FindByPath(rootJson, "game_state"), "game_state", {"PlayerArray"});
        CheckObjectKeys(FindByPath(rootJson, "local_player"), "local_player", {
            "GameInstance_LocalPlayers", "LocalPlayer_PlayerController",
            "NetDriver_ServerConnection", "ServerConnection_PlayerController",
            "PlayerController_Pawn", "PlayerController_CameraManager",
            "LocalPawn_chain", "LocalPawn_from_world_chain"
        });
        CheckObjectKeys(FindByPath(rootJson, "player_controller"), "player_controller",
                        {"Pawn", "PlayerCameraManager"});
        CheckObjectKeys(FindByPath(rootJson, "camera"), "camera", {
            "mode", "matrix_layout", "up_axis", "heuristic_pc_scan", "view_matrix_chain",
            "camera_cache", "CameraCache", "CameraPOV", "CameraLocation",
            "CameraRotation", "CameraFOV", "FOVRelativeToCache"
        });
        CheckObjectKeys(FindByPath(rootJson, "camera.camera_cache"), "camera.camera_cache", {
            "CameraCache", "POV", "Location", "Rotation", "FOV",
            "FOVRelativeToCache", "heuristic_pc_scan"
        });
        CheckObjectKeys(FindByPath(rootJson, "name_system"), "name_system", {
            "enabled", "type", "ChunkSize", "EntryStride", "NameStringOffset",
            "allocator", "entry"
        });
        CheckObjectKeys(FindByPath(rootJson, "name_system.allocator"), "name_system.allocator",
                        {"CurrentBlock", "Blocks", "Stride"});
        CheckObjectKeys(FindByPath(rootJson, "name_system.entry"), "name_system.entry",
                        {"HeaderSize"});
        CheckObjectKeys(FindByPath(rootJson, "entity_filter"), "entity_filter", {
            "mode", "max_distance", "max_z_delta", "skip_zero_pos",
            "skip_local_player", "skip_same_team", "skip_dead",
            "sentinel", "class_name", "class_private"
        });
        CheckObjectKeys(FindByPath(rootJson, "entity_filter.sentinel"), "entity_filter.sentinel",
                        {"offset", "chain", "type", "value", "equals", "epsilon"});
        CheckObjectKeys(FindByPath(rootJson, "entity_filter.class_name"), "entity_filter.class_name",
                        {"contains", "exact", "exclude"});
        CheckObjectKeys(FindByPath(rootJson, "entity_filter.class_private"), "entity_filter.class_private", {
            "offset", "target_names", "heuristic_bootstrap",
            "use_local_pawn_class", "min_hp", "max_hp"
        });
        CheckObjectKeys(FindByPath(rootJson, "actor"), "actor", {
            "ClassPrivate", "FNameIndex", "RootComponent", "WorldPosition",
            "RelativeLocation", "PositionSource", "ComponentToWorld",
            "Health", "MaxHealth", "TeamId", "PlayerName", "PlayerNameIsUtf16",
            "IsBot", "PlayerState", "PawnPrivate", "Controller",
            "AttributesOnPlayerState", "BoxBaseOffset", "BoxHeight",
            "DistanceScale", "Mesh"
        });
        CheckObjectKeys(FindByPath(rootJson, "pawn"), "pawn", {
            "RootComponent", "PlayerState", "Controller", "WorldPosition",
            "HP", "HPMax", "TeamId", "PlayerName"
        });
        CheckObjectKeys(FindByPath(rootJson, "uobject_layout"), "uobject_layout",
                        {"UOBJ_CLASS", "UOBJ_NAME"});
        CheckObjectKeys(FindByPath(rootJson, "player_state"), "player_state",
                        {"PawnPrivate", "Health", "MaxHealth", "TeamId", "PlayerName"});
        CheckObjectKeys(FindByPath(rootJson, "bones"), "bones", {
            "enabled", "Mesh", "ComponentToWorld", "BoneArray", "BoneArrayBias",
            "BoneStride", "BoneCountOffset", "MaxDistance", "MaxSegmentPixels",
            "default_profile", "profiles"
        });
        CheckObjectKeys(FindByPath(rootJson, "internal"), "internal", {
            "GNames_getter", "GNames_getter_offset", "GNames_getter_deref",
            "GetActorArray_fn", "GUObjectArray", "UObjectArray_ElementSize",
            "UObjectArray_ChunkSize", "UObjectArray_NumOffset", "UObjectArray_Chunked",
            "ViewportClassName", "safe_memory_read"
        });

        // 2. Enum, scalar type, and pointer-chain syntax checks
        CheckEnumField("entity_provider.type", {"WorldActors", "GameStatePlayer"});
        CheckEnumField("world.ArrayCountMode", {"count", "end_ptr", "null_terminated"});
        CheckEnumField("camera.mode", {"auto", "matrix", "view_matrix", "camera_cache", "trig"});
        CheckEnumField("camera.matrix_layout", {"column_major", "row_major"});
        CheckEnumField("camera.up_axis", {"Z", "z", "Y", "y"});
        CheckEnumField("name_system.type", {"FNamePool", "GNames"});
        CheckEnumField("entity_filter.mode", {"none", "sentinel", "class_name", "class_private"});
        CheckEnumField("entity_filter.sentinel.type", {"float", "int", "int32"});
        CheckEnumField("actor.PositionSource", {"RelativeLocation", "ComponentToWorld"});

        const char* uintFields[] = {
            "globals.GWorld", "globals.GWorldIntermediate", "globals.GWorld_Offset",
            "globals.GEngine", "globals.GEngineIntermediate", "globals.GEngine_Offset",
            "globals.GNames", "globals.GNames_Offset",
            "globals.FNamePool", "globals.FNamePool_Offset",
            "engine_path.EngineGameViewport", "engine_path.ViewportWorld",
            "world.PersistentLevel", "world.Levels", "world.NetDriver", "world.LevelActors",
            "world.MaxActors", "world.EntityArrayMaxCount",
            "world.GameState", "world.PlayerArray", "world.OwningGameInstance",
            "local_player.GameInstance_LocalPlayers", "local_player.LocalPlayer_PlayerController",
            "local_player.NetDriver_ServerConnection", "local_player.ServerConnection_PlayerController",
            "local_player.PlayerController_Pawn", "local_player.PlayerController_CameraManager",
            "camera.camera_cache.CameraCache", "camera.camera_cache.POV",
            "camera.camera_cache.Location", "camera.camera_cache.Rotation", "camera.camera_cache.FOV",
            "name_system.ChunkSize", "name_system.EntryStride", "name_system.NameStringOffset",
            "name_system.allocator.CurrentBlock", "name_system.allocator.Blocks", "name_system.allocator.Stride",
            "name_system.entry.HeaderSize",
            "entity_filter.sentinel.offset", "entity_filter.class_private.offset",
            "actor.ClassPrivate", "actor.FNameIndex", "actor.RootComponent",
            "actor.WorldPosition", "actor.RelativeLocation", "actor.ComponentToWorld",
            "actor.Health", "actor.MaxHealth", "actor.TeamId", "actor.PlayerName",
            "actor.IsBot", "actor.PlayerState", "actor.PawnPrivate", "actor.Controller",
            "player_state.PawnPrivate", "player_state.Health", "player_state.MaxHealth",
            "player_state.TeamId", "player_state.PlayerName",
            "bones.Mesh", "bones.ComponentToWorld", "bones.BoneArray",
            "bones.BoneArrayBias", "bones.BoneStride", "bones.BoneCountOffset",
            "internal.GNames_getter", "internal.GNames_getter_offset",
            "internal.GetActorArray_fn", "internal.GUObjectArray",
            "internal.UObjectArray_ElementSize", "internal.UObjectArray_ChunkSize",
            "internal.UObjectArray_NumOffset"
        };
        for (const char* p : uintFields) CheckUInt64Field(p);

        const char* floatFields[] = {
            "actor.BoxBaseOffset", "actor.BoxHeight", "actor.DistanceScale",
            "entity_filter.max_distance", "entity_filter.max_z_delta",
            "entity_filter.sentinel.epsilon",
            "entity_filter.class_private.min_hp", "entity_filter.class_private.max_hp",
            "bones.MaxDistance", "bones.MaxSegmentPixels"
        };
        for (const char* p : floatFields) CheckFloatField(p);

        const char* boolFields[] = {
            "name_system.enabled", "bones.enabled",
            "camera.heuristic_pc_scan", "camera.camera_cache.heuristic_pc_scan",
            "camera.camera_cache.FOVRelativeToCache", "camera.FOVRelativeToCache",
            "actor.PlayerNameIsUtf16", "actor.AttributesOnPlayerState",
            "entity_filter.skip_zero_pos", "entity_filter.skip_local_player",
            "entity_filter.skip_same_team", "entity_filter.skip_dead",
            "entity_filter.class_private.heuristic_bootstrap",
            "entity_filter.class_private.use_local_pawn_class",
            "internal.GNames_getter_deref", "internal.UObjectArray_Chunked",
            "internal.safe_memory_read"
        };
        for (const char* p : boolFields) CheckBoolField(p);

        // Pointer chains: syntax + W_ZERO_FIRST_STEP_IN_CHAIN for module-rooted chains
        CheckChainField("globals.GWorld_chain", true);
        CheckChainField("globals.GEngine_chain", true);
        CheckChainField("globals.GNames_chain", true);
        CheckChainField("globals.FNamePool_chain", true);
        CheckChainField("world.EntityList_chain", true);
        CheckChainField("globals.EntityList_chain", true);
        CheckChainField("local_player.LocalPawn_chain", true);
        CheckChainField("globals.LocalPawn_chain", true);
        CheckChainField("camera.view_matrix_chain", true);
        CheckChainField("globals.ViewMatrix_chain", true);
        CheckChainField("local_player.LocalPawn_from_world_chain", false);
        CheckChainField("entity_filter.sentinel.chain", false);

        // Bone profile shapes
        CheckBoneProfileNode(FindByPath(rootJson, "bones.default_profile"), "bones.default_profile");
        if (const nlohmann::json* profs = FindByPath(rootJson, "bones.profiles")) {
            if (!profs->is_null()) {
                if (!profs->is_object()) {
                    AddIssue(ConfigIssue::Level::Error, "bones.profiles", "E_BAD_TYPE",
                             "Expected object mapping bone count strings to 15-element index arrays");
                } else {
                    for (auto it = profs->begin(); it != profs->end(); ++it) {
                        CheckBoneProfileNode(&it.value(), "bones.profiles." + it.key());
                    }
                }
            }
        }

        // 3. Scalar vs chain conflicts (W_CONFLICT_SCALAR_CHAIN)
        CheckScalarChainConflict("globals.GWorld", "globals.GWorldIntermediate", "globals.GWorld_chain", false);
        CheckScalarChainConflict("globals.GEngine", "globals.GEngineIntermediate", "globals.GEngine_chain", false);
        if (namePoolConfig.type == FNamePoolConfig::Type::FNamePool) {
            CheckScalarChainConflict("globals.FNamePool", "globals.FNamePool_Offset", "globals.FNamePool_chain", true);
        } else {
            CheckScalarChainConflict("globals.GNames", "globals.GNames_Offset", "globals.GNames_chain", false);
        }
        if (filterConfig.sentinelOffset != 0 && !filterConfig.sentinelChain.Empty()) {
            const bool equivalent =
                filterConfig.sentinelChain.steps.size() == 1 &&
                !filterConfig.sentinelChain.steps[0].deref &&
                filterConfig.sentinelChain.steps[0].offset == static_cast<int64_t>(filterConfig.sentinelOffset);
            if (!equivalent) {
                AddIssue(ConfigIssue::Level::Warn, "entity_filter.sentinel.chain", "W_CONFLICT_SCALAR_CHAIN",
                         "Both 'entity_filter.sentinel.offset' and 'entity_filter.sentinel.chain' are set; chain takes precedence");
            }
        }

        // 4. Required-key matrix per provider/mode (E_REQUIRED_UNSET)
        const bool isGameStateProvider = (providerType == "GameStatePlayer");
        const bool entityListMode = !isGameStateProvider && (
            !entityListChain.Empty() ||
            FindByPath(rootJson, "world.EntityList_chain") != nullptr ||
            FindByPath(rootJson, "globals.EntityList_chain") != nullptr);
        const bool hasWorldRoot =
            !gWorldChain.Empty() ||
            (!gEngineChain.Empty() &&
             offsets.Engine_GameViewportClient != 0 &&
             offsets.GameViewportClient_World != 0);
        const bool hasInternalWorld =
            internalConfig.gUObjectArrayOffset != 0 &&
            offsets.GameViewportClient_World != 0;
        const bool hasInternalActors = internalConfig.getActorArrayFnOffset != 0;
        const bool hasInternalNames = internalConfig.gNamesGetterOffset != 0;

        if (isGameStateProvider) {
            if (!hasWorldRoot && !hasInternalWorld) {
                AddIssue(ConfigIssue::Level::Error, "globals.GWorld_chain", "E_REQUIRED_UNSET",
                         "Missing UWorld root (set globals.GWorld_chain / GWorld, GEngine + engine_path, or internal.GUObjectArray + engine_path.ViewportWorld)");
            } else if (!hasWorldRoot && hasInternalWorld) {
                AddIssue(ConfigIssue::Level::Info, "internal.GUObjectArray", "I_INTERNAL_ONLY",
                         "UWorld root relies on internal.GUObjectArray (supported by libesp4all_internal.so)");
            }
            if (offsets.World_GameState == 0) {
                AddIssue(ConfigIssue::Level::Error, "world.GameState", "E_REQUIRED_UNSET",
                         "Required by GameStatePlayer provider");
            }
            if (offsets.GameState_PlayerArray == 0) {
                AddIssue(ConfigIssue::Level::Error, "world.PlayerArray", "E_REQUIRED_UNSET",
                         "Required by GameStatePlayer provider");
            }
            if (offsets.PlayerState_PawnPrivate == 0) {
                AddIssue(ConfigIssue::Level::Error, "player_state.PawnPrivate", "E_REQUIRED_UNSET",
                         "Required by GameStatePlayer provider");
            }
        } else if (entityListMode) {
            if (entityListChain.Empty()) {
                AddIssue(ConfigIssue::Level::Error, "world.EntityList_chain", "E_REQUIRED_UNSET",
                         "Required for WorldActors EntityList_chain mode");
            }
        } else {
            if (!hasWorldRoot && !hasInternalWorld) {
                AddIssue(ConfigIssue::Level::Error, "globals.GWorld_chain", "E_REQUIRED_UNSET",
                         "Missing UWorld root (set globals.GWorld_chain / GWorld, GEngine + engine_path, or internal.GUObjectArray + engine_path.ViewportWorld)");
            } else if (!hasWorldRoot && hasInternalWorld) {
                AddIssue(ConfigIssue::Level::Info, "internal.GUObjectArray", "I_INTERNAL_ONLY",
                         "UWorld root relies on internal.GUObjectArray (supported by libesp4all_internal.so)");
            }
            if (offsets.World_PersistentLevel == 0 && offsets.World_Levels == 0) {
                AddIssue(ConfigIssue::Level::Error, "world.PersistentLevel", "E_REQUIRED_UNSET",
                         "Required by WorldActors provider (set world.PersistentLevel or world.Levels)");
            }
            if (offsets.Level_Actors == 0 && !hasInternalActors) {
                AddIssue(ConfigIssue::Level::Error, "world.LevelActors", "E_REQUIRED_UNSET",
                         "Required by WorldActors provider (or set internal.GetActorArray_fn for internal .so mode)");
            } else if (offsets.Level_Actors == 0 && hasInternalActors) {
                AddIssue(ConfigIssue::Level::Info, "internal.GetActorArray_fn", "I_INTERNAL_ONLY",
                         "Actors array relies on internal.GetActorArray_fn (supported by libesp4all_internal.so)");
            }
        }

        // Actor position requirements
        if (offsets.Actor_RootComponent == 0) {
            AddIssue(ConfigIssue::Level::Error, "actor.RootComponent", "E_REQUIRED_UNSET",
                     "Required to resolve actor 3D world position");
        }
        if (actorConfig.positionSource == PositionSource::ComponentToWorld) {
            if (offsets.SceneComponent_ComponentToWorld == 0) {
                AddIssue(ConfigIssue::Level::Error, "actor.ComponentToWorld", "E_REQUIRED_UNSET",
                         "Required when actor.PositionSource is 'ComponentToWorld'");
            }
        } else {
            if (offsets.SceneComponent_RelativeLocation == 0) {
                AddIssue(ConfigIssue::Level::Error, "actor.WorldPosition", "E_REQUIRED_UNSET",
                         "Required when actor.PositionSource is 'RelativeLocation'");
            }
        }

        // Camera requirements
        const bool hasMatrixCam = !cameraConfig.viewMatrixChain.Empty();
        const bool hasCacheCam =
            offsets.PlayerController_PlayerCameraManager != 0 &&
            cameraConfig.cameraCacheOffset != 0;
        if (cameraConfig.mode == CameraMode::ViewMatrix) {
            if (!hasMatrixCam) {
                AddIssue(ConfigIssue::Level::Error, "camera.view_matrix_chain", "E_REQUIRED_UNSET",
                         "Required when camera.mode is 'matrix'");
            }
        } else if (cameraConfig.mode == CameraMode::CameraCache) {
            if (offsets.PlayerController_PlayerCameraManager == 0) {
                AddIssue(ConfigIssue::Level::Error, "local_player.PlayerController_CameraManager", "E_REQUIRED_UNSET",
                         "Required when camera.mode is 'camera_cache'");
            }
            if (cameraConfig.cameraCacheOffset == 0) {
                AddIssue(ConfigIssue::Level::Error, "camera.camera_cache.CameraCache", "E_REQUIRED_UNSET",
                         "Required when camera.mode is 'camera_cache'");
            }
        } else {
            if (!hasMatrixCam && !hasCacheCam) {
                AddIssue(ConfigIssue::Level::Error, "camera.view_matrix_chain", "E_REQUIRED_UNSET",
                         "Neither camera.view_matrix_chain nor camera_cache (PlayerController_CameraManager + CameraCache) is configured");
            }
        }

        // Enabled optional subsystems
        if (namePoolConfig.enabled && namePoolChain.Empty() && !hasInternalNames) {
            const char* namePath = (namePoolConfig.type == FNamePoolConfig::Type::GNames)
                ? "globals.GNames_chain"
                : "globals.FNamePool_chain";
            AddIssue(ConfigIssue::Level::Error, namePath, "E_REQUIRED_UNSET",
                     "Required when name_system.enabled is true (or set internal.GNames_getter for internal .so mode)");
        } else if (namePoolConfig.enabled && namePoolChain.Empty() && hasInternalNames) {
            AddIssue(ConfigIssue::Level::Info, "internal.GNames_getter", "I_INTERNAL_ONLY",
                     "Name system relies on internal.GNames_getter (supported by libesp4all_internal.so)");
        }
        if (filterConfig.mode == EntityFilterConfig::Mode::Sentinel &&
            filterConfig.sentinelChain.Empty() && filterConfig.sentinelOffset == 0) {
            AddIssue(ConfigIssue::Level::Error, "entity_filter.sentinel.offset", "E_REQUIRED_UNSET",
                     "Sentinel filter requires entity_filter.sentinel.offset or entity_filter.sentinel.chain");
        }
        if (filterConfig.mode == EntityFilterConfig::Mode::ClassName && !namePoolConfig.enabled) {
            AddIssue(ConfigIssue::Level::Error, "name_system.enabled", "E_REQUIRED_UNSET",
                     "entity_filter.mode='class_name' requires name_system.enabled=true");
        }
        if (bonesEnabledRequested) {
            if (boneConfig.meshOffset == 0) {
                AddIssue(ConfigIssue::Level::Error, "bones.Mesh", "E_REQUIRED_UNSET",
                         "Required when bones.enabled is true");
            }
            if (boneConfig.componentToWorldOffset == 0) {
                AddIssue(ConfigIssue::Level::Error, "bones.ComponentToWorld", "E_REQUIRED_UNSET",
                         "Required when bones.enabled is true");
            }
            if (boneConfig.boneArrayOffset == 0) {
                AddIssue(ConfigIssue::Level::Error, "bones.BoneArray", "E_REQUIRED_UNSET",
                         "Required when bones.enabled is true");
            }
            if (!boneConfig.defaultProfile.valid && boneConfig.profilesByCount.empty()) {
                AddIssue(ConfigIssue::Level::Error, "bones.default_profile", "E_REQUIRED_UNSET",
                         "bones.enabled=true requires a valid 15-index default_profile or profiles map");
            }
        }

        // 5. Inactive / unused keys (W_UNUSED_KEY)
        if (!namePoolConfig.enabled) {
            for (const char* p : {"name_system.ChunkSize", "name_system.EntryStride",
                                  "name_system.NameStringOffset", "name_system.allocator",
                                  "name_system.entry"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "Ignored because name_system.enabled is false");
                }
            }
        } else if (namePoolConfig.type == FNamePoolConfig::Type::GNames) {
            for (const char* p : {"name_system.allocator", "name_system.entry"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "FNamePool field ignored because name_system.type is 'GNames'");
                }
            }
        } else {
            for (const char* p : {"name_system.ChunkSize", "name_system.EntryStride",
                                  "name_system.NameStringOffset"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "GNames field ignored because name_system.type is 'FNamePool'");
                }
            }
        }

        if (filterConfig.mode != EntityFilterConfig::Mode::Sentinel &&
            IsPresentNonNull("entity_filter.sentinel")) {
            AddIssue(ConfigIssue::Level::Warn, "entity_filter.sentinel", "W_UNUSED_KEY",
                     "Ignored because entity_filter.mode is not 'sentinel'");
        }
        if (filterConfig.mode != EntityFilterConfig::Mode::ClassName &&
            IsPresentNonNull("entity_filter.class_name")) {
            AddIssue(ConfigIssue::Level::Warn, "entity_filter.class_name", "W_UNUSED_KEY",
                     "Ignored because entity_filter.mode is not 'class_name'");
        }
        if (filterConfig.mode != EntityFilterConfig::Mode::ClassPrivate &&
            IsPresentNonNull("entity_filter.class_private")) {
            AddIssue(ConfigIssue::Level::Warn, "entity_filter.class_private", "W_UNUSED_KEY",
                     "Ignored because entity_filter.mode is not 'class_private'");
        }

        const bool hasLocalPlayerSection =
            IsPresentNonNull("local_player") ||
            !localPawnChain.Empty() ||
            !localPawnFromWorldChain.Empty() ||
            offsets.World_OwningGameInstance != 0 ||
            offsets.World_NetDriver != 0 ||
            offsets.NetDriver_ServerConnection != 0;
        if (!hasLocalPlayerSection) {
            if (IsPresentNonNull("entity_filter.max_distance")) {
                AddIssue(ConfigIssue::Level::Warn, "entity_filter.max_distance", "W_UNUSED_KEY",
                         "Has no effect without local_player configured (local position is unavailable)");
            }
            if (IsPresentNonNull("entity_filter.max_z_delta")) {
                AddIssue(ConfigIssue::Level::Warn, "entity_filter.max_z_delta", "W_UNUSED_KEY",
                         "Has no effect without local_player configured (local position is unavailable)");
            }
        }

        if (isGameStateProvider) {
            for (const char* p : {"world.PersistentLevel", "world.Levels", "world.LevelActors",
                                  "world.EntityList_chain", "world.ArrayCountMode", "world.MaxActors"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "Not used by GameStatePlayer provider");
                }
            }
            if (cameraConfig.heuristicPcScan) {
                AddIssue(ConfigIssue::Level::Warn, "camera.heuristic_pc_scan", "W_UNUSED_KEY",
                         "heuristic_pc_scan only runs in WorldActors provider (PlayerArray holds APlayerState*)");
            }
        } else {
            for (const char* p : {"world.GameState", "world.PlayerArray"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "Not used by WorldActors provider");
                }
            }
            if (entityListMode) {
                for (const char* p : {"world.PersistentLevel", "world.Levels"}) {
                    if (IsPresentNonNull(p)) {
                        AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                                 "Skipped when world.EntityList_chain is active");
                    }
                }
            }
        }

        if (cameraConfig.mode == CameraMode::ViewMatrix && IsPresentNonNull("camera.camera_cache")) {
            AddIssue(ConfigIssue::Level::Warn, "camera.camera_cache", "W_UNUSED_KEY",
                     "Ignored because camera.mode is 'matrix'");
        }
        if (cameraConfig.mode == CameraMode::CameraCache && !cameraConfig.viewMatrixChain.Empty()) {
            AddIssue(ConfigIssue::Level::Warn, "camera.view_matrix_chain", "W_UNUSED_KEY",
                     "Ignored because camera.mode is 'camera_cache'");
        }

        if (!bonesEnabledRequested) {
            for (const char* p : {"bones.Mesh", "bones.ComponentToWorld", "bones.BoneArray",
                                  "bones.BoneArrayBias", "bones.BoneStride", "bones.BoneCountOffset",
                                  "bones.MaxDistance", "bones.MaxSegmentPixels",
                                  "bones.default_profile", "bones.profiles"}) {
                if (IsPresentNonNull(p)) {
                    AddIssue(ConfigIssue::Level::Warn, p, "W_UNUSED_KEY",
                             "Ignored because bones.enabled is false");
                }
            }
        }

        if (actorConfig.positionSource == PositionSource::RelativeLocation &&
            offsets.SceneComponent_ComponentToWorld != 0) {
            AddIssue(ConfigIssue::Level::Warn, "actor.ComponentToWorld", "W_UNUSED_KEY",
                     "Ignored because actor.PositionSource is 'RelativeLocation'");
        }
        if (actorConfig.positionSource == PositionSource::ComponentToWorld &&
            offsets.SceneComponent_ComponentToWorld != 0 &&
            offsets.SceneComponent_RelativeLocation != 0) {
            AddIssue(ConfigIssue::Level::Warn, "actor.WorldPosition", "W_UNUSED_KEY",
                     "Ignored when PositionSource is 'ComponentToWorld' (+0x10 is added automatically)");
        }
    }

    void ResetState() {
        rootJson = nlohmann::json();
        offsets = UE4Offsets();
        namePoolConfig = FNamePoolConfig();
        cameraConfig = CameraConfig();
        filterConfig = EntityFilterConfig();
        actorConfig = ActorAttributesConfig();
        boneConfig = BoneEspConfig();
        internalConfig = InternalConfig();
        gWorldChain.Clear();
        gEngineChain.Clear();
        entityListChain.Clear();
        namePoolChain.Clear();
        localPawnChain.Clear();
        localPawnFromWorldChain.Clear();
        packageName.clear();
        moduleName.clear();
        segmentFilter = "r-xp";
        providerType = "WorldActors";
        engineVersion.clear();
        report.issues.clear();
        vehicleMap.clear();
        lootMap.clear();
        loaded = false;
    }

    bool ParseFromRoot(const nlohmann::json& parsed) {
        if (parsed.is_discarded() || !parsed.is_object()) {
            ResetState();
            lastError = "Invalid JSON syntax or root is not an object";
            AddIssue(ConfigIssue::Level::Error, "$", "E_BAD_TYPE", lastError);
            return false;
        }

        ResetState();
        rootJson = parsed;

        // 1. Game / library / provider metadata
        packageName = ExtractString(FirstPath({"game.package_name", "meta.package_name"}));
        moduleName = ExtractString(FirstPath({"lib.name", "game.module_name"}));
        segmentFilter = ExtractString(FirstPath({"lib.segment", "game.segment"}), "r-xp");
        providerType = ExtractString(FirstPath({"entity_provider.type"}), "WorldActors");
        engineVersion = ExtractString(FirstPath({"meta.engine_version", "game.engine_version"}));

        if (packageName.empty() || moduleName.empty()) {
            if (packageName.empty()) {
                AddIssue(ConfigIssue::Level::Error, "game.package_name", "E_REQUIRED_UNSET",
                         "Missing required target package name");
            }
            if (moduleName.empty()) {
                AddIssue(ConfigIssue::Level::Error, "game.module_name", "E_REQUIRED_UNSET",
                         "Missing required target module name");
            }
            lastError = "Missing required game.package_name or game.module_name/lib.name";
            return false;
        }

        // 2. Global offsets & arbitrary pointer chains
        offsets.GWorld = ExtractUInt64(FirstPath({"globals.GWorld", "offsets.global.GWorld"}), 0);
        offsets.GWorld_Intermediate = ExtractUInt64(FirstPath({
            "globals.GWorldIntermediate", "globals.GWorld_Offset", "offsets.global.GWorld_Offset"
        }), 0);

        gWorldChain = ParsePointerChain(FirstPath({"globals.GWorld_chain", "offsets.global.GWorld_chain"}));
        if (gWorldChain.Empty() && offsets.GWorld != 0) {
            gWorldChain.steps.emplace_back(static_cast<int64_t>(offsets.GWorld), true);
            if (offsets.GWorld_Intermediate != 0) {
                gWorldChain.steps.emplace_back(static_cast<int64_t>(offsets.GWorld_Intermediate), true);
            }
        }

        offsets.GEngine = ExtractUInt64(FirstPath({"globals.GEngine", "offsets.global.GEngine"}), 0);
        offsets.GEngine_Intermediate = ExtractUInt64(FirstPath({
            "globals.GEngineIntermediate", "globals.GEngine_Offset", "offsets.global.GEngine_Offset"
        }), 0);

        gEngineChain = ParsePointerChain(FirstPath({"globals.GEngine_chain", "offsets.global.GEngine_chain"}));
        if (gEngineChain.Empty() && offsets.GEngine != 0) {
            gEngineChain.steps.emplace_back(static_cast<int64_t>(offsets.GEngine), true);
            if (offsets.GEngine_Intermediate != 0) {
                gEngineChain.steps.emplace_back(static_cast<int64_t>(offsets.GEngine_Intermediate), true);
            }
        }

        entityListChain = ParsePointerChain(FirstPath({
            "world.EntityList_chain", "globals.EntityList_chain"
        }));

        // 2b. Optional internal-mode getter & GUObjectArray configuration
        internalConfig.gNamesGetterOffset = ExtractUInt64(FirstPath({"internal.GNames_getter"}), 0);
        internalConfig.gNamesGetterAddOffset = ExtractUInt64(FirstPath({"internal.GNames_getter_offset"}), 0);
        internalConfig.gNamesGetterDeref = ExtractBool(FirstPath({"internal.GNames_getter_deref"}), false);
        internalConfig.getActorArrayFnOffset = ExtractUInt64(FirstPath({"internal.GetActorArray_fn"}), 0);
        internalConfig.gUObjectArrayOffset = ExtractUInt64(FirstPath({"internal.GUObjectArray"}), 0);
        internalConfig.uObjectArrayElementSize = static_cast<uint32_t>(
            ExtractUInt64(FirstPath({"internal.UObjectArray_ElementSize"}), 0x18));
        if (internalConfig.uObjectArrayElementSize == 0) {
            internalConfig.uObjectArrayElementSize = 0x18;
        }
        internalConfig.uObjectArrayChunkSize = static_cast<uint32_t>(
            ExtractUInt64(FirstPath({"internal.UObjectArray_ChunkSize"}), 0x10000));
        internalConfig.uObjectArrayChunked = ExtractBool(FirstPath({"internal.UObjectArray_Chunked"}), true);
        internalConfig.uObjectArrayNumOffset = static_cast<uint32_t>(ExtractUInt64(
            FirstPath({"internal.UObjectArray_NumOffset"}),
            internalConfig.uObjectArrayChunked ? 0x14ULL : 0xCULL));
        internalConfig.viewportClassName = ExtractString(
            FirstPath({"internal.ViewportClassName"}), "GameViewportClient");
        internalConfig.safeMemoryRead = ExtractBool(FirstPath({"internal.safe_memory_read"}), true);

        // 3. Name system
        std::string nameSystemType = ExtractString(FirstPath({"name_system.type"}), "FNamePool");
        namePoolConfig.type = (nameSystemType == "GNames")
            ? FNamePoolConfig::Type::GNames
            : FNamePoolConfig::Type::FNamePool;

        if (namePoolConfig.type == FNamePoolConfig::Type::FNamePool) {
            offsets.FNamePool = ExtractUInt64(FirstPath({
                "globals.FNamePool", "offsets.global.FNamePool", "globals.GNames"
            }), 0);
            offsets.FNamePool_Offset = ExtractUInt64(FirstPath({
                "globals.FNamePool_Offset", "offsets.global.FNamePool_Offset"
            }), 0);

            namePoolChain = ParsePointerChain(FirstPath({
                "globals.FNamePool_chain", "offsets.global.FNamePool_chain"
            }));
            if (namePoolChain.Empty() && offsets.FNamePool != 0) {
                if (offsets.FNamePool_Offset != 0) {
                    namePoolChain.steps.emplace_back(static_cast<int64_t>(offsets.FNamePool), true);
                    namePoolChain.steps.emplace_back(static_cast<int64_t>(offsets.FNamePool_Offset), false);
                } else {
                    namePoolChain.steps.emplace_back(static_cast<int64_t>(offsets.FNamePool), false);
                }
            }

            namePoolConfig.FNameEntryAllocator_CurrentBlock = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.allocator.CurrentBlock"}), 0x8));
            namePoolConfig.FNameEntryAllocator_Blocks = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.allocator.Blocks"}), 0x10));
            namePoolConfig.FNameEntryAllocator_Stride = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.allocator.Stride"}), UE4Constants::FNameEntry_DefaultStride));
            if (namePoolConfig.FNameEntryAllocator_Stride == 0) {
                namePoolConfig.FNameEntryAllocator_Stride = UE4Constants::FNameEntry_DefaultStride;
            }
            namePoolConfig.FNameEntry_HeaderSize = static_cast<uint16_t>(
                ExtractUInt64(FirstPath({"name_system.entry.HeaderSize"}), 0x2));
        } else {
            offsets.FNamePool = ExtractUInt64(FirstPath({
                "globals.GNames", "globals.FNamePool", "offsets.global.GNames", "offsets.global.FNamePool"
            }), 0);
            offsets.FNamePool_Offset = ExtractUInt64(FirstPath({
                "globals.GNames_Offset", "globals.FNamePool_Offset", "offsets.global.GNames_Offset", "offsets.global.FNamePool_Offset"
            }), 0);

            namePoolChain = ParsePointerChain(FirstPath({
                "globals.GNames_chain", "globals.FNamePool_chain",
                "offsets.global.GNames_chain", "offsets.global.FNamePool_chain"
            }));
            if (namePoolChain.Empty() && offsets.FNamePool != 0) {
                namePoolChain.steps.emplace_back(static_cast<int64_t>(offsets.FNamePool), true);
                if (offsets.FNamePool_Offset != 0) {
                    namePoolChain.steps.emplace_back(static_cast<int64_t>(offsets.FNamePool_Offset), true);
                }
            }

            namePoolConfig.ChunkSize = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.ChunkSize"}), 0x4000));
            if (namePoolConfig.ChunkSize == 0) namePoolConfig.ChunkSize = 0x4000;
            namePoolConfig.EntryStride = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.EntryStride"}), 0x8));
            if (namePoolConfig.EntryStride == 0) namePoolConfig.EntryStride = 0x8;
            namePoolConfig.NameStringOffset = static_cast<uint32_t>(
                ExtractUInt64(FirstPath({"name_system.NameStringOffset"}), 0xC));
        }

        const nlohmann::json* nameEnabledNode = FirstPath({"name_system.enabled"});
        const bool defaultNameEnabled = !namePoolChain.Empty() || internalConfig.gNamesGetterOffset != 0;
        if (nameEnabledNode) {
            namePoolConfig.enabled = ExtractBool(nameEnabledNode, defaultNameEnabled);
        } else {
            namePoolConfig.enabled = defaultNameEnabled;
        }

        // 4. Engine path (GEngine -> GameViewportClient -> World)
        offsets.Engine_GameViewportClient = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "engine_path.EngineGameViewport", "offsets.engine.GameViewportClient"
        }), 0));
        offsets.GameViewportClient_World = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "engine_path.ViewportWorld", "offsets.game_viewport_client.World"
        }), 0));

        // 5. World / Level / GameState / EntityArray layout
        offsets.World_PersistentLevel = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.PersistentLevel", "offsets.world.PersistentLevel"
        }), 0));
        offsets.World_Levels = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.Levels", "offsets.world.Levels"
        }), 0));
        offsets.World_NetDriver = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.NetDriver", "offsets.world.NetDriver"
        }), 0));
        offsets.Level_Actors = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.LevelActors", "offsets.level.Actors"
        }), 0));

        std::string arrModeStr = ExtractString(FirstPath({"world.ArrayCountMode"}), "count");
        if (arrModeStr == "end_ptr") {
            offsets.entityArrayMode = EntityArrayMode::BeginEndPtr;
        } else if (arrModeStr == "null_terminated") {
            offsets.entityArrayMode = EntityArrayMode::NullTerminated;
        } else {
            offsets.entityArrayMode = EntityArrayMode::TArrayCount;
        }
        offsets.entityArrayMaxCount = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.MaxActors", "world.EntityArrayMaxCount"
        }), 3000));
        if (offsets.entityArrayMaxCount == 0 || offsets.entityArrayMaxCount > 10000) {
            offsets.entityArrayMaxCount = 3000;
        }

        offsets.World_GameState = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.GameState", "offsets.world.GameState"
        }), 0));
        offsets.GameState_PlayerArray = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.PlayerArray", "game_state.PlayerArray", "offsets.game_state.PlayerArray"
        }), 0));
        offsets.World_OwningGameInstance = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "world.OwningGameInstance", "offsets.world.OwningGameInstance"
        }), 0));

        // 6. LocalPlayer / NetDriver / Controller / CameraManager offsets
        offsets.GameInstance_LocalPlayers = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.GameInstance_LocalPlayers", "offsets.game_instance.LocalPlayers"
        }), 0));
        offsets.LocalPlayer_PlayerController = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.LocalPlayer_PlayerController", "offsets.local_player.PlayerController"
        }), 0));
        offsets.NetDriver_ServerConnection = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.NetDriver_ServerConnection", "offsets.net_driver.ServerConnection"
        }), 0));
        offsets.ServerConnection_PlayerController = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.ServerConnection_PlayerController", "offsets.net_connection.PlayerController"
        }), 0));
        offsets.PlayerController_Pawn = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.PlayerController_Pawn", "player_controller.Pawn", "offsets.player_controller.Pawn"
        }), 0));
        offsets.PlayerController_PlayerCameraManager = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "local_player.PlayerController_CameraManager",
            "player_controller.PlayerCameraManager",
            "offsets.player_controller.PlayerCameraManager"
        }), 0));

        localPawnChain = ParsePointerChain(FirstPath({
            "local_player.LocalPawn_chain", "globals.LocalPawn_chain"
        }));
        localPawnFromWorldChain = ParsePointerChain(FirstPath({
            "local_player.LocalPawn_from_world_chain"
        }));

        // 7. Camera configuration
        std::string camModeStr = ExtractString(FirstPath({"camera.mode"}), "auto");
        if (camModeStr == "matrix" || camModeStr == "view_matrix") {
            cameraConfig.mode = CameraMode::ViewMatrix;
        } else if (camModeStr == "camera_cache" || camModeStr == "trig") {
            cameraConfig.mode = CameraMode::CameraCache;
        } else {
            cameraConfig.mode = CameraMode::Auto;
        }

        std::string layoutStr = ExtractString(FirstPath({"camera.matrix_layout"}), "column_major");
        cameraConfig.matrixLayout = (layoutStr == "row_major")
            ? MatrixLayout::RowMajor
            : MatrixLayout::ColumnMajor;

        std::string upAxisStr = ExtractString(FirstPath({"camera.up_axis"}), "Z");
        cameraConfig.upAxis = (upAxisStr == "Y" || upAxisStr == "y") ? UpAxis::Y : UpAxis::Z;

        cameraConfig.viewMatrixChain = ParsePointerChain(FirstPath({
            "camera.view_matrix_chain", "globals.ViewMatrix_chain"
        }));

        cameraConfig.cameraCacheOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "camera.camera_cache.CameraCache",
            "camera.CameraCache",
            "offsets.camera_manager.CameraCachePrivate"
        }), 0));
        cameraConfig.povOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "camera.camera_cache.POV",
            "camera.CameraPOV",
            "offsets.camera_cache_entry.POV"
        }), 0x10));
        cameraConfig.locationOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "camera.camera_cache.Location", "camera.CameraLocation"
        }), UE4Constants::POV_Location));
        cameraConfig.rotationOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "camera.camera_cache.Rotation", "camera.CameraRotation"
        }), UE4Constants::POV_Rotation));
        cameraConfig.fovOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "camera.camera_cache.FOV", "camera.CameraFOV"
        }), UE4Constants::POV_FOV));
        cameraConfig.fovRelativeToCache = ExtractBool(FirstPath({
            "camera.camera_cache.FOVRelativeToCache", "camera.FOVRelativeToCache"
        }), false);
        cameraConfig.heuristicPcScan = ExtractBool(FirstPath({
            "camera.heuristic_pc_scan", "camera.camera_cache.heuristic_pc_scan"
        }), false);

        offsets.CameraManager_CameraCachePrivate = cameraConfig.cameraCacheOffset;
        offsets.CameraCacheEntry_POV = cameraConfig.povOffset;

        // 8. Actor / UObject / SceneComponent / PlayerState offsets
        offsets.Actor_ClassPrivate = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.ClassPrivate", "uobject_layout.UOBJ_CLASS", "offsets.actor.ClassPrivate"
        }), UE4Constants::UObject_ClassPrivate));
        offsets.Actor_FNameIndex = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.FNameIndex", "uobject_layout.UOBJ_NAME", "offsets.actor.FNameIndex"
        }), UE4Constants::UObject_NamePrivate));
        offsets.Actor_RootComponent = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.RootComponent", "pawn.RootComponent", "offsets.actor.RootComponent"
        }), 0));
        offsets.Actor_PlayerState = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.PlayerState", "pawn.PlayerState", "offsets.actor.PlayerState"
        }), 0));
        offsets.Actor_Controller = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.Controller", "pawn.Controller", "offsets.actor.Controller"
        }), 0));
        offsets.PlayerState_PawnPrivate = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "player_state.PawnPrivate", "actor.PawnPrivate", "offsets.player_state.PawnPrivate"
        }), 0));

        offsets.SceneComponent_RelativeLocation = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.WorldPosition", "actor.RelativeLocation",
            "pawn.WorldPosition", "offsets.scene_component.RelativeLocation"
        }), 0));
        offsets.SceneComponent_ComponentToWorld = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.ComponentToWorld", "offsets.scene_component.ComponentToWorld"
        }), 0));

        // Actor attributes config
        std::string posSrcStr = ExtractString(FirstPath({"actor.PositionSource"}), "RelativeLocation");
        actorConfig.positionSource = (posSrcStr == "ComponentToWorld")
            ? PositionSource::ComponentToWorld
            : PositionSource::RelativeLocation;
        actorConfig.attributesOnPlayerState = ExtractBool(FirstPath({"actor.AttributesOnPlayerState"}), false);
        actorConfig.healthOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.Health", "pawn.HP", "offsets.actor.Health"
        }), 0));
        actorConfig.maxHealthOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.MaxHealth", "pawn.HPMax", "offsets.actor.MaxHealth"
        }), 0));
        actorConfig.teamIdOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.TeamId", "pawn.TeamId", "offsets.actor.TeamId"
        }), 0));
        actorConfig.playerNameOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.PlayerName", "pawn.PlayerName", "offsets.actor.PlayerName"
        }), 0));
        actorConfig.playerNameIsUtf16 = ExtractBool(FirstPath({"actor.PlayerNameIsUtf16"}), true);
        actorConfig.isBotOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "actor.IsBot", "offsets.actor.IsBot"
        }), 0));
        actorConfig.psHealthOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "player_state.Health", "offsets.player_state.Health"
        }), 0));
        actorConfig.psMaxHealthOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "player_state.MaxHealth", "offsets.player_state.MaxHealth"
        }), 0));
        actorConfig.psTeamIdOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "player_state.TeamId", "offsets.player_state.TeamId"
        }), 0));
        actorConfig.psPlayerNameOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({
            "player_state.PlayerName", "offsets.player_state.PlayerName"
        }), 0));
        actorConfig.boxBaseOffset = ExtractFloat(FirstPath({"actor.BoxBaseOffset"}), 0.0f);
        actorConfig.boxHeight = ExtractFloat(FirstPath({"actor.BoxHeight"}), 165.0f);
        actorConfig.distanceScale = ExtractFloat(FirstPath({"actor.DistanceScale"}), 0.01f);

        // 9. Entity filter configuration
        std::string filterModeStr = ExtractString(FirstPath({"entity_filter.mode"}), "none");
        if (filterModeStr == "sentinel") {
            filterConfig.mode = EntityFilterConfig::Mode::Sentinel;
        } else if (filterModeStr == "class_name") {
            filterConfig.mode = EntityFilterConfig::Mode::ClassName;
        } else if (filterModeStr == "class_private") {
            filterConfig.mode = EntityFilterConfig::Mode::ClassPrivate;
        } else {
            filterConfig.mode = EntityFilterConfig::Mode::None;
        }

        filterConfig.maxDistanceMeters = ExtractFloat(FirstPath({"entity_filter.max_distance"}), 500.0f);
        filterConfig.maxZDelta = ExtractFloat(FirstPath({"entity_filter.max_z_delta"}), 10000.0f);
        filterConfig.skipZeroPos = ExtractBool(FirstPath({"entity_filter.skip_zero_pos"}), true);
        filterConfig.skipLocalPlayer = ExtractBool(FirstPath({"entity_filter.skip_local_player"}), true);
        filterConfig.skipSameTeam = ExtractBool(FirstPath({"entity_filter.skip_same_team"}), false);
        filterConfig.skipDead = ExtractBool(FirstPath({"entity_filter.skip_dead"}), false);

        filterConfig.sentinelOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({"entity_filter.sentinel.offset"}), 0));
        filterConfig.sentinelChain = ParsePointerChain(FirstPath({"entity_filter.sentinel.chain"}));
        std::string sentType = ExtractString(FirstPath({"entity_filter.sentinel.type"}), "float");
        filterConfig.sentinelType = (sentType == "int" || sentType == "int32")
            ? EntityFilterConfig::SentinelType::Int32
            : EntityFilterConfig::SentinelType::Float;
        const nlohmann::json* sentValNode = FirstPath({"entity_filter.sentinel.value", "entity_filter.sentinel.equals"});
        filterConfig.sentinelFloatValue = ExtractFloat(sentValNode, 0.0f);
        filterConfig.sentinelIntValue = static_cast<int32_t>(ExtractInt64(sentValNode, 0));
        filterConfig.sentinelEpsilon = ExtractFloat(FirstPath({"entity_filter.sentinel.epsilon"}), 0.1f);

        filterConfig.classNameContains = ParseStringList(FirstPath({"entity_filter.class_name.contains"}));
        filterConfig.classNameExact = ParseStringList(FirstPath({"entity_filter.class_name.exact"}));
        filterConfig.classNameExclude = ParseStringList(FirstPath({"entity_filter.class_name.exclude"}));

        filterConfig.classPrivateOffset = static_cast<uint32_t>(
            ExtractUInt64(FirstPath({"entity_filter.class_private.offset"}), offsets.Actor_ClassPrivate));
        filterConfig.classPrivateTargetNames = ParseStringList(FirstPath({"entity_filter.class_private.target_names"}));
        filterConfig.classPrivateHeuristicBootstrap = ExtractBool(
            FirstPath({"entity_filter.class_private.heuristic_bootstrap"}), true);
        filterConfig.classPrivateUseLocalPawn = ExtractBool(
            FirstPath({"entity_filter.class_private.use_local_pawn_class"}), true);
        filterConfig.heuristicMinHp = ExtractFloat(FirstPath({"entity_filter.class_private.min_hp"}), 1.0f);
        filterConfig.heuristicMaxHp = ExtractFloat(FirstPath({"entity_filter.class_private.max_hp"}), 1000.0f);

        // 10. Optional Bone ESP config (FTransform-based)
        const bool bonesEnabledRequested = ExtractBool(FirstPath({"bones.enabled"}), false);
        boneConfig.enabled = bonesEnabledRequested;
        boneConfig.meshOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({"bones.Mesh", "actor.Mesh"}), 0));
        boneConfig.componentToWorldOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({"bones.ComponentToWorld"}), 0));
        boneConfig.boneArrayOffset = static_cast<uint32_t>(ExtractUInt64(FirstPath({"bones.BoneArray"}), 0));
        boneConfig.boneArrayBias = static_cast<uint32_t>(ExtractUInt64(FirstPath({"bones.BoneArrayBias"}), 0));
        boneConfig.boneStride = static_cast<uint32_t>(ExtractUInt64(FirstPath({"bones.BoneStride"}), UE4Constants::FTransform_Stride));
        if (boneConfig.boneStride < sizeof(FTransform)) {
            boneConfig.boneStride = UE4Constants::FTransform_Stride;
        }
        boneConfig.boneCountOffset = static_cast<uint32_t>(ExtractUInt64(
            FirstPath({"bones.BoneCountOffset"}),
            boneConfig.boneArrayOffset != 0 ? (boneConfig.boneArrayOffset + UE4Constants::TArray_Count) : 0));
        boneConfig.maxDistanceMeters = ExtractFloat(FirstPath({"bones.MaxDistance"}), 250.0f);
        boneConfig.maxSegmentPixels = ExtractFloat(FirstPath({"bones.MaxSegmentPixels"}), 350.0f);

        boneConfig.defaultProfile = ParseBoneProfile(FirstPath({"bones.default_profile"}));
        const nlohmann::json* profilesNode = FirstPath({"bones.profiles"});
        if (profilesNode && profilesNode->is_object()) {
            for (auto it = profilesNode->begin(); it != profilesNode->end(); ++it) {
                int64_t boneCountKey = 0;
                if (ParseInt64String(it.key(), boneCountKey) && boneCountKey > 0) {
                    BoneProfile prof = ParseBoneProfile(&it.value());
                    if (prof.valid) {
                        boneConfig.profilesByCount[static_cast<int32_t>(boneCountKey)] = prof;
                    }
                }
            }
        }
        // If no valid profile was supplied, keep Bone ESP cleanly disabled
        if (boneConfig.enabled &&
            (boneConfig.meshOffset == 0 || boneConfig.componentToWorldOffset == 0 ||
             boneConfig.boneArrayOffset == 0 ||
             (!boneConfig.defaultProfile.valid && boneConfig.profilesByCount.empty()))) {
            boneConfig.enabled = false;
        }

        // 11. Optional vehicle & loot maps
        ParseStringMap(FirstPath({"vehicle_map"}), vehicleMap);
        ParseStringMap(FirstPath({"loot_map"}), lootMap);

        // 12. Schema & semantic validation (non-throwing, outside frame loop)
        ValidateInternal(bonesEnabledRequested);

        lastError.clear();
        loaded = true;
        return true;
    }

public:
    ConfigManager() = default;

    bool Load(const std::string& configPath) {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            ResetState();
            lastError = "Failed to open config file: " + configPath;
            AddIssue(ConfigIssue::Level::Error, "$", "E_IO", lastError);
            return false;
        }
        nlohmann::json parsed = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        return ParseFromRoot(parsed);
    }

    bool LoadFromString(const std::string& jsonText) {
        nlohmann::json parsed = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        return ParseFromRoot(parsed);
    }

    bool IsLoaded() const noexcept { return loaded; }
    const std::string& GetLastError() const noexcept { return lastError; }
    const ConfigReport& GetReport() const noexcept { return report; }
    ConfigReport Validate() const { return report; }

    std::string DumpEffective() const {
        nlohmann::json out;
        out["game"] = {
            {"package_name", packageName},
            {"module_name", moduleName},
            {"segment", segmentFilter}
        };
        out["entity_provider"] = {{"type", providerType}};
        out["globals"] = {
            {"GWorld_chain", ChainToJson(gWorldChain)},
            {"GEngine_chain", ChainToJson(gEngineChain)},
            {"NamePool_chain", ChainToJson(namePoolChain)}
        };
        out["world"] = {
            {"PersistentLevel", HexString(offsets.World_PersistentLevel)},
            {"Levels", HexString(offsets.World_Levels)},
            {"NetDriver", HexString(offsets.World_NetDriver)},
            {"LevelActors", HexString(offsets.Level_Actors)},
            {"EntityList_chain", ChainToJson(entityListChain)},
            {"ArrayCountMode", offsets.entityArrayMode == EntityArrayMode::BeginEndPtr
                ? "end_ptr"
                : (offsets.entityArrayMode == EntityArrayMode::NullTerminated ? "null_terminated" : "count")},
            {"MaxActors", offsets.entityArrayMaxCount},
            {"GameState", HexString(offsets.World_GameState)},
            {"PlayerArray", HexString(offsets.GameState_PlayerArray)},
            {"OwningGameInstance", HexString(offsets.World_OwningGameInstance)}
        };
        out["local_player"] = {
            {"GameInstance_LocalPlayers", HexString(offsets.GameInstance_LocalPlayers)},
            {"LocalPlayer_PlayerController", HexString(offsets.LocalPlayer_PlayerController)},
            {"NetDriver_ServerConnection", HexString(offsets.NetDriver_ServerConnection)},
            {"ServerConnection_PlayerController", HexString(offsets.ServerConnection_PlayerController)},
            {"PlayerController_Pawn", HexString(offsets.PlayerController_Pawn)},
            {"PlayerController_CameraManager", HexString(offsets.PlayerController_PlayerCameraManager)}
        };
        out["camera"] = {
            {"mode", cameraConfig.mode == CameraMode::ViewMatrix
                ? "matrix"
                : (cameraConfig.mode == CameraMode::CameraCache ? "camera_cache" : "auto")},
            {"matrix_layout", cameraConfig.matrixLayout == MatrixLayout::RowMajor ? "row_major" : "column_major"},
            {"up_axis", cameraConfig.upAxis == UpAxis::Y ? "Y" : "Z"},
            {"heuristic_pc_scan", cameraConfig.heuristicPcScan},
            {"view_matrix_chain", ChainToJson(cameraConfig.viewMatrixChain)},
            {"CameraCache", HexString(cameraConfig.cameraCacheOffset)},
            {"POV", HexString(cameraConfig.povOffset)}
        };
        out["name_system"] = {
            {"enabled", namePoolConfig.enabled},
            {"type", namePoolConfig.type == FNamePoolConfig::Type::GNames ? "GNames" : "FNamePool"}
        };
        out["entity_filter"] = {
            {"mode", filterConfig.mode == EntityFilterConfig::Mode::Sentinel
                ? "sentinel"
                : (filterConfig.mode == EntityFilterConfig::Mode::ClassName
                    ? "class_name"
                    : (filterConfig.mode == EntityFilterConfig::Mode::ClassPrivate ? "class_private" : "none"))},
            {"max_distance", filterConfig.maxDistanceMeters},
            {"max_z_delta", filterConfig.maxZDelta},
            {"skip_zero_pos", filterConfig.skipZeroPos},
            {"skip_local_player", filterConfig.skipLocalPlayer},
            {"skip_same_team", filterConfig.skipSameTeam},
            {"skip_dead", filterConfig.skipDead}
        };
        out["actor"] = {
            {"RootComponent", HexString(offsets.Actor_RootComponent)},
            {"PositionSource", actorConfig.positionSource == PositionSource::ComponentToWorld
                ? "ComponentToWorld" : "RelativeLocation"},
            {"WorldPosition", HexString(offsets.SceneComponent_RelativeLocation)},
            {"ComponentToWorld", HexString(offsets.SceneComponent_ComponentToWorld)},
            {"BoxBaseOffset", actorConfig.boxBaseOffset},
            {"BoxHeight", actorConfig.boxHeight},
            {"DistanceScale", actorConfig.distanceScale}
        };
        out["player_state"] = {
            {"PawnPrivate", HexString(offsets.PlayerState_PawnPrivate)},
            {"Health", HexString(actorConfig.psHealthOffset)},
            {"MaxHealth", HexString(actorConfig.psMaxHealthOffset)},
            {"TeamId", HexString(actorConfig.psTeamIdOffset)},
            {"PlayerName", HexString(actorConfig.psPlayerNameOffset)}
        };
        out["bones"] = {
            {"enabled", boneConfig.enabled},
            {"Mesh", HexString(boneConfig.meshOffset)},
            {"ComponentToWorld", HexString(boneConfig.componentToWorldOffset)},
            {"BoneArray", HexString(boneConfig.boneArrayOffset)}
        };
        out["internal"] = {
            {"GNames_getter", HexString(internalConfig.gNamesGetterOffset)},
            {"GetActorArray_fn", HexString(internalConfig.getActorArrayFnOffset)},
            {"GUObjectArray", HexString(internalConfig.gUObjectArrayOffset)},
            {"safe_memory_read", internalConfig.safeMemoryRead}
        };
        return out.dump(2);
    }

    const UE4Offsets& GetOffsets() const noexcept { return offsets; }
    const FNamePoolConfig& GetNamePoolConfig() const noexcept { return namePoolConfig; }
    const CameraConfig& GetCameraConfig() const noexcept { return cameraConfig; }
    const EntityFilterConfig& GetEntityFilterConfig() const noexcept { return filterConfig; }
    const ActorAttributesConfig& GetActorConfig() const noexcept { return actorConfig; }
    const BoneEspConfig& GetBoneConfig() const noexcept { return boneConfig; }
    const InternalConfig& GetInternalConfig() const noexcept { return internalConfig; }

    const PointerChain& GetGWorldChain() const noexcept { return gWorldChain; }
    const PointerChain& GetGEngineChain() const noexcept { return gEngineChain; }
    const PointerChain& GetEntityListChain() const noexcept { return entityListChain; }
    const PointerChain& GetNamePoolChain() const noexcept { return namePoolChain; }
    const PointerChain& GetLocalPawnChain() const noexcept { return localPawnChain; }
    const PointerChain& GetLocalPawnFromWorldChain() const noexcept { return localPawnFromWorldChain; }

    const std::string& GetPackageName() const noexcept { return packageName; }
    const std::string& GetModuleName() const noexcept { return moduleName; }
    const std::string& GetSegmentFilter() const noexcept { return segmentFilter; }
    const std::string& GetProviderType() const noexcept { return providerType; }
    const std::string& GetEngineVersion() const noexcept { return engineVersion; }

    const std::unordered_map<std::string, std::string>& GetVehicleMap() const noexcept { return vehicleMap; }
    const std::unordered_map<std::string, std::string>& GetLootMap() const noexcept { return lootMap; }

    std::string GetString(const std::string& key, const std::string& defaultValue = "") const {
        return ExtractString(FindByPath(rootJson, key), defaultValue);
    }

    uint32_t GetOffset(const std::string& key, uint32_t defaultValue = 0) const {
        return static_cast<uint32_t>(ExtractUInt64(FindByPath(rootJson, key), defaultValue));
    }

    uint64_t GetOffset64(const std::string& key, uint64_t defaultValue = 0) const {
        return ExtractUInt64(FindByPath(rootJson, key), defaultValue);
    }
};
