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
// Nesta versao: armas E armaduras (joias ficam para depois).
// Armaduras: a defesa recebe um multiplicador por raridade, limitado por
// um bonus maximo em relacao a base e por um teto de defesa por tipo de peca.
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
#include <type_traits>
#include <vector>
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
        Range       armor;      // multiplicador de defesa (armaduras)
    };

    struct Config
    {
        std::array<RarityDef, 5> rarities{ {
            { "Comum", 60.0f, { 1.00f, 1.24f }, { 1.00f, 1.10f }, 1.00f, { 1.00f, 1.24f }, { 0.00f, 0.50f }, { 1.00f, 1.10f } },
            { "Rara", 25.0f, { 1.25f, 1.74f }, { 1.10f, 1.30f }, 1.25f, { 1.25f, 1.49f }, { 0.50f, 1.00f }, { 1.10f, 1.25f } },
            { "Epica", 10.0f, { 1.75f, 2.99f }, { 1.30f, 1.60f }, 1.75f, { 1.50f, 1.74f }, { 1.00f, 1.50f }, { 1.25f, 1.45f } },
            { "Lendaria", 4.0f, { 3.00f, 4.99f }, { 1.60f, 1.99f }, 2.25f, { 1.75f, 1.99f }, { 1.50f, 2.00f }, { 1.45f, 1.75f } },
            { "Mitica", 1.0f, { 5.00f, 5.00f }, { 2.00f, 2.00f }, 3.00f, { 2.00f, 2.00f }, { 2.00f, 2.00f }, { 2.00f, 2.00f } },
        } };
        bool onlyWhenCrafting = true;
        bool notify = true;
        bool affectWeapons = true;
        bool affectArmor = true;
        bool affectClothing = true;  // roupas (palavra-chave ArmorClothing)
        bool affectJewelry = true;   // joias e acessorios (aneis, amuletos, oculos etc.)

        // Janela de resultado depois de forjar
        bool resultWindow = true;
        int  resultMinRarity = 0;  // 0 = Comum ... 4 = Mitica: a janela aparece a partir desta raridade
        int  resultMaxItems = 2;   // quantos itens cabem na mesma caixa (o resto vira "+N anteriores")

        // Avaliador: tecla no inventario que sorteia a raridade de um item que voce ja tem
        bool appraiseEnabled = true;
        int  appraiseKey = 0x18;  // tecla O (codigo DirectInput)

        // Receita de refino (teste): permite melhorar o item na pedra de amolar / mesa de armaduras
        bool          refineEnabled = true;
        int           refineMinRarity = 2;  // 0-based: 2 = Epica
        int           refineMaxRarity = 2;
        std::uint32_t refineItemID = 0x0005ACE4;  // Lingote de Ferro (Skyrim.esm), so para teste
        int           refineItemCount = 1;

        // Armaduras: limites de defesa
        float armorMaxMult = 2.0f;  // bonus maximo em relacao a defesa base (0 = sem limite)
        // teto de defesa EXIBIDA por tipo de peca (0 = sem teto). Nunca reduz uma peca abaixo da base.
        float capBody = 80.0f;
        float capHead = 45.0f;
        float capHands = 45.0f;
        float capFeet = 45.0f;
        float capShield = 70.0f;
        float capOther = 45.0f;
    };

    Config g_cfg;

    bool        g_craftingMenuOpen = false;
    int         g_forceRarity = 0;  // 0 = aleatoria; 1..5 = forca a raridade (para testes)
    std::mutex  g_summaryMutex;
    std::string g_lastSummary = "(nenhum item forjado ainda)";

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
                } else if (key == "AppraiseEnabled") {
                    g_cfg.appraiseEnabled = ToBool(val);
                } else if (key == "AppraiseKey") {
                    g_cfg.appraiseKey = std::stoi(val);
                } else if (key == "RefineEnabled") {
                    g_cfg.refineEnabled = ToBool(val);
                } else if (key == "RefineMinRarity") {
                    g_cfg.refineMinRarity = std::clamp(std::stoi(val) - 1, 0, 4);
                } else if (key == "RefineMaxRarity") {
                    g_cfg.refineMaxRarity = std::clamp(std::stoi(val) - 1, 0, 4);
                } else if (key == "RefineItemID") {
                    g_cfg.refineItemID = static_cast<std::uint32_t>(std::stoul(val, nullptr, 0));
                } else if (key == "RefineItemCount") {
                    g_cfg.refineItemCount = std::max(1, std::stoi(val));
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
        f << std::format("OnlyWhenCrafting={}\nNotify={}\n", g_cfg.onlyWhenCrafting ? 1 : 0, g_cfg.notify ? 1 : 0);
        f << std::format("AffectWeapons={}\nAffectArmor={}\n", g_cfg.affectWeapons ? 1 : 0, g_cfg.affectArmor ? 1 : 0);
        f << std::format("AffectClothing={}\nAffectJewelry={}\n", g_cfg.affectClothing ? 1 : 0, g_cfg.affectJewelry ? 1 : 0);
        f << "; Janela de resultado: ResultMinRarity 1=Comum 2=Rara 3=Epica 4=Lendaria 5=Mitica\n";
        f << std::format("ResultWindow={}\nResultMinRarity={}\nResultMaxItems={}\n", g_cfg.resultWindow ? 1 : 0, g_cfg.resultMinRarity + 1, g_cfg.resultMaxItems);
        f << "; Avaliador: AppraiseKey = codigo de tecla DirectInput (24 = O, 25 = P, 0 = desligada)\n";
        f << std::format("AppraiseEnabled={}\nAppraiseKey={}\n", g_cfg.appraiseEnabled ? 1 : 0, g_cfg.appraiseKey);
        f << "; Refino (teste): faixa de raridades que ganham receita, e o item exigido (FormID em hexadecimal)\n";
        f << std::format("RefineEnabled={}\nRefineMinRarity={}\nRefineMaxRarity={}\nRefineItemID=0x{:08X}\nRefineItemCount={}\n",
                         g_cfg.refineEnabled ? 1 : 0, g_cfg.refineMinRarity + 1, g_cfg.refineMaxRarity + 1, g_cfg.refineItemID, g_cfg.refineItemCount);
        f << "; Armaduras: ArmorMaxMultiplier = bonus maximo de defesa vs a base (0 = sem limite)\n";
        f << "; Cap* = teto de defesa exibida por tipo de peca (0 = sem teto)\n";
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
        bool          isArmor{ false };
        float         armor{ 1.0f };    // multiplicador de defesa sorteado (armaduras)
        std::uint32_t armorFinal{ 0 };  // defesa final gravada (ja com os limites aplicados)
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

    // ------------------------------------------------------------------
    // Armaduras
    // ------------------------------------------------------------------
    constexpr float kRatingScale = 100.0f;  // o jogo guarda a defesa multiplicada por 100

    // Para cada classe candidata a "parte" da armadura: se a armadura realmente a
    // herda, copia o componente; se nao herda, ignora (sem erro de compilacao).
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

    // Onde termina (em bytes, a partir do inicio do objeto) a parte T da armadura.
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
        // caixa de limites do objeto
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

        // fim do ultimo componente = inicio dos campos proprios da armadura
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
        Log("  partes copiadas (armadura): {} componentes; campos proprios a partir de 0x{:X}, {} bytes (tamanho total 0x{:X})",
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

    // slots: 30 cabeca(bit0) 31 cabelo(1) 32 corpo(2) 33 maos(3) 34 antebracos(4)
    //        35 amuleto(5) 36 anel(6) 37 pes(7) 38 canelas(8) 39 escudo(9) 42 diadema(12)
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
        case ArmorCat::Body: return "peitoral";
        case ArmorCat::Head: return "cabeca";
        case ArmorCat::Hands: return "maos";
        case ArmorCat::Feet: return "pes";
        case ArmorCat::Shield: return "escudo";
        default: return "outro";
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

        // peca "de armadura de verdade": ocupa cabeca, corpo, maos, pes ou escudo
        const bool wornArmor = (m & ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 7) | (1u << 9))) != 0;
        const bool jewelry = !wornArmor && (HasKeywordId(kw, "ArmorJewelry") || (m & ((1u << 5) | (1u << 6))) != 0);
        const bool clothing = !wornArmor && !jewelry && HasKeywordId(kw, "ArmorClothing");

        if (jewelry && !g_cfg.affectJewelry) {
            Log("  armadura {:08X} ignorada: joia/acessorio desligado no menu (espacos 0x{:X})", a->GetFormID(), m);
            return false;
        }
        if (clothing && !g_cfg.affectClothing) {
            Log("  armadura {:08X} ignorada: roupa desligada no menu (espacos 0x{:X})", a->GetFormID(), m);
            return false;
        }
        // Pecas sem defesa tambem entram: nome, valor e peso recebem a raridade,
        // so a defesa continua zero.
        return true;
    }

    // defesa final: multiplicador sorteado, limitado pelo bonus maximo e pelo teto da peca.
    // O teto nunca faz a peca ficar abaixo da defesa que ela ja tinha.
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

        Log("  armadura {:08X} de {:08X}: nome='{}' tipo={} defesa {:.1f}->{:.1f} (exibida; bruto {}->{}) valor {}->{} peso {:.2f}->{:.2f} palavras-chave={}",
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
        Log("Refazendo copias ({}): {} registradas.", reason, g_clones.size());
        for (auto& [cloneID, rec] : g_clones) {
            if (g_built.contains(cloneID)) {
                continue;  // ja montada nesta sessao
            }
            if (rec.isArmor) {
                auto* aClone = RE::TESForm::LookupByID<RE::TESObjectARMO>(cloneID);
                auto* aBase = RE::TESForm::LookupByID<RE::TESObjectARMO>(rec.baseID);
                if (!aClone) {
                    Log("  armadura {:08X} nao existe mais no jogo (ignorada).", cloneID);
                    continue;
                }
                if (!aBase) {
                    Log("  armadura original {:08X} nao encontrada (mod removido?). Copia {:08X} ignorada.", rec.baseID, cloneID);
                    continue;
                }
                BuildArmorClone(aClone, aBase, rec);
                continue;
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
    constexpr std::uint32_t kRecordVersion = 2;

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
            const std::uint8_t isArmor = rec.isArmor ? 1 : 0;
            intfc->WriteRecordData(isArmor);
            intfc->WriteRecordData(rec.armor);
            intfc->WriteRecordData(rec.armorFinal);
        }
        Log("Cosave gravado: {} copias.", count);
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
                if (version >= 2) {
                    std::uint8_t isArmor = 0;
                    intfc->ReadRecordData(isArmor);
                    intfc->ReadRecordData(rec.armor);
                    intfc->ReadRecordData(rec.armorFinal);
                    rec.isArmor = isArmor != 0;
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

    // ------------------------------------------------------------------
    // Janela de resultado (caixa de mensagem do jogo)
    // ------------------------------------------------------------------
    std::mutex               g_pendingMutex;
    std::vector<std::string> g_pendingMessages;

    void QueueResultWindow(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingMessages.push_back(text);
    }

    // Deve rodar no fluxo principal do jogo.
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
            all += std::format("(+{} itens anteriores)\n\n", start);
        }
        for (std::size_t i = start; i < msgs.size(); ++i) {
            all += msgs[i];
            if (i + 1 < msgs.size()) {
                all += "\n\n";
            }
        }
        Log("Mostrando janela de resultado ({} itens).", msgs.size());
        RE::DebugMessageBox(all.c_str());
    }

    // ------------------------------------------------------------------
    // Receita de refino (TESTE): cria, para cada copia, uma receita de melhoria
    // na pedra de amolar (armas) ou na mesa de armaduras.
    // ------------------------------------------------------------------
    std::unordered_map<RE::FormID, RE::BGSConstructibleObject*> g_recipes;  // copia -> receita

    template <class C>
    void FillRecipe(C* cobj, RE::TESForm* created, RE::BGSKeyword* bench, RE::TESBoundObject* ingredient, int count)
    {
        auto** arr = static_cast<RE::ContainerObject**>(RE::malloc(sizeof(RE::ContainerObject*)));
        auto*  entry = static_cast<RE::ContainerObject*>(RE::malloc(sizeof(RE::ContainerObject)));
        std::memset(entry, 0, sizeof(RE::ContainerObject));
        entry->obj = ingredient;
        entry->count = count;
        arr[0] = entry;
        if constexpr (requires { cobj->requiredItems; }) {
            cobj->requiredItems.containerObjects = arr;
            cobj->requiredItems.numContainerObjects = 1;
        } else {
            cobj->containerObjects = arr;
            cobj->numContainerObjects = 1;
        }
        cobj->createdItem = created;
        cobj->benchKeyword = bench;
        if constexpr (requires { cobj->data.numConstructed; }) {
            cobj->data.numConstructed = 1;
        }
    }

    void EnsureRefineRecipe(RE::TESBoundObject* clone, bool isArmor, int rarityIndex)
    {
        if (!clone || !g_cfg.refineEnabled) {
            return;
        }
        if (rarityIndex < g_cfg.refineMinRarity || rarityIndex > g_cfg.refineMaxRarity) {
            return;
        }
        if (g_recipes.contains(clone->GetFormID())) {
            return;
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* bench = RE::TESForm::LookupByID<RE::BGSKeyword>(isArmor ? 0x000ADB78u : 0x00088108u);
        auto* ingredient = RE::TESForm::LookupByID<RE::TESBoundObject>(g_cfg.refineItemID);
        if (!dh || !bench || !ingredient) {
            Log("  receita de refino NAO criada para {:08X}: dh={} bancada={} ingrediente={}",
                clone->GetFormID(), dh != nullptr, bench != nullptr, ingredient != nullptr);
            return;
        }
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSConstructibleObject>();
        auto* cobj = factory ? factory->Create() : nullptr;
        if (!cobj) {
            Log("  receita de refino NAO criada: fabrica de receitas indisponivel.");
            return;
        }

        FillRecipe(cobj, clone, bench, ingredient, g_cfg.refineItemCount);
        dh->GetFormArray<RE::BGSConstructibleObject>().push_back(cobj);
        g_recipes[clone->GetFormID()] = cobj;
        const char* benchId = bench->GetFormEditorID();
        Log("  receita de refino criada {:08X} para {:08X} (bancada {}, {}x {:08X})",
            cobj->GetFormID(), clone->GetFormID(), benchId ? benchId : "?", g_cfg.refineItemCount, g_cfg.refineItemID);
    }

    int RarityIndexByName(const std::string& name)
    {
        for (int i = 0; i < 5; ++i) {
            if (g_cfg.rarities[i].name == name) {
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
            Log("  raridade forcada pelo menu: {}", chosen->name);
        }
        return chosen;
    }

    bool ProcessNewItem(RE::FormID itemID)
    {
        Log("ProcessNewItem {:08X}", itemID);

        if (g_clones.contains(itemID)) {
            Log("  ja e uma copia nossa, ignorado.");
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::LookupByID(itemID);
        auto* weap = form ? form->As<RE::TESObjectWEAP>() : nullptr;
        auto* armo = form ? form->As<RE::TESObjectARMO>() : nullptr;

        const bool okWeap = weap && g_cfg.affectWeapons && IsEligible(weap);
        const bool okArmo = armo && IsEligibleArmor(armo);
        if (!player || (!okWeap && !okArmo)) {
            Log("  ignorado (nao e uma arma ou armadura elegivel).");
            return false;
        }
        RE::TESBoundObject* baseObj = okWeap ? static_cast<RE::TESBoundObject*>(weap) : static_cast<RE::TESBoundObject*>(armo);

        auto inv = player->GetInventory([baseObj](RE::TESBoundObject& o) { return &o == baseObj; });
        auto it = inv.find(baseObj);
        if (it == inv.end() || it->second.first < 1) {
            Log("  item nao encontrado no inventario.");
            return false;
        }

        // 1) raridade e 2) multiplicadores
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
            Log("  sorteio (arma): {} dano x{:.2f} valor x{:.2f} peso /{:.2f} crit x{:.2f} vel x{:.2f}",
                rec.rarity, rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);
        } else {
            rec.armor = RollRange(chosen->armor);
            Log("  sorteio (armadura): {} defesa x{:.2f} valor x{:.2f} peso /{:.2f}",
                rec.rarity, rec.armor, rec.value, rec.weightDiv);
        }

        // 3) criar a copia
        RE::TESBoundObject* clone = nullptr;
        if (okWeap) {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectWEAP>();
            auto* c = factory ? factory->Create() : nullptr;
            if (!c) {
                Log("  FALHA: nao foi possivel criar a copia da arma.");
                return false;
            }
            Log("  copia criada com FormID {:08X}", c->GetFormID());
            if (!BuildClone(c, weap, rec)) {
                Log("  FALHA ao preencher a copia.");
                return false;
            }
            clone = c;
        } else {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectARMO>();
            auto* c = factory ? factory->Create() : nullptr;
            if (!c) {
                Log("  FALHA: nao foi possivel criar a copia da armadura.");
                return false;
            }
            Log("  copia criada com FormID {:08X}", c->GetFormID());
            if (!BuildArmorClone(c, armo, rec)) {
                Log("  FALHA ao preencher a copia.");
                return false;
            }
            clone = c;
        }
        g_clones[clone->GetFormID()] = rec;
        const int rarityIndex = static_cast<int>(chosen - &g_cfg.rarities[0]);
        EnsureRefineRecipe(clone, okArmo, rarityIndex);

        // texto da janela de resultado
        if (g_cfg.resultWindow && rarityIndex >= g_cfg.resultMinRarity) {
            std::string txt = std::format("[{}] {}\n", rec.rarity, baseObj->GetName());
            if (okWeap) {
                auto* cw = clone->As<RE::TESObjectWEAP>();
                txt += std::format("Dano: {} para {}\n", weap->attackDamage, cw->attackDamage);
                txt += std::format("Dano critico: {} para {}\n", weap->criticalData.damage, cw->criticalData.damage);
                txt += std::format("Velocidade: {:.2f} para {:.2f}\n", weap->weaponData.speed, cw->weaponData.speed);
                txt += std::format("Peso: {:.1f} para {:.1f}\n", weap->weight, cw->weight);
                txt += std::format("Valor: {} para {}", weap->value, cw->value);
            } else {
                auto*       ca = clone->As<RE::TESObjectARMO>();
                const bool  limited = ca->armorRating < static_cast<std::uint32_t>(std::lround(static_cast<float>(armo->armorRating) * rec.armor));
                txt += std::format("Defesa: {:.1f} para {:.1f}{}\n", static_cast<float>(armo->armorRating) / kRatingScale,
                                   static_cast<float>(ca->armorRating) / kRatingScale, limited ? " (limite)" : "");
                txt += std::format("Peso: {:.1f} para {:.1f}\n", armo->weight, ca->weight);
                txt += std::format("Valor: {} para {}", armo->value, ca->value);
            }
            QueueResultWindow(txt);
        }

        {
            std::lock_guard<std::mutex> lock(g_summaryMutex);
            if (okWeap) {
                g_lastSummary = std::format("[{}] {} | dano x{:.2f}  valor x{:.2f}  peso /{:.2f}  critico x{:.2f}  velocidade x{:.2f}",
                                            rec.rarity, baseObj->GetName(), rec.damage, rec.value, rec.weightDiv, rec.crit, rec.speed);
            } else {
                g_lastSummary = std::format("[{}] {} ({}) | defesa x{:.2f} -> {:.1f} (base {:.1f})  valor x{:.2f}  peso /{:.2f}",
                                            rec.rarity, baseObj->GetName(), CatName(Categorize(armo)), rec.armor,
                                            static_cast<float>(rec.armorFinal) / kRatingScale,
                                            static_cast<float>(armo->armorRating) / kRatingScale, rec.value, rec.weightDiv);
            }
        }

        // 4) trocar no inventario
        player->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        Log("  item original removido");
        player->AddObjectToContainer(clone, nullptr, 1, nullptr);
        Log("  copia adicionada ao inventario");

        if (g_cfg.notify) {
            const std::string msg = std::format("{}: {}", rec.rarity, baseObj->GetName());
            RE::DebugNotification(msg.c_str());
        }
        if (!g_craftingMenuOpen) {
            FlushResultWindow();
        }
        Log("  concluido");
        return true;
    }

    // ------------------------------------------------------------------
    // Eventos do jogo
    // ------------------------------------------------------------------
    bool g_busy = false;
    std::chrono::steady_clock::time_point g_cooldownUntil{};

    // ------------------------------------------------------------------
    // Avaliador: sorteia a raridade de um item que o jogador ja tem.
    // Fluxo: tecla no inventario -> o inventario e fechado -> o item e trocado -> janela de resultado.
    // (A lista do inventario nao se atualiza com o menu aberto, por isso a troca so acontece depois de fechar.)
    // ------------------------------------------------------------------
    RE::FormID                            g_pendingAppraisal = 0;
    std::chrono::steady_clock::time_point g_pendingSince{};

    void Toast(const char* msg)
    {
        Log("Aviso ao jogador: {}", msg);
        RE::DebugNotification(msg);
    }

    // quantas unidades "simples" (sem dados extras: encantamento do jogador, aprimoramento, equipado...)
    int PlainCount(RE::PlayerCharacter* player, RE::TESBoundObject* obj)
    {
        auto inv = player->GetInventory([obj](RE::TESBoundObject& o) { return &o == obj; });
        auto it = inv.find(obj);
        if (it == inv.end()) {
            return 0;
        }
        const int               total = it->second.first;
        int                     extra = 0;
        RE::InventoryEntryData* entry = it->second.second.get();
        if (entry && entry->extraLists) {
            for (auto* list : *entry->extraLists) {
                if (list) {
                    extra += std::max<int>(1, list->GetCount());
                }
            }
        }
        return total - extra;
    }

    // FormID do item selecionado no inventario (le a lista da interface; funciona com SkyUI)
    RE::FormID SelectedInventoryFormID()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
            return 0;
        }
        auto movie = ui->GetMovieView(RE::InventoryMenu::MENU_NAME);
        if (!movie) {
            Log("Avaliador: interface do inventario indisponivel.");
            return 0;
        }
        RE::GFxValue entry;
        if (!movie->GetVariable(&entry, "_root.Menu_mc.inventoryLists.itemList.selectedEntry") || !entry.IsObject()) {
            Log("Avaliador: nao encontrei o item selecionado na lista do inventario.");
            return 0;
        }
        RE::GFxValue fid;
        if (!entry.GetMember("formId", &fid) || !fid.IsNumber()) {
            Log("Avaliador: o item selecionado nao informa o formId.");
            return 0;
        }
        const auto n = static_cast<std::int64_t>(fid.GetNumber());
        return static_cast<RE::FormID>(static_cast<std::uint32_t>(n));
    }

    // 1) tecla apertada com o inventario aberto: valida e pede para fechar o inventario
    void RequestAppraisal()
    {
        Log("Avaliador: tecla pressionada no inventario.");
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || g_pendingAppraisal != 0) {
            return;
        }
        const RE::FormID id = SelectedInventoryFormID();
        if (id == 0) {
            Toast("Nao consegui ler o item selecionado.");
            return;
        }
        if (g_clones.contains(id)) {
            Toast("Este item ja tem raridade.");
            return;
        }
        auto* form = RE::TESForm::LookupByID(id);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        const bool isWeapon = obj && obj->As<RE::TESObjectWEAP>();
        const bool isArmor = obj && obj->As<RE::TESObjectARMO>();
        if (!isWeapon && !isArmor) {
            Toast("So armas e armaduras podem ser avaliadas.");
            return;
        }
        if (PlainCount(player, obj) < 1) {
            Toast("Desequipe o item. Itens equipados, encantados por voce ou aprimorados nao podem ser avaliados.");
            return;
        }

        g_pendingAppraisal = id;
        g_pendingSince = std::chrono::steady_clock::now();
        Log("Avaliador: item {:08X} aceito; fechando o inventario para trocar o item.", id);
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    // 2) o inventario fechou: troca o item e mostra a janela de resultado
    void RunAppraisal(RE::FormID id)
    {
        Log("Avaliador: inventario fechado, avaliando {:08X}.", id);
        g_busy = true;
        const bool ok = ProcessNewItem(id);
        g_busy = false;
        g_cooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (!ok) {
            Toast("Este item nao pode receber raridade (veja as opcoes no menu).");
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
                    Log("Avaliador: pedido expirado, ignorado.");
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

            // so nos interessam armas e armaduras
            auto*      form = RE::TESForm::LookupByID(base);
            const bool isWeapon = form && form->As<RE::TESObjectWEAP>() && g_cfg.affectWeapons;
            const bool isArmor = form && form->As<RE::TESObjectARMO>() && g_cfg.affectArmor;
            if (!isWeapon && !isArmor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            Log("Evento: {} {:08X} x{} entrou no inventario durante a forja", isArmor ? "armadura" : "arma", base, count);

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
        ImGuiMCP::Checkbox("Afetar armas", &g_cfg.affectWeapons);
        ImGuiMCP::Checkbox("Afetar armaduras", &g_cfg.affectArmor);
        ImGuiMCP::Checkbox("Afetar roupas", &g_cfg.affectClothing);
        ImGuiMCP::Checkbox("Afetar joias e acessorios", &g_cfg.affectJewelry);
        ImGuiMCP::Spacing();
        ImGuiMCP::Checkbox("Mostrar janela com os atributos ao criar o item", &g_cfg.resultWindow);
        ImGuiMCP::Text("Mostrar a janela a partir da raridade:");
        ImGuiMCP::Combo("##janelamin", &g_cfg.resultMinRarity, "Comum\0Rara\0Epica\0Lendaria\0Mitica\0");
        ImGuiMCP::SliderInt("Itens por janela", &g_cfg.resultMaxItems, 1, 5, "%d", 0);
        ImGuiMCP::TextWrapped("Os numeros da janela sao os valores BASE do item. No inventario o jogo soma o bonus de habilidades e perks.");

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

        if (ImGuiMCP::CollapsingHeader("Avaliador")) {
            ImGuiMCP::TextWrapped("Com o inventario aberto, selecione uma arma ou armadura que voce ja tem e aperte a tecla de avaliar. "
                                  "O inventario fecha, o item e trocado por uma versao com raridade, e a janela de resultado aparece.");
            ImGuiMCP::Checkbox("Ativar o avaliador", &g_cfg.appraiseEnabled);
            ImGuiMCP::TextWrapped("Tecla: codigo %d (AppraiseKey no .ini; 24 = O, 25 = P, 0 desliga). "
                                  "Digitar essa letra na busca do inventario tambem aciona.", g_cfg.appraiseKey);
            ImGuiMCP::TextWrapped("Nao avalia: itens que ja tem raridade, equipados, encantados por voce ou aprimorados.");
        }
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("Refino (teste)")) {
            ImGuiMCP::TextWrapped("Cria uma receita para melhorar o item na pedra de amolar (armas) ou na mesa de armaduras. "
                                  "Vale para itens criados DEPOIS de ligar e, ao carregar o jogo, para os existentes.");
            ImGuiMCP::Checkbox("Criar receita de refino", &g_cfg.refineEnabled);
            ImGuiMCP::Text("Da raridade:");
            ImGuiMCP::Combo("##refmin", &g_cfg.refineMinRarity, "Comum\0Rara\0Epica\0Lendaria\0Mitica\0");
            ImGuiMCP::Text("Ate a raridade:");
            ImGuiMCP::Combo("##refmax", &g_cfg.refineMaxRarity, "Comum\0Rara\0Epica\0Lendaria\0Mitica\0");
            ImGuiMCP::TextWrapped("Ingrediente exigido: %08X x%d (troque no .ini: RefineItemID e RefineItemCount)",
                                  g_cfg.refineItemID, g_cfg.refineItemCount);
        }
        ImGuiMCP::Separator();

        if (ImGuiMCP::CollapsingHeader("Armaduras: limites de defesa")) {
            ImGuiMCP::TextWrapped("Bonus maximo de defesa em relacao a base (0 = sem limite). Vale para todas as pecas.");
            ImGuiMCP::SliderFloat("Bonus maximo (x a base)", &g_cfg.armorMaxMult, 0.0f, 5.0f, "%.2f", 0);
            ImGuiMCP::Spacing();
            ImGuiMCP::TextWrapped("Teto de defesa por tipo de peca (valor exibido no inventario; 0 = sem teto). "
                                  "O teto nunca reduz uma peca abaixo da defesa que ela ja tinha.");
            ImGuiMCP::SliderFloat("Teto: peitoral", &g_cfg.capBody, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Teto: cabeca", &g_cfg.capHead, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Teto: maos", &g_cfg.capHands, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Teto: pes", &g_cfg.capFeet, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Teto: escudo", &g_cfg.capShield, 0.0f, 700.0f, "%.0f", 0);
            ImGuiMCP::SliderFloat("Teto: outras pecas", &g_cfg.capOther, 0.0f, 700.0f, "%.0f", 0);
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
                RangeSliders("Defesa de armadura (multiplicador)", r.armor, 0.5f, 5.0f);
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
            if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
                input->AddEventSink(InputSink::Get());
            }
            Log("Eventos registrados.");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            RepopulateAll("apos carregar o jogo");
            EnsureAllRecipes();
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
