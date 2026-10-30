// Overgeared - rarity for crafted and appraised weapons, armor, clothing and accessories
//
// When the player crafts an item (or appraises one from the inventory), the plugin:
//   1) rolls a rarity (Common, Rare, Epic, Legendary, Mythic);
//   2) creates a COPY of the item (a new form, only for the player);
//   3) applies the rarity's stats:
//      - damage, value, attack speed and critical bonus (weapons) / defense (armor):
//        rolled inside the rarity's range (critical damage follows the new damage);
//      - weight: FIXED reduction per rarity (the item gets X times lighter);
//   4) swaps the original item for the copy in the inventory.
//
// The game does not save the data of these copies by itself, so the plugin keeps a
// list (in a file next to the save, the "cosave") and rebuilds the data on load.
//
// Armor: defense gets a per-rarity multiplier, limited by a maximum bonus over the
// base and by a defense cap per piece type.
//
// Improvement recipes (grindstone / armor workbench) are created per rarity.

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <mutex>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// SKSE Menu Framework header (downloaded from the mod's Nexus page).
// It must be in the same folder as this file.
#include "SKSEMenuFramework.h"

namespace
{
    // ------------------------------------------------------------------
    // Log (written to two places so it is easy to find)
    // ------------------------------------------------------------------
    std::ofstream g_logA;  // SKSE log folder (Documents/My Games/...)
    std::ofstream g_logB;  // Data/SKSE/Plugins (with MO2 this ends up in the Overwrite folder)

    void WriteLog(const std::string& s)
    {
        if (g_logA.is_open()) {
            g_logA << s << "\n";
            g_logA.flush();
        }
        if (g_logB.is_open()) {
            g_logB << s << "\n";
            g_logB.flush();
        }
    }

    template <class... Args>
    void Log(std::format_string<Args...> fmt, Args&&... args)
    {
        WriteLog(std::format(fmt, std::forward<Args>(args)...));
    }

    void OpenLogFile(std::ofstream& stream, const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        stream.open(path, std::ios::out | std::ios::trunc);
    }

    void SetupLog()
    {
        if (auto folder = SKSE::log::log_directory()) {
            OpenLogFile(g_logA, *folder / "Overgeared.log");
        }
        OpenLogFile(g_logB, std::filesystem::path("Data") / "SKSE" / "Plugins" / "Overgeared.log");
        Log("Overgeared started.");
    }

    // ------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------
    struct Range
    {
        float min;
        float max;
    };

    struct RarityDef
    {
        std::string name;
        float       chance;     // relative chance
        Range       damage;     // damage multiplier
        Range       value;      // sell value multiplier
        float       weightDiv;  // weight divisor (fixed): 2.0 = half the weight
        Range       speed;      // attack speed multiplier
        Range       critBonus;  // bonus added to the critical damage multiplier (1 + bonus)
        Range       armor;      // defense multiplier (armor)
    };

    struct Ingredient
    {
        std::string   plugin;  // plugin that owns the item (e.g. "Skyrim.esm")
        std::uint32_t id;      // local FormID inside that plugin
        int           count;
    };

    // Default recipes (all items are from Skyrim.esm; soul gems are the FILLED versions).
    inline std::array<std::vector<Ingredient>, 5> DefaultRecipes()
    {
        const std::string sk = "Skyrim.esm";
        return { {
            { { sk, 0x0005ACE4, 3 } },                                                                              // Common: 3 Iron Ingot
            { { sk, 0x0005ACE4, 5 }, { sk, 0x000DB8A2, 2 } },                                                       // Rare: 5 Iron, 2 Dwarven
            { { sk, 0x000DB8A2, 5 }, { sk, 0x0005AD9E, 5 }, { sk, 0x0002E4F3, 1 } },                                // Epic: 5 Dwarven, 5 Gold, 1 Common Soul Gem (filled)
            { { sk, 0x0005AD9E, 10 }, { sk, 0x0005ADA1, 5 }, { sk, 0x0002E4FF, 1 } },                               // Legendary: 10 Gold, 5 Malachite, 1 Grand Soul Gem (filled)
            { { sk, 0x0005AD9D, 15 }, { sk, 0x0005ADA1, 5 }, { sk, 0x0005AD99, 5 }, { sk, 0x0002E504, 1 } },        // Mythic: 15 Ebony, 5 Malachite, 5 Orichalcum, 1 Black Soul Gem (filled)
        } };
    }

    struct Config
    {
        std::array<RarityDef, 5> rarities{ {
            { "Common", 60.0f, { 1.00f, 1.24f }, { 1.00f, 1.10f }, 1.00f, { 1.00f, 1.24f }, { 0.00f, 0.50f }, { 1.00f, 1.10f } },
            { "Rare", 25.0f, { 1.25f, 1.74f }, { 1.10f, 1.30f }, 1.25f, { 1.25f, 1.49f }, { 0.50f, 1.00f }, { 1.10f, 1.25f } },
            { "Epic", 10.0f, { 1.75f, 2.99f }, { 1.30f, 1.60f }, 1.75f, { 1.50f, 1.74f }, { 1.00f, 1.50f }, { 1.25f, 1.45f } },
            { "Legendary", 4.0f, { 3.00f, 4.99f }, { 1.60f, 1.99f }, 2.25f, { 1.75f, 1.99f }, { 1.50f, 2.00f }, { 1.45f, 1.75f } },
            { "Mythic", 1.0f, { 5.00f, 5.00f }, { 2.00f, 2.00f }, 3.00f, { 2.00f, 2.00f }, { 2.00f, 2.00f }, { 2.00f, 2.00f } },
        } };
        bool onlyWhenCrafting = true;
        bool notify = true;
        bool affectWeapons = true;
        bool affectArmor = true;
        bool affectClothing = true;  // clothing (ArmorClothing keyword)
        bool affectJewelry = true;   // jewelry and accessories (rings, amulets, glasses, etc.)

        // Result window after crafting
        bool resultWindow = true;
        int  resultMinRarity = 0;  // 0 = Common ... 4 = Mythic: the window appears from this rarity up
        int  resultMaxItems = 2;   // how many items fit in one box (the rest becomes "+N earlier")

        // Appraiser: inventory key that rolls the rarity of an item you already own
        bool appraiseEnabled = true;
        bool appraiseWindow = true;  // result window when appraising (always shown, any rarity)
        int  appraiseKey = 0x18;  // O key (DirectInput code)

        // Improvement recipes: lets rarity items be improved at the grindstone / armor workbench.
        // One recipe per rarity (the game reuses the same recipe for every improvement step).
        bool                                  refineEnabled = true;
        std::array<std::vector<Ingredient>, 5> recipes = DefaultRecipes();

        // Appraiser: block items that would lose data when swapped (upgraded, stolen, player-enchanted, renamed, poisoned).
        // Equipped items are always blocked.
        bool blockLossyItems = false;

        // Armor: defense limits
        float armorMaxMult = 2.0f;  // maximum bonus relative to base defense (0 = no limit)
        // DISPLAYED defense cap per piece type (0 = no cap). Never lowers a piece below its base.
        float capBody = 80.0f;
        float capHead = 45.0f;
        float capHands = 45.0f;
        float capFeet = 45.0f;
        float capShield = 70.0f;
        float capOther = 45.0f;
    };

    Config g_cfg;

    bool        g_craftingMenuOpen = false;
    int         g_forceRarity = 0;  // 0 = random; 1..5 = forces that rarity (for testing)
    std::mutex  g_summaryMutex;
    std::string g_lastSummary = "(no item crafted yet)";

    std::string Trim(std::string s)
    {
        const char* ws = " \t\r\n";
        const auto  b = s.find_first_not_of(ws);
        if (b == std::string::npos) {
            return {};
        }
        const auto e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    }

    bool ToBool(const std::string& v)
    {
        return v == "1" || v == "true" || v == "True" || v == "TRUE";
    }

