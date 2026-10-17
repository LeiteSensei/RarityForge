// RarityForge - versao inicial (prototipo)
//
// O que faz:
//   Quando o jogador forja uma arma ou armadura, o plugin sorteia uma raridade
//   (Comum, Rara, Epica, Lendaria, Mitica) e um multiplicador entre 1.00 e 5.00,
//   e aplica no item criado:
//     - o nome ganha uma etiqueta, ex: "[Rara] Espada de Ferro"
//     - o multiplicador e gravado no "ExtraHealth" do item (o mesmo dado que a
//       forja usa para o aprimoramento), que afeta dano/armadura e valor.
//
// IMPORTANTE: este codigo NAO foi compilado nem testado. Pode ter erros de
// compilacao ou comportamento diferente do esperado. Veja o LEIAME.md.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <string>

namespace
{
    // ------------------------------------------------------------------
    // Configuracao (lida de Data/SKSE/Plugins/RarityForge.ini)
    // ------------------------------------------------------------------
    struct RarityDef
    {
        std::string name;
        float       weight;   // chance relativa
        float       minMult;  // multiplicador minimo
        float       maxMult;  // multiplicador maximo
    };

    struct Config
    {
        std::array<RarityDef, 5> rarities{ {
            { "Comum", 60.0f, 1.00f, 1.24f },
            { "Rara", 25.0f, 1.25f, 1.74f },
            { "Epica", 10.0f, 1.75f, 2.99f },
            { "Lendaria", 4.0f, 3.00f, 4.99f },
            { "Mitica", 1.0f, 5.00f, 5.00f },
        } };
        bool  onlyWhenCrafting = true;
        bool  affectWeapons = true;
        bool  affectArmor = true;
        bool  notify = true;
        float healthScale = 1.0f;  // multiplica o valor gravado no ExtraHealth
    };

    Config g_cfg;

    bool g_craftingMenuOpen = false;
    bool g_busy = false;  // evita o plugin reagir aos proprios ajustes

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

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

