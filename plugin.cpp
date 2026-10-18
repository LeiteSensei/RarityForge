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

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <unordered_set>

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

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        switch (msg->type) {
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