    // "[plugin|]hexid:count, [plugin|]hexid:count, ..."  (plugin defaults to Skyrim.esm)
    std::vector<Ingredient> ParseRecipe(const std::string& text)
    {
        std::vector<Ingredient> out;
        std::size_t             pos = 0;
        while (pos <= text.size()) {
            auto        comma = text.find(',', pos);
            std::string tok = Trim(text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
            pos = comma == std::string::npos ? text.size() + 1 : comma + 1;
            if (tok.empty()) {
                continue;
            }
            Ingredient ing{ "Skyrim.esm", 0, 1 };
            auto       bar = tok.find('|');
            if (bar != std::string::npos) {
                ing.plugin = Trim(tok.substr(0, bar));
                tok = Trim(tok.substr(bar + 1));
            }
            auto colon = tok.find(':');
            ing.id = static_cast<std::uint32_t>(std::stoul(tok.substr(0, colon), nullptr, 16));
            if (colon != std::string::npos) {
                ing.count = std::max(1, std::stoi(tok.substr(colon + 1)));
            }
            out.push_back(ing);
        }
        return out;
    }

    std::string FormatRecipe(const std::vector<Ingredient>& recipe)
    {
        std::string out;
        for (std::size_t i = 0; i < recipe.size(); ++i) {
            out += std::format("{}{}|{:08X}:{}", i ? ", " : "", recipe[i].plugin, recipe[i].id, recipe[i].count);
        }
        return out;
    }

    void LoadConfig()
    {
        std::ifstream file("Data\\SKSE\\Plugins\\Overgeared.ini");
        if (!file.is_open()) {
            Log("Overgeared.ini not found, using default values.");
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '[') {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = Trim(line.substr(0, eq));
            const std::string val = Trim(line.substr(eq + 1));

            try {
                if (key == "OnlyWhenCrafting") {
                    g_cfg.onlyWhenCrafting = ToBool(val);
                } else if (key == "Notify") {
                    g_cfg.notify = ToBool(val);
                } else if (key == "AffectWeapons") {
                    g_cfg.affectWeapons = ToBool(val);
                } else if (key == "AffectArmor") {
                    g_cfg.affectArmor = ToBool(val);
                } else if (key == "ResultWindow") {
                    g_cfg.resultWindow = ToBool(val);
                } else if (key == "ResultMaxItems") {
                    g_cfg.resultMaxItems = std::clamp(std::stoi(val), 1, 5);
                } else if (key == "ResultMinRarity") {
                    g_cfg.resultMinRarity = std::clamp(std::stoi(val) - 1, 0, 4);
                } else if (key == "AppraiseWindow") {
                    g_cfg.appraiseWindow = ToBool(val);
                } else if (key == "AppraiseEnabled") {
                    g_cfg.appraiseEnabled = ToBool(val);
                } else if (key == "AppraiseKey") {
                    g_cfg.appraiseKey = std::stoi(val);
                } else if (key == "RefineEnabled") {
                    g_cfg.refineEnabled = ToBool(val);
                } else if (key == "BlockLossyItems") {
                    g_cfg.blockLossyItems = ToBool(val);
                } else if (key.rfind("RefineRecipe", 0) == 0 && key.size() == 13 && key[12] >= '1' && key[12] <= '5') {
                    g_cfg.recipes[key[12] - '1'] = ParseRecipe(val);
                } else if (key == "AffectClothing") {
                    g_cfg.affectClothing = ToBool(val);
                } else if (key == "AffectJewelry") {
                    g_cfg.affectJewelry = ToBool(val);
                } else if (key == "ArmorMaxMultiplier") {
                    g_cfg.armorMaxMult = std::stof(val);
                } else if (key == "CapBody") {
                    g_cfg.capBody = std::stof(val);
                } else if (key == "CapHead") {
                    g_cfg.capHead = std::stof(val);
                } else if (key == "CapHands") {
                    g_cfg.capHands = std::stof(val);
                } else if (key == "CapFeet") {
                    g_cfg.capFeet = std::stof(val);
                } else if (key == "CapShield") {
                    g_cfg.capShield = std::stof(val);
                } else if (key == "CapOther") {
                    g_cfg.capOther = std::stof(val);
                } else {
                    for (int i = 0; i < 5; ++i) {
                        const std::string n = std::to_string(i + 1);
                        auto&             r = g_cfg.rarities[i];
                        if (key == "Name" + n) r.name = val;
                        else if (key == "Chance" + n) r.chance = std::stof(val);
                        else if (key == "DamageMin" + n) r.damage.min = std::stof(val);
                        else if (key == "DamageMax" + n) r.damage.max = std::stof(val);
                        else if (key == "ValueMin" + n) r.value.min = std::stof(val);
                        else if (key == "ValueMax" + n) r.value.max = std::stof(val);
                        else if (key == "WeightDivisor" + n) r.weightDiv = std::stof(val);
                        else if (key == "CritBonusMin" + n) r.critBonus.min = std::stof(val);
                        else if (key == "CritBonusMax" + n) r.critBonus.max = std::stof(val);
                        else if (key == "SpeedMin" + n) r.speed.min = std::stof(val);
                        else if (key == "SpeedMax" + n) r.speed.max = std::stof(val);
                        else if (key == "ArmorMin" + n) r.armor.min = std::stof(val);
                        else if (key == "ArmorMax" + n) r.armor.max = std::stof(val);
                    }
                }
            } catch (...) {
                Log("Invalid value in ini: {} = {}", key, val);
            }
        }
        Log("Configuration loaded.");
    }

    bool SaveConfig()
    {
        std::ofstream f("Data\\SKSE\\Plugins\\Overgeared.ini", std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            Log("ERROR: could not write Overgeared.ini");
            return false;
        }
        f << "; Overgeared - configuration saved from the in-game menu\n";
        f << "; Damage/Value/Speed/Armor = multiplier rolled between Min and Max\n";
        f << "; WeightDivisor = weight is divided by this value (fixed)\n";
        f << "; CritBonus = bonus added to the critical damage multiplier (1 + bonus)\n\n";
        f << std::format("OnlyWhenCrafting={}\nNotify={}\n", g_cfg.onlyWhenCrafting ? 1 : 0, g_cfg.notify ? 1 : 0);
        f << std::format("AffectWeapons={}\nAffectArmor={}\n", g_cfg.affectWeapons ? 1 : 0, g_cfg.affectArmor ? 1 : 0);
        f << std::format("AffectClothing={}\nAffectJewelry={}\n", g_cfg.affectClothing ? 1 : 0, g_cfg.affectJewelry ? 1 : 0);
        f << "; Result window: ResultMinRarity 1=Common 2=Rare 3=Epic 4=Legendary 5=Mythic\n";
        f << std::format("ResultWindow={}\nResultMinRarity={}\nResultMaxItems={}\n", g_cfg.resultWindow ? 1 : 0, g_cfg.resultMinRarity + 1, g_cfg.resultMaxItems);
        f << "; Appraiser: AppraiseKey = DirectInput key code (24 = O, 25 = P, 0 = disabled)\n";
        f << std::format("AppraiseEnabled={}\nAppraiseWindow={}\nAppraiseKey={}\n", g_cfg.appraiseEnabled ? 1 : 0, g_cfg.appraiseWindow ? 1 : 0, g_cfg.appraiseKey);
        f << "; Improvement recipes: one line per rarity, format [plugin|]hexFormID:count, ...\n";
        f << std::format("RefineEnabled={}\n", g_cfg.refineEnabled ? 1 : 0);
        for (int i = 0; i < 5; ++i) {
            f << std::format("RefineRecipe{}={}\n", i + 1, FormatRecipe(g_cfg.recipes[i]));
        }
        f << std::format("BlockLossyItems={}\n", g_cfg.blockLossyItems ? 1 : 0);
        f << "; Armor: ArmorMaxMultiplier = maximum defense bonus vs the base (0 = no limit)\n";
        f << "; Cap* = displayed defense cap per piece type (0 = no cap)\n";
        f << std::format("ArmorMaxMultiplier={:.2f}\n", g_cfg.armorMaxMult);
        f << std::format("CapBody={:.0f}\nCapHead={:.0f}\nCapHands={:.0f}\nCapFeet={:.0f}\nCapShield={:.0f}\nCapOther={:.0f}\n\n",
                         g_cfg.capBody, g_cfg.capHead, g_cfg.capHands, g_cfg.capFeet, g_cfg.capShield, g_cfg.capOther);
        for (int i = 0; i < 5; ++i) {
            const auto& r = g_cfg.rarities[i];
            const int   n = i + 1;
            f << std::format("Name{0}={1}\nChance{0}={2:.2f}\n", n, r.name, r.chance);
            f << std::format("DamageMin{0}={1:.2f}\nDamageMax{0}={2:.2f}\n", n, r.damage.min, r.damage.max);
            f << std::format("ValueMin{0}={1:.2f}\nValueMax{0}={2:.2f}\n", n, r.value.min, r.value.max);
            f << std::format("WeightDivisor{0}={1:.2f}\n", n, r.weightDiv);
            f << std::format("SpeedMin{0}={1:.2f}\nSpeedMax{0}={2:.2f}\n", n, r.speed.min, r.speed.max);
            f << std::format("CritBonusMin{0}={1:.2f}\nCritBonusMax{0}={2:.2f}\n", n, r.critBonus.min, r.critBonus.max);
            f << std::format("ArmorMin{0}={1:.2f}\nArmorMax{0}={2:.2f}\n\n", n, r.armor.min, r.armor.max);
        }
        Log("Overgeared.ini saved from the menu.");
        return true;
    }

    // ------------------------------------------------------------------
    // Rolling
    // ------------------------------------------------------------------
    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // rolls in 0.01 steps within the range
    float RollRange(const Range& r)
    {
        const int lo = static_cast<int>(std::lround(r.min * 100.0f));
        const int hi = static_cast<int>(std::lround(r.max * 100.0f));
        if (hi <= lo) {
            return static_cast<float>(lo) / 100.0f;
        }
        std::uniform_int_distribution<int> step(lo, hi);
        return static_cast<float>(step(Rng())) / 100.0f;
    }

    // ------------------------------------------------------------------
    int RarityIndexByName(const std::string& name);  // defined below

    // Registry of copies (what must survive saving/loading)
    // ------------------------------------------------------------------
    struct CloneRecord
    {
        RE::FormID  baseID{ 0 };
        std::string rarity;
        float       damage{ 1.0f };
        float       value{ 1.0f };
        float       weightDiv{ 1.0f };  // weight is divided by this
        float       crit{ 1.0f };       // final CRITICAL DAMAGE multiplier (1 + bonus)
        float       speed{ 1.0f };
        bool          isArmor{ false };
        float         armor{ 1.0f };    // rolled defense multiplier (armor)
        std::uint32_t armorFinal{ 0 };  // stored final defense (limits already applied)
    };

    std::unordered_map<RE::FormID, CloneRecord> g_clones;  // key: FormID of the copy
    std::unordered_set<RE::FormID>              g_built;   // copies already built this session

    // The library's Copy() does not copy the model, animation type, etc.
    // So we copy each part of the weapon explicitly.
    template <class T>
    void CopyComp(RE::TESObjectWEAP* dst, RE::TESObjectWEAP* src)
    {
        RE::BaseFormComponent* d = static_cast<T*>(dst);
        RE::BaseFormComponent* s = static_cast<T*>(src);
        d->CopyComponent(s);
    }

    void CopyWeaponParts(RE::TESObjectWEAP* dst, RE::TESObjectWEAP* src)
    {
        // object bounds (the part specific to TESBoundObject, right after TESForm)
        {
            auto*             dstB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(dst)) + sizeof(RE::TESForm);
            auto*             srcB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(src)) + sizeof(RE::TESForm);
            const std::size_t sizeB = sizeof(RE::TESBoundObject) - sizeof(RE::TESForm);
            std::memcpy(dstB, srcB, sizeB);
        }