    void LoadConfig()
    {
        std::ifstream file("Data\\SKSE\\Plugins\\RarityForge.ini");
        if (!file.is_open()) {
            spdlog::warn("RarityForge.ini nao encontrado, usando valores padrao.");
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
                if (key == "OnlyWhenCrafting") g_cfg.onlyWhenCrafting = ToBool(val);
                else if (key == "AffectWeapons") g_cfg.affectWeapons = ToBool(val);
                else if (key == "AffectArmor") g_cfg.affectArmor = ToBool(val);
                else if (key == "Notify") g_cfg.notify = ToBool(val);
                else if (key == "HealthScale") g_cfg.healthScale = std::stof(val);
                else {
                    // Chaves por raridade: Name1, Weight1, Min1, Max1 ... Name5...
                    for (int i = 0; i < 5; ++i) {
                        const std::string n = std::to_string(i + 1);
                        if (key == "Name" + n) g_cfg.rarities[i].name = val;
                        else if (key == "Weight" + n) g_cfg.rarities[i].weight = std::stof(val);
                        else if (key == "Min" + n) g_cfg.rarities[i].minMult = std::stof(val);
                        else if (key == "Max" + n) g_cfg.rarities[i].maxMult = std::stof(val);
                    }
                }
            } catch (...) {
                spdlog::warn("Valor invalido no ini: {} = {}", key, val);
            }
        }
        spdlog::info("Configuracao carregada.");
    }

    // ------------------------------------------------------------------
    // Sorteio
    // ------------------------------------------------------------------
    struct RollResult
    {
        std::string name;
        float       multiplier;
    };

    RollResult Roll()
    {
        float total = 0.0f;
        for (auto& r : g_cfg.rarities) {
            total += std::max(0.0f, r.weight);
        }

        std::uniform_real_distribution<float> pick(0.0f, total);
        float                                  x = pick(Rng());

        const RarityDef* chosen = &g_cfg.rarities[0];
        for (auto& r : g_cfg.rarities) {
            const float w = std::max(0.0f, r.weight);
            if (x < w) {
                chosen = &r;
                break;
            }
            x -= w;
        }

        // multiplicador em passos de 0.01 (1.01, 1.02, ... 4.99, 5.00)
        const int lo = static_cast<int>(std::lround(chosen->minMult * 100.0f));
        const int hi = static_cast<int>(std::lround(chosen->maxMult * 100.0f));
        int       v = lo;
        if (hi > lo) {
            std::uniform_int_distribution<int> step(lo, hi);
            v = step(Rng());
        }
        return { chosen->name, static_cast<float>(v) / 100.0f };
    }

    // ------------------------------------------------------------------
    // Filtro de itens
    // ------------------------------------------------------------------
    bool HasKeyword(RE::BGSKeywordForm* form, const char* editorID)
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

    bool IsEligible(RE::TESBoundObject* obj)
    {
        if (!obj) {
            return false;
        }

        if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
            if (!g_cfg.affectWeapons) {
                return false;
            }
            if (weap->IsStaff()) {
                return false;
            }
            return true;
        }

        if (auto* armo = obj->As<RE::TESObjectARMO>()) {
            if (!g_cfg.affectArmor) {
                return false;
            }
            // roupas e joias nao entram
            if (HasKeyword(armo, "ArmorClothing") || HasKeyword(armo, "ArmorJewelry")) {
                return false;
            }
            return true;
        }

        return false;
    }

    // ------------------------------------------------------------------
    // Aplicacao da raridade
    // ------------------------------------------------------------------
    bool IsTagged(RE::ExtraDataList* list)
    {
        if (!list) {
            return false;
        }
        auto* tdd = list->GetByType<RE::ExtraTextDisplayData>();
        if (!tdd) {
            return false;
        }
        const char* n = tdd->displayName.c_str();
        return n && n[0] == '[';
    }

    void Decorate(RE::ExtraDataList* list, RE::TESBoundObject* obj, const RollResult& r)
    {
        // 1) multiplicador (dano / armadura / valor)
        auto* health = list->GetByType<RE::ExtraHealth>();
        if (!health) {
            health = RE::BSExtraData::Create<RE::ExtraHealth>();
            list->Add(health);
        }
        health->health = r.multiplier * g_cfg.healthScale;

        // 2) nome com etiqueta
        auto*       tdd = list->GetByType<RE::ExtraTextDisplayData>();
        std::string baseName = obj->GetName();
        if (tdd && tdd->displayName.c_str() && tdd->displayName.c_str()[0] != '\0') {
            baseName = tdd->displayName.c_str();
        }
        if (!tdd) {
            tdd = RE::BSExtraData::Create<RE::ExtraTextDisplayData>();
            list->Add(tdd);
        }
        const std::string newName = std::format("[{}] {}", r.name, baseName);
        tdd->SetName(newName.c_str());

        spdlog::info("Item {:08X} ({}) -> {} x{:.2f}", obj->GetFormID(), baseName, r.name, r.multiplier);
    }

    void ProcessNewItem(RE::FormID baseID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(baseID);
        if (!player || !IsEligible(obj)) {
            return;
        }

        auto inv = player->GetInventory([obj](RE::TESBoundObject& o) { return &o == obj; });
        auto it = inv.find(obj);
        if (it == inv.end()) {
            return;
        }

        const int                totalCount = it->second.first;
        RE::InventoryEntryData*  entry = it->second.second.get();

        int                 extraTotal = 0;
        RE::ExtraDataList*  untaggedList = nullptr;
        if (entry && entry->extraLists) {
            for (auto* list : *entry->extraLists) {
                if (!list) {
                    continue;
                }
                const int c = std::max<int>(1, list->GetCount());
                extraTotal += c;
                if (!untaggedList && c == 1 && !IsTagged(list)) {
                    untaggedList = list;
                }
            }
        }
        const int plainCount = totalCount - extraTotal;

        const RollResult r = Roll();

        if (plainCount > 0) {
            // Caminho A: item "simples" (sem dados extras). Removemos 1 e
            // devolvemos 1 com um bloco de dados extras proprio.
            // O construtor de ExtraDataList nao esta disponivel na biblioteca,
            // entao pedimos memoria ao jogo e zeramos (= bloco vazio valido).
            void* mem = RE::malloc(sizeof(RE::ExtraDataList));
            if (!mem) {
                return;
            }
            std::memset(mem, 0, sizeof(RE::ExtraDataList));
            auto* list = static_cast<RE::ExtraDataList*>(mem);
            Decorate(list, obj, r);
            player->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            player->AddObjectToContainer(obj, list, 1, nullptr);
        } else if (untaggedList) {
            // Caminho B: o item ja tem dados extras (ex: aprimoramento). Alteramos no lugar.
            Decorate(untaggedList, obj, r);
        } else {
            return;
        }

        if (g_cfg.notify) {
            const std::string msg = std::format("{}: {} (x{:.2f})", r.name, obj->GetName(), r.multiplier);
            RE::DebugNotification(msg.c_str());
        }
    }

    // ------------------------------------------------------------------
    // Eventos do jogo
    // ------------------------------------------------------------------
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
            if (!a_event || g_busy) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // item novo (veio de "lugar nenhum") entrando no inventario do jogador (0x14)
            if (a_event->newContainer != 0x14 || a_event->oldContainer != 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (g_cfg.onlyWhenCrafting && !g_craftingMenuOpen) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const RE::FormID base = a_event->baseObj;
            const int        count = std::max<int>(1, a_event->itemCount);

            // Executa no fluxo principal do jogo, fora do evento, por seguranca.
            SKSE::GetTaskInterface()->AddTask([base, count]() {
                g_busy = true;
                for (int i = 0; i < count; ++i) {
                    ProcessNewItem(base);
                }
                g_busy = false;
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
            }
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::Get());
            }
            spdlog::info("RarityForge: eventos registrados.");
        }
    }

    void SetupLog()
    {
        auto folder = SKSE::log::log_directory();
        if (!folder) {
            return;
        }
        auto path = *folder / "RarityForge.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto logger = std::make_shared<spdlog::logger>("RarityForge", std::move(sink));
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    spdlog::info("RarityForge carregando...");
    LoadConfig();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
