// Overgeared v3 - PROTOTIPO de copias
//
// Quando o jogador forja uma ARMA, o plugin:
//   1) sorteia uma raridade (Comum, Rara, Epica, Lendaria, Mitica);
//   2) cria uma COPIA da arma (um item novo, so do jogador);
//   3) aplica os atributos da raridade:
//      - dano, valor, velocidade de ataque e bonus de critico: sorteados
//        dentro da faixa da raridade (o dano critico acompanha o dano novo);
//      - peso: reducao FIXA por raridade (a arma fica X vezes mais leve);
//   4) troca a arma forjada pela copia no inventario.
//
// Como o jogo nao grava sozinho os dados dessas copias, o plugin guarda uma
// lista (num arquivo ao lado do save, o "cosave") e refaz os dados ao carregar.
//
// Nesta versao: so armas. Armaduras ficam sem raridade.
//
// IMPORTANTE: codigo NAO testado. Veja o texto que acompanha.

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
#include <unordered_map>
#include <unordered_set>

// Cabecalho do SKSE Menu Framework (baixado da pagina do mod no Nexus).
// Precisa estar na mesma pasta que este arquivo.
#include "SKSEMenuFramework.h"

namespace
{
    // ------------------------------------------------------------------
    // Log (grava em dois lugares para facilitar achar)
    // ------------------------------------------------------------------
    std::ofstream g_logA;  // pasta de logs do SKSE (Documentos/My Games/...)
    std::ofstream g_logB;  // Data/SKSE/Plugins (no MO2, vai para a pasta Overwrite)

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
        Log("Overgeared v3 (prototipo de copias) iniciado.");
    }

    // ------------------------------------------------------------------
    // Configuracao
    // ------------------------------------------------------------------
    struct Range
    {
        float min;
        float max;
    };

    struct RarityDef
    {
        std::string name;
        float       chance;     // chance relativa
        Range       damage;     // multiplicador de dano
        Range       value;      // multiplicador de valor de venda
        float       weightDiv;  // divisor do peso (fixo): 2.0 = metade do peso
        Range       speed;      // multiplicador de velocidade de ataque
        Range       critBonus;  // bonus somado ao multiplicador do dano critico (1 + bonus)
    };

    struct Config
    {
        std::array<RarityDef, 5> rarities{ {
            { "Comum", 60.0f, { 1.00f, 1.24f }, { 1.00f, 1.10f }, 1.00f, { 1.00f, 1.24f }, { 0.00f, 0.50f } },
            { "Rara", 25.0f, { 1.25f, 1.74f }, { 1.10f, 1.30f }, 1.25f, { 1.25f, 1.49f }, { 0.50f, 1.00f } },
            { "Epica", 10.0f, { 1.75f, 2.99f }, { 1.30f, 1.60f }, 1.75f, { 1.50f, 1.74f }, { 1.00f, 1.50f } },
            { "Lendaria", 4.0f, { 3.00f, 4.99f }, { 1.60f, 1.99f }, 2.25f, { 1.75f, 1.99f }, { 1.50f, 2.00f } },
            { "Mitica", 1.0f, { 5.00f, 5.00f }, { 2.00f, 2.00f }, 3.00f, { 2.00f, 2.00f }, { 2.00f, 2.00f } },
        } };
        bool onlyWhenCrafting = true;
        bool notify = true;
    };

    Config g_cfg;

    int         g_forceRarity = 0;  // 0 = aleatoria; 1..5 = forca a raridade (para testes)
    std::mutex  g_summaryMutex;
    std::string g_lastSummary = "(nenhuma arma forjada ainda)";

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
        std::ifstream file("Data\\SKSE\\Plugins\\Overgeared.ini");
        if (!file.is_open()) {
            Log("Overgeared.ini nao encontrado, usando valores padrao.");
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
                    }
                }
            } catch (...) {
                Log("Valor invalido no ini: {} = {}", key, val);
            }
        }
        Log("Configuracao carregada.");
    }

    bool SaveConfig()
    {
        std::ofstream f("Data\\SKSE\\Plugins\\Overgeared.ini", std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            Log("ERRO: nao foi possivel gravar o Overgeared.ini");
            return false;
        }
        f << "; Overgeared - configuracao salva pelo menu do jogo\n";
        f << "; Damage/Value/Speed = multiplicador sorteado entre Min e Max\n";
        f << "; WeightDivisor = peso dividido por este valor (fixo)\n";
        f << "; CritBonus = bonus somado ao multiplicador do dano critico (1 + bonus)\n\n";
        f << std::format("OnlyWhenCrafting={}\nNotify={}\n\n", g_cfg.onlyWhenCrafting ? 1 : 0, g_cfg.notify ? 1 : 0);
        for (int i = 0; i < 5; ++i) {
            const auto& r = g_cfg.rarities[i];
            const int   n = i + 1;
            f << std::format("Name{0}={1}\nChance{0}={2:.2f}\n", n, r.name, r.chance);
            f << std::format("DamageMin{0}={1:.2f}\nDamageMax{0}={2:.2f}\n", n, r.damage.min, r.damage.max);
            f << std::format("ValueMin{0}={1:.2f}\nValueMax{0}={2:.2f}\n", n, r.value.min, r.value.max);
            f << std::format("WeightDivisor{0}={1:.2f}\n", n, r.weightDiv);
            f << std::format("SpeedMin{0}={1:.2f}\nSpeedMax{0}={2:.2f}\n", n, r.speed.min, r.speed.max);
            f << std::format("CritBonusMin{0}={1:.2f}\nCritBonusMax{0}={2:.2f}\n\n", n, r.critBonus.min, r.critBonus.max);
        }
        Log("Overgeared.ini gravado pelo menu.");
        return true;
    }

    // ------------------------------------------------------------------
    // Sorteio
    // ------------------------------------------------------------------
    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // sorteia em passos de 0.01 dentro da faixa
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
    // Registro das copias (o que precisa sobreviver ao salvar/carregar)
    // ------------------------------------------------------------------
    struct CloneRecord
    {
        RE::FormID  baseID{ 0 };
        std::string rarity;
        float       damage{ 1.0f };
        float       value{ 1.0f };
        float       weightDiv{ 1.0f };  // peso dividido por isto
        float       crit{ 1.0f };       // multiplicador final do DANO CRITICO (1 + bonus)
        float       speed{ 1.0f };
    };

    std::unordered_map<RE::FormID, CloneRecord> g_clones;  // chave: FormID da copia
    std::unordered_set<RE::FormID>              g_built;   // copias ja montadas nesta sessao

    // O Copy() da biblioteca nao copia o modelo, o tipo de animacao etc.
    // Por isso copiamos cada parte da arma explicitamente.
    template <class T>
    void CopyComp(RE::TESObjectWEAP* dst, RE::TESObjectWEAP* src)
    {
        RE::BaseFormComponent* d = static_cast<T*>(dst);
        RE::BaseFormComponent* s = static_cast<T*>(src);
        d->CopyComponent(s);
    }

    void CopyWeaponParts(RE::TESObjectWEAP* dst, RE::TESObjectWEAP* src)
    {
        // caixa de limites do objeto (a parte propria do TESBoundObject, logo apos o TESForm)
        {
            auto*             dstB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(dst)) + sizeof(RE::TESForm);
            auto*             srcB = reinterpret_cast<std::byte*>(static_cast<RE::TESForm*>(src)) + sizeof(RE::TESForm);
            const std::size_t sizeB = sizeof(RE::TESBoundObject) - sizeof(RE::TESForm);
            std::memcpy(dstB, srcB, sizeB);
        }

        // componentes (modelo, equipamento, sons, palavras-chave...)
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

        // campos proprios da arma (tipo de animacao, alcance, velocidade, sons de ataque...):
        // tudo o que vem depois do ultimo componente
        auto*             dstTail = reinterpret_cast<std::byte*>(static_cast<RE::TESDescription*>(dst)) + sizeof(RE::TESDescription);
        auto*             srcTail = reinterpret_cast<std::byte*>(static_cast<RE::TESDescription*>(src)) + sizeof(RE::TESDescription);
        const std::size_t offset = static_cast<std::size_t>(dstTail - reinterpret_cast<std::byte*>(dst));
        const std::size_t size = sizeof(RE::TESObjectWEAP) - offset;
        std::memcpy(dstTail, srcTail, size);
        Log("  partes copiadas: campos proprios a partir do deslocamento 0x{:X}, {} bytes", offset, size);
    }

    // Preenche (ou refaz) a copia a partir da arma original + multiplicadores.
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
        // O dano critico acompanha o dano NOVO da arma, mantendo a proporcao que a
        // arma original tinha (na maioria das armas e metade do dano), e so entao
        // aplica o bonus de critico da raridade.
        {
            const float baseDmg = static_cast<float>(base->attackDamage);
            const float ratio = baseDmg > 0.0f ? static_cast<float>(base->criticalData.damage) / baseDmg : 0.5f;
            clone->criticalData.damage = rnd16(static_cast<float>(clone->attackDamage) * ratio * rec.crit);
        }
        clone->weaponData.speed = base->weaponData.speed * rec.speed;

        const std::string newName = std::format("[{}] {}", rec.rarity, base->GetName());
        clone->fullName = newName.c_str();

        Log("  copia {:08X} de {:08X}: nome='{}' dano {}->{} valor {}->{} peso {:.2f}->{:.2f} crit {}->{} vel {:.2f}->{:.2f} palavras-chave={} modelo='{}'",
            clone->GetFormID(), base->GetFormID(), newName,
            base->attackDamage, clone->attackDamage,
            base->value, clone->value,
            base->weight, clone->weight,
            base->criticalData.damage, clone->criticalData.damage,
            base->weaponData.speed, clone->weaponData.speed,
            clone->numKeywords,
            clone->GetModel() ? clone->GetModel() : "(sem modelo)");
        g_built.insert(clone->GetFormID());
        return true;
    }

    void RepopulateAll(const char* reason)
    {
        Log("Refazendo copias ({}): {} registradas.", reason, g_clones.size());
        for (auto& [cloneID, rec] : g_clones) {
            if (g_built.contains(cloneID)) {
                continue;  // ja montada nesta sessao
            }
            auto* clone = RE::TESForm::LookupByID<RE::TESObjectWEAP>(cloneID);
            auto* base = RE::TESForm::LookupByID<RE::TESObjectWEAP>(rec.baseID);
            if (!clone) {
                Log("  copia {:08X} nao existe mais no jogo (ignorada).", cloneID);
                continue;
            }
            if (!base) {
                Log("  arma original {:08X} nao encontrada (mod removido?). Copia {:08X} ignorada.", rec.baseID, cloneID);
                continue;
            }
            BuildClone(clone, base, rec);
        }
    }

    // ------------------------------------------------------------------
    // Gravacao junto do save (cosave)
    // ------------------------------------------------------------------
    constexpr std::uint32_t kUniqueID = 'OVG1';
    constexpr std::uint32_t kRecordClones = 'CLON';
    constexpr std::uint32_t kRecordVersion = 1;

    void SaveCallback(SKSE::SerializationInterface* intfc)
    {
        if (!intfc->OpenRecord(kRecordClones, kRecordVersion)) {
            Log("ERRO: nao foi possivel abrir o registro do cosave.");
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
        }
        Log("Cosave gravado: {} copias.", count);
    }

    void LoadCallback(SKSE::SerializationInterface* intfc)
    {
        g_clones.clear();
        g_built.clear();

        std::uint32_t type = 0, version = 0, length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecordClones || version != kRecordVersion) {
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

                RE::FormID newClone = 0, newBase = 0;
                if (!intfc->ResolveFormID(oldClone, newClone)) {
                    Log("  nao foi possivel resolver a copia {:08X}.", oldClone);
                    continue;
                }
                if (!intfc->ResolveFormID(oldBase, newBase)) {
                    Log("  nao foi possivel resolver a arma original {:08X}.", oldBase);
                    continue;
                }
                rec.baseID = newBase;
                g_clones[newClone] = rec;
            }
        }
        Log("Cosave lido: {} copias.", g_clones.size());
        RepopulateAll("carregamento do save");
    }

    void RevertCallback(SKSE::SerializationInterface*)
    {
        g_clones.clear();
        g_built.clear();
        Log("Revert: registro de copias limpo.");
    }

    // ------------------------------------------------------------------
    // Criacao de uma copia nova para um item forjado
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

    void ProcessNewItem(RE::FormID itemID)
    {
        Log("ProcessNewItem {:08X}", itemID);

        if (g_clones.contains(itemID)) {
            Log("  ja e uma copia nossa, ignorado.");
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::LookupByID<RE::TESObjectWEAP>(itemID);
        if (!player || !base || !IsEligible(base)) {
            Log("  ignorado (nao e uma arma elegivel).");
            return;
        }

        auto inv = player->GetInventory([base](RE::TESBoundObject& o) { return &o == base; });
        auto it = inv.find(base);
        if (it == inv.end() || it->second.first < 1) {
            Log("  arma nao encontrada no inventario.");
            return;
        }

        // 1) raridade
        float total = 0.0f;
        for (auto& r : g_cfg.rarities) {
            total += std::max(0.0f, r.chance);
        }
        std::uniform_real_distribution<float> pick(0.0f, total);
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
            Log("  raridade forcada pelo menu: {}", chosen->name);
        }

        // 2) multiplicadores
        CloneRecord rec;
        rec.baseID = base->GetFormID();
        rec.rarity = chosen->name;
        rec.damage = RollRange(chosen->damage);
        rec.value = RollRange(chosen->value);
        rec.weightDiv = chosen->weightDiv;
        rec.crit = 1.0f + RollRange(chosen->critBonus);
        rec.speed = RollRange(chosen->speed);
        Log("  sorteio: {} dano x{:.2f} valor x{:.2f} peso /{:.2f} crit x{:.2f} vel x{:.2f}",
            rec.rarity, rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);

        // 3) criar a copia
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectWEAP>();
        if (!factory) {
            Log("  FALHA: fabrica de armas indisponivel.");
            return;
        }
        auto* clone = factory->Create();
        if (!clone) {
            Log("  FALHA: nao foi possivel criar a copia.");
            return;
        }
        Log("  copia criada com FormID {:08X}", clone->GetFormID());

        if (!BuildClone(clone, base, rec)) {
            Log("  FALHA ao preencher a copia.");
            return;
        }
        g_clones[clone->GetFormID()] = rec;
        {
            std::lock_guard<std::mutex> lock(g_summaryMutex);
            g_lastSummary = std::format("[{}] {} | dano x{:.2f}  valor x{:.2f}  peso /{:.2f}  critico x{:.2f}  velocidade x{:.2f}",
                                        rec.rarity, base->GetName(), rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);
        }

        // 4) trocar no inventario
        player->RemoveItem(base, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        Log("  arma original removida");
        player->AddObjectToContainer(clone, nullptr, 1, nullptr);
        Log("  copia adicionada ao inventario");

        if (g_cfg.notify) {
            const std::string msg = std::format("{}: {}", rec.rarity, base->GetName());
            RE::DebugNotification(msg.c_str());
        }
        Log("  concluido");
    }

    // ------------------------------------------------------------------
    // Eventos do jogo
    // ------------------------------------------------------------------
    bool g_craftingMenuOpen = false;
    bool g_busy = false;
    std::chrono::steady_clock::time_point g_cooldownUntil{};

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

            // so nos interessam armas
            auto* form = RE::TESForm::LookupByID(base);
            if (!form || !form->As<RE::TESObjectWEAP>()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            Log("Evento: arma {:08X} x{} entrou no inventario durante a forja", base, count);

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
    // Menu no SKSE Menu Framework
    // ------------------------------------------------------------------
    std::string g_menuStatus;

    // dois sliders (minimo e maximo) que nunca se cruzam
    void RangeSliders(const char* name, Range& r, float lo, float hi)
    {
        ImGuiMCP::PushID(name);
        ImGuiMCP::Text("%s", name);
        const bool a = ImGuiMCP::SliderFloat("minimo", &r.min, lo, hi, "%.2f", 0);
        const bool b = ImGuiMCP::SliderFloat("maximo", &r.max, lo, hi, "%.2f", 0);
        if (a && r.min > r.max) {
            r.max = r.min;
        }
        if (b && r.max < r.min) {
            r.min = r.max;
        }
        ImGuiMCP::PopID();
    }

    void __stdcall RenderMenu()
    {
        ImGuiMCP::Text("Overgeared - raridade de armas forjadas");
        ImGuiMCP::TextWrapped("As mudancas valem para as PROXIMAS armas forjadas. Armas que ja existem nao mudam.");
        ImGuiMCP::Separator();

        ImGuiMCP::Checkbox("So afeta itens forjados", &g_cfg.onlyWhenCrafting);
        ImGuiMCP::Checkbox("Mostrar notificacao ao forjar", &g_cfg.notify);

        ImGuiMCP::Spacing();
        ImGuiMCP::Text("Forcar raridade (para testes):");
        ImGuiMCP::Combo("##forcar", &g_forceRarity, "Aleatoria\0Comum\0Rara\0Epica\0Lendaria\0Mitica\0");

        ImGuiMCP::Spacing();
        ImGuiMCP::Text("Ultima arma forjada:");
        {
            std::string copy;
            {
                std::lock_guard<std::mutex> lock(g_summaryMutex);
                copy = g_lastSummary;
            }
            ImGuiMCP::TextWrapped("%s", copy.c_str());
        }

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button("Salvar no .ini")) {
            g_menuStatus = SaveConfig() ? "Salvo em Data/SKSE/Plugins/Overgeared.ini" : "ERRO ao salvar (veja o log)";
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Recarregar do .ini")) {
            LoadConfig();
            g_menuStatus = "Recarregado do .ini";
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Valores padrao")) {
            g_cfg = Config{};
            g_menuStatus = "Valores padrao restaurados (ainda nao salvos)";
        }
        if (!g_menuStatus.empty()) {
            ImGuiMCP::TextWrapped("%s", g_menuStatus.c_str());
        }
        ImGuiMCP::Separator();

        for (int i = 0; i < 5; ++i) {
            auto& r = g_cfg.rarities[i];
            ImGuiMCP::PushID(i);
            if (ImGuiMCP::CollapsingHeader(r.name.c_str())) {
                ImGuiMCP::SliderFloat("Chance (peso relativo)", &r.chance, 0.0f, 100.0f, "%.1f", 0);
                RangeSliders("Dano (multiplicador)", r.damage, 0.5f, 8.0f);
                RangeSliders("Valor de venda (multiplicador)", r.value, 0.5f, 8.0f);
                ImGuiMCP::SliderFloat("Peso: divisor fixo", &r.weightDiv, 0.5f, 6.0f, "%.2f", 0);
                RangeSliders("Velocidade de ataque (multiplicador)", r.speed, 0.1f, 5.0f);
                RangeSliders("Dano critico: bonus somado", r.critBonus, 0.0f, 6.0f);
            }
            ImGuiMCP::PopID();
        }
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            Log("SKSE Menu Framework nao encontrado: o menu do Overgeared fica desativado.");
            return;
        }
        SKSEMenuFramework::SetSection("Overgeared");
        SKSEMenuFramework::AddSectionItem("Raridades", RenderMenu);
        Log("Menu registrado no SKSE Menu Framework.");
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
            Log("Eventos registrados.");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            RepopulateAll("apos carregar o jogo");
            RE::DebugNotification("Overgeared v3 (prototipo) ativo");
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