        // components (model, equip type, sounds, keywords...)
        CopyComp<RE::TESFullName>(dst, src);
        CopyComp<RE::TESModelTextureSwap>(dst, src);
        CopyComp<RE::TESIcon>(dst, src);
        CopyComp<RE::TESEnchantableForm>(dst, src);
        CopyComp<RE::BGSDestructibleObjectForm>(dst, src);
        CopyComp<RE::BGSEquipType>(dst, src);
        CopyComp<RE::BGSPreloadable>(dst, src);
        CopyComp<RE::TESWeightForm>(dst, src);
        CopyComp<RE::TESValueForm>(dst, src);
        CopyComp<RE::TESAttackDamageForm>(dst, src);
        CopyComp<RE::BGSMessageIcon>(dst, src);
        CopyComp<RE::BGSPickupPutdownSounds>(dst, src);
        CopyComp<RE::BGSBlockBashData>(dst, src);
        CopyComp<RE::BGSKeywordForm>(dst, src);
        CopyComp<RE::TESDescription>(dst, src);

        // weapon-specific fields (animation type, reach, speed, attack sounds...):
        // everything that comes after the last component
        auto*             dstTail = reinterpret_cast<std::byte*>(static_cast<RE::TESDescription*>(dst)) + sizeof(RE::TESDescription);
        auto*             srcTail = reinterpret_cast<std::byte*>(static_cast<RE::TESDescription*>(src)) + sizeof(RE::TESDescription);
        const std::size_t offset = static_cast<std::size_t>(dstTail - reinterpret_cast<std::byte*>(dst));
        const std::size_t size = sizeof(RE::TESObjectWEAP) - offset;
        std::memcpy(dstTail, srcTail, size);
        Log("  parts copied: own fields from offset 0x{:X}, {} bytes", offset, size);
    }

    // Fills (or rebuilds) the copy from the original weapon + multipliers.
    bool BuildClone(RE::TESObjectWEAP* clone, RE::TESObjectWEAP* base, const CloneRecord& rec)
    {
        if (!clone || !base) {
            return false;
        }

        CopyWeaponParts(clone, base);

        auto rnd16 = [](float v) {
            return static_cast<std::uint16_t>(std::clamp<long>(std::lround(v), 0L, 65535L));
        };

        clone->attackDamage = std::max<std::uint16_t>(1, rnd16(static_cast<float>(base->attackDamage) * rec.damage));
        clone->value = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(static_cast<float>(base->value) * rec.value)));
        clone->weight = std::max(0.0f, base->weight / std::max(0.01f, rec.weightDiv));
        // Critical damage follows the weapon's NEW damage, keeping the ratio the
        // original weapon had (half the damage on most weapons), and only then
        // applies the rarity's critical bonus.
        {
            const float baseDmg = static_cast<float>(base->attackDamage);
            const float ratio = baseDmg > 0.0f ? static_cast<float>(base->criticalData.damage) / baseDmg : 0.5f;
            clone->criticalData.damage = rnd16(static_cast<float>(clone->attackDamage) * ratio * rec.crit);
        }
        clone->weaponData.speed = base->weaponData.speed * rec.speed;

        const std::string newName = std::format("[{}] {}", rec.rarity, base->GetName());
        clone->fullName = newName.c_str();

        Log("  copy {:08X} of {:08X}: name='{}' damage {}->{} value {}->{} weight {:.2f}->{:.2f} crit {}->{} speed {:.2f}->{:.2f} keywords={} model='{}'",
            clone->GetFormID(), base->GetFormID(), newName,
            base->attackDamage, clone->attackDamage,
            base->value, clone->value,
            base->weight, clone->weight,
            base->criticalData.damage, clone->criticalData.damage,
            base->weaponData.speed, clone->weaponData.speed,
            clone->numKeywords,
            clone->GetModel() ? clone->GetModel() : "(no model)");
        g_built.insert(clone->GetFormID());
        return true;
    }

    // ------------------------------------------------------------------
    // Armor
    // ------------------------------------------------------------------
    constexpr float kRatingScale = 100.0f;  // the game stores defense multiplied by 100

    // For each candidate "part" class of the armor: if the armor actually inherits
    // it, copy the component; otherwise skip it (no compile error).
    template <class T, class Derived>
    int CopyCompIfBase(Derived* dst, Derived* src)
    {
        if constexpr (std::is_base_of_v<T, Derived>) {
            RE::BaseFormComponent* d = static_cast<T*>(dst);
            RE::BaseFormComponent* s = static_cast<T*>(src);
            d->CopyComponent(s);
            return 1;
        } else {
            return 0;
        }
    }

    // Where part T of the armor ends (in bytes, from the start of the object).
    template <class T, class Derived>
    std::size_t SubobjectEnd(Derived* obj)
    {
        if constexpr (std::is_base_of_v<T, Derived>) {
            auto* sub = static_cast<T*>(obj);
            return static_cast<std::size_t>(reinterpret_cast<std::byte*>(sub) - reinterpret_cast<std::byte*>(obj)) + sizeof(T);
        } else {
            return 0;
        }
    }

    void CopyArmorParts(RE::TESObjectARMO* dst, RE::TESObjectARMO* src)
    {
        // object bounds
        {
            auto*             dstB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(dst)) + sizeof(RE::TESForm);
            auto*             srcB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(src)) + sizeof(RE::TESForm);
            const std::size_t sizeB = sizeof(RE::TESBoundObject) - sizeof(RE::TESForm);
            std::memcpy(dstB, srcB, sizeB);
        }

        int copied = 0;
        copied += CopyCompIfBase<RE::TESFullName>(dst, src);
        copied += CopyCompIfBase<RE::TESRaceForm>(dst, src);
        copied += CopyCompIfBase<RE::TESModelTextureSwap>(dst, src);
        copied += CopyCompIfBase<RE::TESIcon>(dst, src);
        copied += CopyCompIfBase<RE::TESEnchantableForm>(dst, src);
        copied += CopyCompIfBase<RE::BGSDestructibleObjectForm>(dst, src);
        copied += CopyCompIfBase<RE::BGSPickupPutdownSounds>(dst, src);
        copied += CopyCompIfBase<RE::TESBipedModelForm>(dst, src);
        copied += CopyCompIfBase<RE::BGSEquipType>(dst, src);
        copied += CopyCompIfBase<RE::BGSBipedObjectForm>(dst, src);
        copied += CopyCompIfBase<RE::BGSBlockBashData>(dst, src);
        copied += CopyCompIfBase<RE::BGSKeywordForm>(dst, src);
        copied += CopyCompIfBase<RE::TESDescription>(dst, src);
        copied += CopyCompIfBase<RE::TESWeightForm>(dst, src);
        copied += CopyCompIfBase<RE::TESValueForm>(dst, src);
        copied += CopyCompIfBase<RE::BGSMessageIcon>(dst, src);
        copied += CopyCompIfBase<RE::BGSPreloadable>(dst, src);

        // end of the last component = start of the armor's own fields
        const std::size_t end = std::max({
            SubobjectEnd<RE::TESBoundObject>(dst),
            SubobjectEnd<RE::TESFullName>(dst),
            SubobjectEnd<RE::TESRaceForm>(dst),
            SubobjectEnd<RE::TESModelTextureSwap>(dst),
            SubobjectEnd<RE::TESIcon>(dst),
            SubobjectEnd<RE::TESEnchantableForm>(dst),
            SubobjectEnd<RE::BGSDestructibleObjectForm>(dst),
            SubobjectEnd<RE::BGSPickupPutdownSounds>(dst),
            SubobjectEnd<RE::TESBipedModelForm>(dst),
            SubobjectEnd<RE::BGSEquipType>(dst),
            SubobjectEnd<RE::BGSBipedObjectForm>(dst),
            SubobjectEnd<RE::BGSBlockBashData>(dst),
            SubobjectEnd<RE::BGSKeywordForm>(dst),
            SubobjectEnd<RE::TESDescription>(dst),
            SubobjectEnd<RE::TESWeightForm>(dst),
            SubobjectEnd<RE::TESValueForm>(dst),
            SubobjectEnd<RE::BGSMessageIcon>(dst),
            SubobjectEnd<RE::BGSPreloadable>(dst),
        });
        const std::size_t total = sizeof(RE::TESObjectARMO);
        if (end < total) {
            std::memcpy(reinterpret_cast<std::byte*>(dst) + end, reinterpret_cast<std::byte*>(src) + end, total - end);
        }
        Log("  parts copied (armor): {} components; own fields from 0x{:X}, {} bytes (total size 0x{:X})",
            copied, end, total > end ? total - end : 0, total);
    }

    bool HasKeywordId(RE::BGSKeywordForm* form, const char* editorID)
    {
        if (!form || !form->keywords) {
            return false;
        }
        for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
            auto* kw = form->keywords[i];
            if (kw) {
                const char* id = kw->GetFormEditorID();
                if (id && std::string_view(id) == editorID) {
                    return true;
                }
            }
        }
        return false;
    }

    std::uint32_t SlotMask(RE::TESObjectARMO* a)
    {
        return static_cast<std::uint32_t>(a->GetSlotMask());
    }

    enum class ArmorCat
    {
        Body,
        Head,
        Hands,
        Feet,
        Shield,
        Other
    };

    // slots: 30 head(bit0) 31 hair(1) 32 body(2) 33 hands(3) 34 forearms(4)
    //        35 amulet(5) 36 ring(6) 37 feet(7) 38 calves(8) 39 shield(9) 42 circlet(12)
    ArmorCat Categorize(RE::TESObjectARMO* a)
    {
        const std::uint32_t m = SlotMask(a);
        if (m & (1u << 2)) return ArmorCat::Body;
        if (m & (1u << 9)) return ArmorCat::Shield;
        if (m & ((1u << 3) | (1u << 4))) return ArmorCat::Hands;
        if (m & ((1u << 7) | (1u << 8))) return ArmorCat::Feet;
        if (m & ((1u << 0) | (1u << 1) | (1u << 11) | (1u << 12))) return ArmorCat::Head;
        return ArmorCat::Other;
    }

    const char* CatName(ArmorCat c)
    {
        switch (c) {
        case ArmorCat::Body: return "chest";
        case ArmorCat::Head: return "head";
        case ArmorCat::Hands: return "hands";
        case ArmorCat::Feet: return "feet";
        case ArmorCat::Shield: return "shield";
        default: return "other";
        }
    }

    float CapFor(ArmorCat c)
    {
        switch (c) {
        case ArmorCat::Body: return g_cfg.capBody;
        case ArmorCat::Head: return g_cfg.capHead;
        case ArmorCat::Hands: return g_cfg.capHands;
        case ArmorCat::Feet: return g_cfg.capFeet;
        case ArmorCat::Shield: return g_cfg.capShield;
        default: return g_cfg.capOther;
        }
    }

    bool IsEligibleArmor(RE::TESObjectARMO* a)
    {
        if (!a || !g_cfg.affectArmor) {
            return false;
        }
        auto*               kw = static_cast<RE::BGSKeywordForm*>(a);
        const std::uint32_t m = SlotMask(a);

        // "real armor" piece: occupies head, body, hands, feet or shield
        const bool wornArmor = (m & ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7) | (1u << 9))) != 0;
        const bool jewelry = !wornArmor && (HasKeywordId(kw, "ArmorJewelry") || (m & ((1u << 5) | (1u << 6))) != 0);
        const bool clothing = !wornArmor && !jewelry && HasKeywordId(kw, "ArmorClothing");

        if (jewelry && !g_cfg.affectJewelry) {
            Log("  armor {:08X} ignored: jewelry/accessories turned off in the menu (slots 0x{:X})", a->GetFormID(), m);
            return false;
        }
        if (clothing && !g_cfg.affectClothing) {
            Log("  armor {:08X} ignored: clothing turned off in the menu (slots 0x{:X})", a->GetFormID(), m);
            return false;
        }
        // Pieces with no defense are included too: name, value and weight get the rarity,
        // only the defense stays at zero.
        return true;
    }

    // final defense: rolled multiplier, limited by the maximum bonus and by the piece's cap.
    // The cap never makes a piece end up below the defense it already had.
    std::uint32_t ComputeArmorRating(RE::TESObjectARMO* base, float mult)
    {
        const float baseRating = static_cast<float>(base->armorRating);
        float       r = baseRating * mult;
        if (g_cfg.armorMaxMult > 0.0f) {
            r = std::min(r, baseRating * std::max(1.0f, g_cfg.armorMaxMult));
        }
        const float cap = CapFor(Categorize(base)) * kRatingScale;
        if (cap > 0.0f) {
            r = std::min(r, std::max(baseRating, cap));
        }
        return static_cast<std::uint32_t>(std::max(0L, std::lround(r)));
    }

    bool BuildArmorClone(RE::TESObjectARMO* clone, RE::TESObjectARMO* base, CloneRecord& rec)
    {
        if (!clone || !base) {
            return false;
        }

        CopyArmorParts(clone, base);

        if (rec.armorFinal == 0) {
            rec.armorFinal = ComputeArmorRating(base, rec.armor);
        }
        clone->armorRating = rec.armorFinal;
        clone->value = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(static_cast<float>(base->value) * rec.value)));
        clone->weight = std::max(0.0f, base->weight / std::max(0.01f, rec.weightDiv));

        const std::string newName = std::format("[{}] {}", rec.rarity, base->GetName());
        clone->fullName = newName.c_str();

        Log("  armor {:08X} of {:08X}: name='{}' type={} defense {:.1f}->{:.1f} (displayed; raw {}->{}) value {}->{} weight {:.2f}->{:.2f} keywords={}",
            clone->GetFormID(), base->GetFormID(), newName, CatName(Categorize(base)),
            static_cast<float>(base->armorRating) / kRatingScale, static_cast<float>(clone->armorRating) / kRatingScale,
            base->armorRating, clone->armorRating,
            base->value, clone->value,
            base->weight, clone->weight,
            clone->numKeywords);
        g_built.insert(clone->GetFormID());
        return true;
    }

    void RepopulateAll(const char* reason)
    {
        Log("Rebuilding copies ({}): {} registered.", reason, g_clones.size());
        for (auto& [cloneID, rec] : g_clones) {
            if (g_built.contains(cloneID)) {
                continue;  // already built this session
            }
            if (rec.isArmor) {
                auto* aClone = RE::TESForm::LookupByID<RE::TESObjectARMO>(cloneID);
                auto* aBase = RE::TESForm::LookupByID<RE::TESObjectARMO>(rec.baseID);
                if (!aClone) {
                    Log("  armor {:08X} no longer exists in the game (ignored).", cloneID);
                    continue;
                }
                if (!aBase) {
                    Log("  original armor {:08X} not found (mod removed?). Copy {:08X} ignored.", rec.baseID, cloneID);
                    continue;
                }
                BuildArmorClone(aClone, aBase, rec);
                continue;
            }
            auto* clone = RE::TESForm::LookupByID<RE::TESObjectWEAP>(cloneID);
            auto* base = RE::TESForm::LookupByID<RE::TESObjectWEAP>(rec.baseID);
            if (!clone) {
                Log("  copy {:08X} no longer exists in the game (ignored).", cloneID);
                continue;
            }
            if (!base) {
                Log("  original weapon {:08X} not found (mod removed?). Copy {:08X} ignored.", rec.baseID, cloneID);
                continue;
            }
            BuildClone(clone, base, rec);
        }
    }

    // ------------------------------------------------------------------
    // Saving alongside the save game (cosave)
    // ------------------------------------------------------------------
    constexpr std::uint32_t kUniqueID = 'OVG1';
    constexpr std::uint32_t kRecordClones = 'CLON';
    constexpr std::uint32_t kRecordVersion = 2;

    void SaveCallback(SKSE::SerializationInterface* intfc)
    {
        if (!intfc->OpenRecord(kRecordClones, kRecordVersion)) {
            Log("ERROR: could not open the cosave record.");
            return;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(g_clones.size());
        intfc->WriteRecordData(count);
        for (auto& [cloneID, rec] : g_clones) {
            intfc->WriteRecordData(cloneID);
            intfc->WriteRecordData(rec.baseID);
            intfc->WriteRecordData(rec.damage);
            intfc->WriteRecordData(rec.value);
            intfc->WriteRecordData(rec.weightDiv);
            intfc->WriteRecordData(rec.crit);
            intfc->WriteRecordData(rec.speed);
            const std::uint32_t len = static_cast<std::uint32_t>(rec.rarity.size());
            intfc->WriteRecordData(len);
            if (len > 0) {
                intfc->WriteRecordData(rec.rarity.data(), len);
            }
            const std::uint8_t isArmor = rec.isArmor ? 1 : 0;
            intfc->WriteRecordData(isArmor);
            intfc->WriteRecordData(rec.armor);
            intfc->WriteRecordData(rec.armorFinal);
        }
        Log("Cosave written: {} copies.", count);
    }

    void LoadCallback(SKSE::SerializationInterface* intfc)
    {
        g_clones.clear();
        g_built.clear();

        std::uint32_t type = 0, version = 0, length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecordClones || (version != 1 && version != 2)) {
                continue;
            }
            std::uint32_t count = 0;
            intfc->ReadRecordData(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID  oldClone = 0, oldBase = 0;
                CloneRecord rec;
                intfc->ReadRecordData(oldClone);
                intfc->ReadRecordData(oldBase);
                intfc->ReadRecordData(rec.damage);
                intfc->ReadRecordData(rec.value);
                intfc->ReadRecordData(rec.weightDiv);
                intfc->ReadRecordData(rec.crit);
                intfc->ReadRecordData(rec.speed);
                std::uint32_t len = 0;
                intfc->ReadRecordData(len);
                if (len > 0 && len < 256) {
                    rec.rarity.resize(len);
                    intfc->ReadRecordData(rec.rarity.data(), len);
                }
                if (const int idx = RarityIndexByName(rec.rarity); idx >= 0) {
                    rec.rarity = g_cfg.rarities[idx].name;  // old saves get the current (English) name
                }
                if (version >= 2) {
                    std::uint8_t isArmor = 0;
                    intfc->ReadRecordData(isArmor);
                    intfc->ReadRecordData(rec.armor);
                    intfc->ReadRecordData(rec.armorFinal);
                    rec.isArmor = isArmor != 0;
                }

                RE::FormID newClone = 0, newBase = 0;
                if (!intfc->ResolveFormID(oldClone, newClone)) {
                    Log("  could not resolve copy {:08X}.", oldClone);
                    continue;
                }
                if (!intfc->ResolveFormID(oldBase, newBase)) {
                    Log("  could not resolve original item {:08X}.", oldBase);
                    continue;
                }
                rec.baseID = newBase;
                g_clones[newClone] = rec;
            }
        }
        Log("Cosave read: {} copies.", g_clones.size());
        RepopulateAll("save load");
    }

    void RevertCallback(SKSE::SerializationInterface*)
    {
        g_clones.clear();
        g_built.clear();
        Log("Revert: copy registry cleared.");
    }

    // ------------------------------------------------------------------
    // Creating a new copy for a crafted item
    // ------------------------------------------------------------------
    bool IsEligible(RE::TESBoundObject* obj)
    {
        auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;
        if (!weap) {
            return false;
        }
        if (weap->IsStaff()) {
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Result window (the game's message box)
    // ------------------------------------------------------------------
    std::mutex               g_pendingMutex;
    std::vector<std::string> g_pendingMessages;

    void QueueResultWindow(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingMessages.push_back(text);
    }

    // Must run on the game's main thread.
    void FlushResultWindow()
    {
        std::vector<std::string> msgs;
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            msgs.swap(g_pendingMessages);
        }
        if (msgs.empty()) {
            return;
        }
        std::string       all;
        const std::size_t maxItems = static_cast<std::size_t>(std::max(1, g_cfg.resultMaxItems));
        const std::size_t start = msgs.size() > maxItems ? msgs.size() - maxItems : 0;
        if (start > 0) {
            all += std::format("(+{} earlier items)\n\n", start);
        }
        for (std::size_t i = start; i < msgs.size(); ++i) {
            all += msgs[i];
            if (i + 1 < msgs.size()) {
                all += "\n\n";
            }
        }
        Log("Showing result window ({} items).", msgs.size());
        RE::DebugMessageBox(all.c_str());
    }

    // ------------------------------------------------------------------
    // Improvement recipe: creates, for each copy, an improvement recipe
    // at the grindstone (weapons) or the armor workbench.
    // ------------------------------------------------------------------
    std::unordered_map<RE::FormID, RE::BGSConstructibleObject*> g_recipes;  // copy -> recipe

    template <class C>
    void FillRecipe(C* cobj, RE::TESForm* created, RE::BGSKeyword* bench, const std::vector<std::pair<RE::TESBoundObject*, int>>& items)
    {
        const std::size_t n = items.size();
        auto**            arr = static_cast<RE::ContainerObject**>(RE::malloc(sizeof(RE::ContainerObject*) * n));
        for (std::size_t i = 0; i < n; ++i) {
            auto* entry = static_cast<RE::ContainerObject*>(RE::malloc(sizeof(RE::ContainerObject)));
            std::memset(entry, 0, sizeof(RE::ContainerObject));
            entry->obj = items[i].first;
            entry->count = items[i].second;
            arr[i] = entry;
        }
        if constexpr (requires { cobj->requiredItems; }) {
            cobj->requiredItems.containerObjects = arr;
            cobj->requiredItems.numContainerObjects = static_cast<std::uint32_t>(n);
        } else {
            cobj->containerObjects = arr;
            cobj->numContainerObjects = static_cast<std::uint32_t>(n);
        }
        cobj->createdItem = created;
        cobj->benchKeyword = bench;
        if constexpr (requires { cobj->data.numConstructed; }) {
            cobj->data.numConstructed = 1;
        }
    }

    void EnsureRefineRecipe(RE::TESBoundObject* clone, bool isArmor, int rarityIndex)
    {
        if (!clone || !g_cfg.refineEnabled || rarityIndex < 0 || rarityIndex > 4) {
            return;
        }
        if (g_recipes.contains(clone->GetFormID())) {
            return;
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* bench = RE::TESForm::LookupByID<RE::BGSKeyword>(isArmor ? 0x000ADB78u : 0x00088108u);
        if (!dh || !bench) {
            Log("  improvement recipe NOT created for {:08X}: data handler={} workbench keyword={}",
                clone->GetFormID(), dh != nullptr, bench != nullptr);
            return;
        }

        std::vector<std::pair<RE::TESBoundObject*, int>> items;
        for (const auto& ing : g_cfg.recipes[rarityIndex]) {
            auto* obj = dh->LookupForm<RE::TESBoundObject>(ing.id, ing.plugin);
            if (!obj) {
                Log("  recipe ingredient not found: {}|{:08X} (skipped)", ing.plugin, ing.id);
                continue;
            }
            items.emplace_back(obj, ing.count);
        }
        if (items.empty()) {
            Log("  improvement recipe NOT created for {:08X}: no valid ingredients for rarity {}.", clone->GetFormID(), rarityIndex + 1);
            return;
        }

        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSConstructibleObject>();
        auto* cobj = factory ? factory->Create() : nullptr;
        if (!cobj) {
            Log("  improvement recipe NOT created: recipe factory unavailable.");
            return;
        }

        FillRecipe(cobj, clone, bench, items);
        dh->GetFormArray<RE::BGSConstructibleObject>().push_back(cobj);
        g_recipes[clone->GetFormID()] = cobj;

        std::string list;
        for (auto& [obj, count] : items) {
            list += std::format("{}{}x {}", list.empty() ? "" : ", ", count, obj->GetName());
        }
        const char* benchId = bench->GetFormEditorID();
        Log("  improvement recipe created {:08X} for {:08X} (workbench {}): {}",
            cobj->GetFormID(), clone->GetFormID(), benchId ? benchId : "?", list);
    }

    int RarityIndexByName(const std::string& name)
    {
        for (int i = 0; i < 5; ++i) {
            if (g_cfg.rarities[i].name == name) {
                return i;
            }
        }
        // names used by early test versions (Portuguese): keep old saves working
        static const char* legacy[5] = { "Comum", "Rara", "Epica", "Lendaria", "Mitica" };
        for (int i = 0; i < 5; ++i) {
            if (name == legacy[i]) {
                return i;
            }
        }
        return -1;
    }

    void EnsureAllRecipes()
    {
        for (auto& [cloneID, rec] : g_clones) {
            if (!g_built.contains(cloneID)) {
                continue;
            }
            auto* form = RE::TESForm::LookupByID<RE::TESBoundObject>(cloneID);
            EnsureRefineRecipe(form, rec.isArmor, RarityIndexByName(rec.rarity));
        }
    }

    // ------------------------------------------------------------------
    // Extra data of an inventory item
    // ------------------------------------------------------------------
    // Extra data that cannot survive the swap. Equipped items are always blocked; the others
    // (improved, player-made enchantment, custom name, poison, owned/stolen) are blocked only
    // when "BlockLossyItems" is on. Anything else (enchantment charge, hotkey...) is just lost.
    const char* BlockingExtra(RE::ExtraDataList* list)
    {
        if (!list) {
            return nullptr;
        }
        using T = RE::ExtraDataType;
        if (list->HasType(T::kWorn) || list->HasType(T::kWornLeft)) return "equipped";  // always blocked
        if (!g_cfg.blockLossyItems) {
            return nullptr;  // everything else is allowed (extra data is lost in the swap)
        }
        if (list->HasType(T::kHealth)) return "improved";
        if (list->HasType(T::kEnchantment)) return "player-made enchantment";
        if (list->HasType(T::kTextDisplayData)) return "custom name";
        if (list->HasType(T::kPoison)) return "poisoned";
        if (list->HasType(T::kOwnership)) return "owned (stolen) item";
        return nullptr;
    }

    // Looks for a unit of the item that can be swapped. outList stays null for a plain unit,
    // or points to the harmless extra-data list of the chosen unit.
    bool FindRemovable(RE::PlayerCharacter* player, RE::TESBoundObject* obj, RE::ExtraDataList*& outList, std::string* whyNot = nullptr)
    {
        outList = nullptr;
        auto inv = player->GetInventory([obj](RE::TESBoundObject& o) { return &o == obj; });
        auto it = inv.find(obj);
        if (it == inv.end() || it->second.first < 1) {
            if (whyNot) *whyNot = "item not found in the inventory";
            return false;
        }
        const int               total = it->second.first;
        int                     extraTotal = 0;
        RE::ExtraDataList*      harmless = nullptr;
        const char*             blockedBy = nullptr;
        RE::InventoryEntryData* entry = it->second.second.get();
        if (entry && entry->extraLists) {
            for (auto* list : *entry->extraLists) {
                if (!list) {
                    continue;
                }
                extraTotal += std::max<int>(1, list->GetCount());
                if (const char* why = BlockingExtra(list)) {
                    blockedBy = why;
                } else if (!harmless) {
                    harmless = list;
                }
            }
        }
        if (harmless) {
            outList = harmless;
            return true;
        }
        if (total - extraTotal > 0) {
            return true;  // plain unit
        }
        if (whyNot) *whyNot = blockedBy ? blockedBy : "no unit available";
        return false;
    }

    const RarityDef* PickRarity()
    {
        float total = 0.0f;
        for (auto& r : g_cfg.rarities) {
            total += std::max(0.0f, r.chance);
        }
        std::uniform_real_distribution<float> pick(0.0f, std::max(total, 0.0001f));
        float                                  x = pick(Rng());
        const RarityDef*                       chosen = &g_cfg.rarities[0];
        for (auto& r : g_cfg.rarities) {
            const float w = std::max(0.0f, r.chance);
            if (x < w) {
                chosen = &r;
                break;
            }
            x -= w;
        }
        if (g_forceRarity >= 1 && g_forceRarity <= 5) {
            chosen = &g_cfg.rarities[g_forceRarity - 1];
            Log("  rarity forced from the menu: {}", chosen->name);
        }
        return chosen;
    }

    bool ProcessNewItem(RE::FormID itemID, bool fromAppraisal = false)
    {
        Log("ProcessNewItem {:08X}", itemID);

        if (g_clones.contains(itemID)) {
            Log("  already one of our copies, ignored.");
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::LookupByID(itemID);
        auto* weap = form ? form->As<RE::TESObjectWEAP>() : nullptr;
        auto* armo = form ? form->As<RE::TESObjectARMO>() : nullptr;

        const bool okWeap = weap && g_cfg.affectWeapons && IsEligible(weap);
        const bool okArmo = armo && IsEligibleArmor(armo);
        if (!player || (!okWeap && !okArmo)) {
            Log("  ignored (not an eligible weapon or armor).");
            return false;
        }
        RE::TESBoundObject* baseObj = okWeap ? static_cast<RE::TESBoundObject*>(weap) : static_cast<RE::TESBoundObject*>(armo);

        RE::ExtraDataList* removeList = nullptr;
        std::string        whyNot;
        if (!FindRemovable(player, baseObj, removeList, &whyNot)) {
            Log("  no swappable unit: {}.", whyNot);
            return false;
        }

        // 1) rarity and 2) multipliers
        const RarityDef* chosen = PickRarity();
        CloneRecord      rec;
        rec.baseID = baseObj->GetFormID();
        rec.rarity = chosen->name;
        rec.isArmor = okArmo;
        rec.value = RollRange(chosen->value);
        rec.weightDiv = chosen->weightDiv;
        if (okWeap) {
            rec.damage = RollRange(chosen->damage);
            rec.crit = 1.0f + RollRange(chosen->critBonus);
            rec.speed = RollRange(chosen->speed);
            Log("  roll (weapon): {} damage x{:.2f} value x{:.2f} weight /{:.2f} crit x{:.2f} speed x{:.2f}",
                rec.rarity, rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);
        } else {
            rec.armor = RollRange(chosen->armor);
            Log("  roll (armor): {} defense x{:.2f} value x{:.2f} weight /{:.2f}",
                rec.rarity, rec.armor, rec.value, rec.weightDiv);
        }

        // 3) create the copy
        RE::TESBoundObject* clone = nullptr;
        if (okWeap) {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectWEAP>();
            auto* c = factory ? factory->Create() : nullptr;
            if (!c) {
                Log("  FAILURE: could not create the weapon copy.");
                return false;
            }
            Log("  copy created with FormID {:08X}", c->GetFormID());
            if (!BuildClone(c, weap, rec)) {
                Log("  FAILURE while filling the copy.");
                return false;
            }
            clone = c;
        } else {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectARMO>();
            auto* c = factory ? factory->Create() : nullptr;
            if (!c) {
                Log("  FAILURE: could not create the armor copy.");
                return false;
            }
            Log("  copy created with FormID {:08X}", c->GetFormID());
            if (!BuildArmorClone(c, armo, rec)) {
                Log("  FAILURE while filling the copy.");
                return false;
            }
            clone = c;
        }
        g_clones[clone->GetFormID()] = rec;
        const int rarityIndex = static_cast<int>(chosen - &g_cfg.rarities[0]);
        EnsureRefineRecipe(clone, okArmo, rarityIndex);

        // result window text
        const bool showWindow = fromAppraisal ? g_cfg.appraiseWindow : (g_cfg.resultWindow && rarityIndex >= g_cfg.resultMinRarity);
        if (showWindow) {
            std::string txt = std::format("[{}] {}\n", rec.rarity, baseObj->GetName());
            if (okWeap) {
                auto* cw = clone->As<RE::TESObjectWEAP>();
                txt += std::format("Damage: {} to {}\n", weap->attackDamage, cw->attackDamage);
                txt += std::format("Critical damage: {} to {}\n", weap->criticalData.damage, cw->criticalData.damage);
                txt += std::format("Speed: {:.2f} to {:.2f}\n", weap->weaponData.speed, cw->weaponData.speed);
                txt += std::format("Weight: {:.1f} to {:.1f}\n", weap->weight, cw->weight);
                txt += std::format("Value: {} to {}", weap->value, cw->value);
            } else {
                auto*       ca = clone->As<RE::TESObjectARMO>();
                const bool  limited = ca->armorRating < static_cast<std::uint32_t>(std::lround(static_cast<float>(armo->armorRating) * rec.armor));
                txt += std::format("Defense: {:.1f} to {:.1f}{}\n", static_cast<float>(armo->armorRating) / kRatingScale,
                                   static_cast<float>(ca->armorRating) / kRatingScale, limited ? " (capped)" : "");
                txt += std::format("Weight: {:.1f} to {:.1f}\n", armo->weight, ca->weight);
                txt += std::format("Value: {} to {}", armo->value, ca->value);
            }
            QueueResultWindow(txt);
        }

        {
            std::lock_guard<std::mutex> lock(g_summaryMutex);
            if (okWeap) {
                g_lastSummary = std::format("[{}] {} | damage x{:.2f}  value x{:.2f}  weight /{:.2f}  critical x{:.2f}  speed x{:.2f}",
                                            rec.rarity, baseObj->GetName(), rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);
            } else {
                g_lastSummary = std::format("[{}] {} ({}) | defense x{:.2f} -> {:.1f} (base {:.1f})  value x{:.2f}  weight /{:.2f}",
                                            rec.rarity, baseObj->GetName(), CatName(Categorize(armo)), rec.armor,
                                            static_cast<float>(rec.armorFinal) / kRatingScale,
                                            static_cast<float>(armo->armorRating) / kRatingScale, rec.value, rec.weightDiv);
            }
        }

        // 4) swap in the inventory
        player->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, removeList, nullptr);
        Log("  original item removed{}", removeList ? " (unit with harmless extra data)" : "");
        player->AddObjectToContainer(clone, nullptr, 1, nullptr);
        Log("  copy added to the inventory");

        if (g_cfg.notify) {
            const std::string msg = std::format("{}: {}", rec.rarity, baseObj->GetName());
            RE::DebugNotification(msg.c_str());
        }
        if (!g_craftingMenuOpen) {
            FlushResultWindow();
        }
        Log("  done");
        return true;
    }

    // ------------------------------------------------------------------
    // Game events
    // ------------------------------------------------------------------
    bool g_busy = false;
    std::chrono::steady_clock::time_point g_cooldownUntil{};

    // ------------------------------------------------------------------
    // Appraiser: rolls the rarity of an item the player already owns.
    // Flow: key in the inventory -> the inventory is closed -> the item is swapped -> result window.
    // (The inventory list does not refresh while the menu is open, so the swap only happens after it closes.)
    // ------------------------------------------------------------------
    RE::FormID                            g_pendingAppraisal = 0;
    std::chrono::steady_clock::time_point g_pendingSince{};

    void Toast(const char* msg)
    {
        Log("Player notice: {}", msg);
        RE::DebugNotification(msg);
    }

    // FormID of the item selected in the inventory (reads the UI list; works with SkyUI)
    RE::FormID SelectedInventoryFormID()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
            return 0;
        }
        auto movie = ui->GetMovieView(RE::InventoryMenu::MENU_NAME);
        if (!movie) {
            Log("Appraiser: inventory interface unavailable.");
            return 0;
        }
        RE::GFxValue entry;
        if (!movie->GetVariable(&entry, "_root.Menu_mc.inventoryLists.itemList.selectedEntry") || !entry.IsObject()) {
            Log("Appraiser: could not find the selected item in the inventory list.");
            return 0;
        }
        RE::GFxValue fid;
        if (!entry.GetMember("formId", &fid) || !fid.IsNumber()) {
            Log("Appraiser: the selected item does not report a formId.");
            return 0;
        }
        const auto n = static_cast<std::int64_t>(fid.GetNumber());
        return static_cast<RE::FormID>(static_cast<std::uint32_t>(n));
    }

    // 1) key pressed with the inventory open: validate and ask to close the inventory
    void RequestAppraisal()
    {
        Log("Appraiser: key pressed in the inventory.");
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || g_pendingAppraisal != 0) {
            return;
        }
        const RE::FormID id = SelectedInventoryFormID();
        if (id == 0) {
            Toast("Could not read the selected item.");
            return;
        }
        if (g_clones.contains(id)) {
            Toast("This item already has a rarity.");
            return;
        }
        auto* form = RE::TESForm::LookupByID(id);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        const bool isWeapon = obj && obj->As<RE::TESObjectWEAP>();
        const bool isArmor = obj && obj->As<RE::TESObjectARMO>();
        if (!isWeapon && !isArmor) {
            Toast("Only weapons and armor can be appraised.");
            return;
        }
        {
            RE::ExtraDataList* l = nullptr;
            std::string        why;
            if (!FindRemovable(player, obj, l, &why)) {
                Log("Appraiser: item {:08X} refused: {}.", id, why);
                Toast("This item cannot be appraised right now (it is equipped, or blocked by your settings).");
                return;
            }
        }

        g_pendingAppraisal = id;
        g_pendingSince = std::chrono::steady_clock::now();
        Log("Appraiser: item {:08X} accepted; closing the inventory to swap the item.", id);
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    // 2) the inventory closed: swap the item and show the result window
    void RunAppraisal(RE::FormID id)
    {
        Log("Appraiser: inventory closed, appraising {:08X}.", id);
        g_busy = true;
        const bool ok = ProcessNewItem(id, true);
        g_busy = false;
        g_cooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (!ok) {
            Toast("This item cannot receive a rarity (check the options in the menu).");
        }
    }

    class InputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink* Get()
        {
            static InputSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_event || !*a_event || !g_cfg.appraiseEnabled || g_cfg.appraiseKey == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* e = *a_event; e; e = e->next) {
                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                auto* b = static_cast<RE::ButtonEvent*>(e);
                if (!b->IsDown() || b->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                if (static_cast<int>(b->GetIDCode()) != g_cfg.appraiseKey) {
                    continue;
                }
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
                    SKSE::GetTaskInterface()->AddTask([]() { RequestAppraisal(); });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ------------------------------------------------------------------
    // Return the original items (to uninstall the mod safely)
    // ------------------------------------------------------------------
    bool        g_revertArmed = false;
    std::string g_revertStatus;  // protected by g_summaryMutex

    void RevertAllInInventory()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        Log("Return: starting.");
        auto inv = player->GetInventory([](RE::TESBoundObject& o) { return g_clones.contains(o.GetFormID()); });
        std::vector<std::pair<RE::TESBoundObject*, int>> work;
        for (auto& [obj, data] : inv) {
            work.emplace_back(obj, data.first);
        }

        int kinds = 0, units = 0, missing = 0;
        g_busy = true;
        for (auto& [obj, count] : work) {
            auto  rec = g_clones.find(obj->GetFormID());
            auto* base = rec != g_clones.end() ? RE::TESForm::LookupByID<RE::TESBoundObject>(rec->second.baseID) : nullptr;
            if (!base) {
                Log("  item {:08X}: the original was not found (the item's mod removed?). Kept.", obj->GetFormID());
                ++missing;
                continue;
            }
            player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            player->AddObjectToContainer(base, nullptr, count, nullptr);
            Log("  {:08X} x{} -> {:08X}", obj->GetFormID(), count, base->GetFormID());
            ++kinds;
            units += count;
        }
        g_busy = false;
        g_cooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

        const std::string msg = std::format("Returned {} items ({} kinds).{}", units, kinds,
                                            missing > 0 ? std::format(" {} could not be returned (see the log).", missing) : "");
        Log("Return finished: {}", msg);
        {
            std::lock_guard<std::mutex> lock(g_summaryMutex);
            g_revertStatus = msg;
        }
        RE::DebugNotification(msg.c_str());
    }

    class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuSink* Get()
        {
            static MenuSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*               a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
                g_craftingMenuOpen = a_event->opening;
                if (!a_event->opening) {
                    SKSE::GetTaskInterface()->AddTask([]() { FlushResultWindow(); });
                }
            }
            if (a_event && !a_event->opening && a_event->menuName == RE::InventoryMenu::MENU_NAME && g_pendingAppraisal != 0) {
                const RE::FormID id = g_pendingAppraisal;
                g_pendingAppraisal = 0;
                const auto waited = std::chrono::steady_clock::now() - g_pendingSince;
                if (waited < std::chrono::seconds(5)) {
                    SKSE::GetTaskInterface()->AddTask([id]() { RunAppraisal(id); });
                } else {
                    Log("Appraiser: request expired, ignored.");
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static ContainerSink* Get()
        {
            static ContainerSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent*               a_event,
                                              RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!a_event || g_busy || std::chrono::steady_clock::now() < g_cooldownUntil) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->newContainer != 0x14 || a_event->oldContainer != 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (g_cfg.onlyWhenCrafting && !g_craftingMenuOpen) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const RE::FormID base = a_event->baseObj;
            const int        count = std::max<int>(1, a_event->itemCount);

            // we only care about weapons and armor
            auto*      form = RE::TESForm::LookupByID(base);
            const bool isWeapon = form && form->As<RE::TESObjectWEAP>() && g_cfg.affectWeapons;
            const bool isArmor = form && form->As<RE::TESObjectARMO>() && g_cfg.affectArmor;
            if (!isWeapon && !isArmor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            Log("Event: {} {:08X} x{} entered the inventory while crafting", isArmor ? "armor" : "weapon", base, count);

            SKSE::GetTaskInterface()->AddTask([base, count]() {
                g_busy = true;
                for (int i = 0; i < count; ++i) {
                    ProcessNewItem(base);
                }
                g_busy = false;
                g_cooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ------------------------------------------------------------------
    // Menu in the SKSE Menu Framework
    // ------------------------------------------------------------------
    std::string g_menuStatus;

    // two sliders (minimum and maximum) that never cross
    void RangeSliders(const char* name, Range& r, float lo, float hi)
    {
        ImGuiMCP::PushID(name);
        ImGuiMCP::Text("%s", name);
        const bool a = ImGuiMCP::SliderFloat("minimum", &r.min, lo, hi, "%.2f", 0);
        const bool b = ImGuiMCP::SliderFloat("maximum", &r.max, lo, hi, "%.2f", 0);
        if (a && r.min > r.max) {
            r.max = r.min;
        }
        if (b && r.max < r.min) {
            r.min = r.max;
        }
        ImGuiMCP::Spacing();
        ImGuiMCP::PopID();
    }

    // slider with its title on top, like the minimum/maximum ones
    void TitledSlider(const char* title, float* v, float lo, float hi, const char* fmt)
    {
        ImGuiMCP::PushID(title);
        ImGuiMCP::Text("%s", title);
        ImGuiMCP::SliderFloat("##v", v, lo, hi, fmt, 0);
        ImGuiMCP::Spacing();
        ImGuiMCP::PopID();
    }

    void __stdcall RenderMenu()
    {
        ImGuiMCP::Text("Overgeared - rarity for crafted gear");
        ImGuiMCP::TextWrapped("Changes apply to the NEXT items you craft or appraise. Items that already exist do not change.");
        ImGuiMCP::Separator();

        ImGuiMCP::Checkbox("Only affect crafted items", &g_cfg.onlyWhenCrafting);
        ImGuiMCP::Checkbox("Show a notification when crafting", &g_cfg.notify);
        ImGuiMCP::Checkbox("Affect weapons", &g_cfg.affectWeapons);
        ImGuiMCP::Checkbox("Affect armor", &g_cfg.affectArmor);
        ImGuiMCP::Checkbox("Affect clothing", &g_cfg.affectClothing);
        ImGuiMCP::Checkbox("Affect jewelry and accessories", &g_cfg.affectJewelry);
        ImGuiMCP::Spacing();
        ImGuiMCP::Checkbox("Result window when CRAFTING", &g_cfg.resultWindow);
        ImGuiMCP::Text("When crafting, show the window from this rarity up:");
        ImGuiMCP::Combo("##windowmin", &g_cfg.resultMinRarity, "Common\0Rare\0Epic\0Legendary\0Mythic\0");
        ImGuiMCP::SliderInt("Items per window", &g_cfg.resultMaxItems, 1, 5, "%d", 0);
        ImGuiMCP::TextWrapped("The numbers in the window are the item\'s BASE values. In the inventory the game adds the bonus from skills and perks.");

        ImGuiMCP::Spacing();
        ImGuiMCP::Text("Force rarity (for testing):");
        ImGuiMCP::Combo("##force", &g_forceRarity, "Random\0Common\0Rare\0Epic\0Legendary\0Mythic\0");

        ImGuiMCP::Spacing();
        ImGuiMCP::Text("Last item crafted:");
        {
            std::string copy;
            {
                std::lock_guard<std::mutex> lock(g_summaryMutex);
                copy = g_lastSummary;
            }
            ImGuiMCP::TextWrapped("%s", copy.c_str());
        }

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button("Save to .ini")) {
            g_menuStatus = SaveConfig() ? "Saved to Data/SKSE/Plugins/Overgeared.ini" : "ERROR while saving (see the log)";
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Reload from .ini")) {
            LoadConfig();
            g_menuStatus = "Reloaded from .ini";
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Reset to defaults")) {
            g_cfg = Config{};
            g_menuStatus = "Defaults restored (not saved yet)";
        }
        if (!g_menuStatus.empty()) {
            ImGuiMCP::TextWrapped("%s", g_menuStatus.c_str());
        }
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("Appraiser")) {
            ImGuiMCP::TextWrapped("With the inventory open, select a weapon or armor you already own and press the appraise key. "
                                  "The inventory closes, the item is swapped for a version with a rarity, and the result window appears.");
            ImGuiMCP::Checkbox("Enable the appraiser", &g_cfg.appraiseEnabled);
            ImGuiMCP::Checkbox("Result window when APPRAISING (always shown, any rarity)", &g_cfg.appraiseWindow);
            ImGuiMCP::TextWrapped("Key: code %d (AppraiseKey in the .ini; 24 = O, 25 = P, 0 disables). "
                                  "Typing that letter in the inventory search box also triggers it.", g_cfg.appraiseKey);
            ImGuiMCP::Checkbox("Block items that would lose data (improved, stolen, enchanted by you, renamed)", &g_cfg.blockLossyItems);
            ImGuiMCP::TextWrapped("Never appraises: items that already have a rarity, or equipped items. With the option above off, improved, stolen, renamed or player-enchanted items are appraised and lose that data. Enchantments that come with the item (from the game or a mod) are kept.");
        }
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("Uninstalling: return the original items")) {
            ImGuiMCP::TextWrapped("Items with a rarity are stored in your save and depend on this mod. Before uninstalling, "
                                  "return the originals. This swaps every rarity item in YOUR INVENTORY back to the original item.");
            ImGuiMCP::TextWrapped("Items on the ground, in chests or with followers must be picked up first. "
                                  "Improvements and enchantments you made on rarity items are LOST. Unequip everything first.");
            if (!g_revertArmed) {
                if (ImGuiMCP::Button("Prepare return")) {
                    g_revertArmed = true;
                }
            } else {
                ImGuiMCP::TextWrapped("Are you sure? This cannot be undone.");
                if (ImGuiMCP::Button("CONFIRM: return the originals")) {
                    g_revertArmed = false;
                    SKSE::GetTaskInterface()->AddTask([]() { RevertAllInInventory(); });
                }
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Cancel")) {
                    g_revertArmed = false;
                }
            }
            std::string status;
            {
                std::lock_guard<std::mutex> lock(g_summaryMutex);
                status = g_revertStatus;
            }
            if (!status.empty()) {
                ImGuiMCP::TextWrapped("%s", status.c_str());
            }
        }
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("Armor: defense limits")) {
            ImGuiMCP::TextWrapped("Maximum defense bonus relative to the base (0 = no limit). Applies to every piece.");
            ImGuiMCP::SliderFloat("Maximum bonus (x base)", &g_cfg.armorMaxMult, 0.0f, 5.0f, "%.2f", 0);
            ImGuiMCP::Spacing();
            ImGuiMCP::TextWrapped("Defense cap per piece type (value shown in the inventory; 0 = no cap). "
                                  "The cap never lowers a piece below the defense it already had.");
            ImGuiMCP::SliderFloat("Cap: chest", &g_cfg.capBody, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Cap: head", &g_cfg.capHead, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Cap: hands", &g_cfg.capHands, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Cap: feet", &g_cfg.capFeet, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Cap: shield", &g_cfg.capShield, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Cap: other pieces", &g_cfg.capOther, 0.0f, 700.0f, "%.0f", 0);
        }
        ImGuiMCP::Separator();

        for (int i = 0; i < 5; ++i) {
            auto& r = g_cfg.rarities[i];
            ImGuiMCP::PushID(i);
            if (ImGuiMCP::CollapsingHeader(r.name.c_str())) {
                TitledSlider("Chance (relative weight)", &r.chance, 0.0f, 100.0f, "%.1f");
                RangeSliders("Damage (multiplier)", r.damage, 0.5f, 8.0f);
                RangeSliders("Sell value (multiplier)", r.value, 0.5f, 8.0f);
                TitledSlider("Weight (fixed divisor: 2 = half the weight)", &r.weightDiv, 0.5f, 6.0f, "%.2f");
                RangeSliders("Attack speed (multiplier)", r.speed, 0.1f, 5.0f);
                RangeSliders("Critical damage: added bonus", r.critBonus, 0.0f, 6.0f);
                RangeSliders("Armor defense (multiplier)", r.armor, 0.5f, 5.0f);
            }
            ImGuiMCP::PopID();
        }
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            Log("SKSE Menu Framework not found: the Overgeared menu is disabled.");
            return;
        }
        SKSEMenuFramework::SetSection("Overgeared");
        SKSEMenuFramework::AddSectionItem("Rarities", RenderMenu);
        Log("Menu registered in the SKSE Menu Framework.");
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        switch (msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            RegisterMenu();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
            }
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::Get());
            }
            if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
                input->AddEventSink(InputSink::Get());
            }
            Log("Events registered.");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            RepopulateAll("after loading the game");
            EnsureAllRecipes();
            RE::DebugNotification("Overgeared active");
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    LoadConfig();

    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID(kUniqueID);
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
    }
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
