/*
 * mod-rotation — cœur du module : configuration, interception du sort
 * déclencheur, helpers communs et aiguillage classe/spécialisation.
 */

#include "Rotation.h"

#include <algorithm>
#include <vector>

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "CreatureAI.h"
#include "Duration.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Language.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Pet.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

namespace ModRotation
{
Settings config;

// ---------------------------------------------------------------------------
// ACM assist bridge v2 (WoWbot 2026-10-30, ACM port).
// The server tells the player's own client which spell the rotation will cast
// NEXT, so the Action Combat Mode addon can show it beside the crosshair and
// highlight it on the action bars (retail Assisted Highlight behaviour).
// Wire: CHAT_MSG_ADDON, LANG_ADDON, built with ChatHandler::BuildChatPacket
// (correct 3.3.5 layout), text "CMROT\t<spellId>,<tree>,<class>,1,<role>"
// where role is H/T/D. spellId 0 clears the suggestion.
// Per-press state is thread_local (map updates run on several threads);
// per-player state lives in Player::CustomData.
static thread_local bool   t_dryRun    = false;
static thread_local uint32 t_predicted = 0;
// Collect mode (47322 tooltip): every known spell the priority list evaluates, in order.
static thread_local std::vector<uint32>* t_collect = nullptr;

bool IsDryRun() { return t_dryRun; }

struct RotationAssistData : public DataMap::Base
{
    bool   subscribed = false;
    uint32 sinceTick  = 0;
    uint32 sinceSend  = 0;
    uint32 lastSpell  = 0;
    uint8  listTree   = 255;   // tree whose priority list was last sent (255 = never)
    uint32 sinceSurvey = 0;
};

static char const* const ASSIST_KEY = "mod-rotation-assist";

static RotationAssistData* AssistData(Player* p)
{
    return p->CustomData.GetDefault<RotationAssistData>(ASSIST_KEY);
}

// Dominant CLASS talent tree (0..2). FamWoW's 4th tree is a separate system,
// so it never decides the rotation spec.
uint8 ClassicTree(Player* p)
{
    uint8 t = p->GetMostPointsTalentTree();
    if (t < 3)
        return t;

    uint32 pts[3] = { 0, 0, 0 };
    for (auto const& kv : p->GetTalentMap())
    {
        if (kv.second->State == PLAYERSPELL_REMOVED || !kv.second->IsInSpec(p->GetActiveSpec()))
            continue;
        TalentEntry const* ti = sTalentStore.LookupEntry(kv.second->talentID);
        if (!ti)
            continue;
        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(ti->TalentTab);
        if (!tab || tab->tabpage >= 3)
            continue;
        for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
            if (ti->RankID[r] && kv.first == ti->RankID[r])
            {
                pts[tab->tabpage] += r + 1;
                break;
            }
    }
    uint8 best = 0;
    for (uint8 i = 1; i < 3; ++i)
        if (pts[i] > pts[best])
            best = i;
    return best;
}

static char RoleFor(Player* p, uint8 tree)
{
    switch (p->getClass())
    {
        case CLASS_WARRIOR:      return tree == 2 ? 'T' : 'D';
        case CLASS_PALADIN:      return tree == 0 ? 'H' : (tree == 1 ? 'T' : 'D');
        case CLASS_DEATH_KNIGHT: return tree == 0 ? 'T' : 'D';
        case CLASS_PRIEST:       return tree == 2 ? 'D' : 'H';
        case CLASS_SHAMAN:       return tree == 2 ? 'H' : 'D';
        case CLASS_DRUID:
            if (tree == 2)
                return 'H';
            if (tree == 1 && (p->GetShapeshiftForm() == FORM_BEAR || p->GetShapeshiftForm() == FORM_DIREBEAR))
                return 'T';
            return 'D';
        default:                 return 'D';
    }
}

static void SendAssist(Player* p, uint32 spellId, uint8 tree)
{
    if (!config.AssistBroadcast || !p || !p->GetSession())
        return;

    std::string msg = "CMROT\t" + std::to_string(spellId) + "," +
                      std::to_string(uint32(tree)) + "," +
                      std::to_string(uint32(p->getClass())) + ",1," + RoleFor(p, tree);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_ADDON, LANG_ADDON, p, p, msg);
    p->GetSession()->SendPacket(&data);
}


uint32 MaxRank(Player* p, uint32 firstRankId)
{
    uint32 best = 0;
    for (SpellInfo const* info = sSpellMgr->GetSpellInfo(firstRankId); info; info = info->GetNextRankSpell())
        if (p->HasSpell(info->Id))
            best = info->Id;

    return best;
}

static bool CanPayCost(Player* p, SpellInfo const* info)
{
    if (info->PowerType != POWER_MANA && info->PowerType != POWER_RAGE && info->PowerType != POWER_ENERGY)
        return true; // runes, etc. : vérifié par le cœur au moment du cast

    int32 cost = info->CalcPowerCost(p, info->GetSchoolMask());
    return int32(p->GetPower(Powers(info->PowerType))) >= cost;
}

static bool ReadyToCast(Player* p, SpellInfo const* info)
{
    if (!info)
        return false;

    if (p->HasSpellCooldown(info->Id))
        return false;

    if (p->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return false;

    // Posture/forme requise non satisfaite (ex. Pourfendre en Posture
    // berserker) : on ignore silencieusement au lieu de spammer l'erreur.
    if (info->CheckShapeshift(p->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    return CanPayCost(p, info);
}

// Dry run: would this spell be cast now (ignoring the GCD, so the suggestion
// is ready while the GCD spins)? Records the first hit and stops the chain.
static bool Predict(Player* p, Unit* target, SpellInfo const* info)
{
    if (t_collect)
    {
        if (info && p->HasSpell(info->Id) && t_collect->size() < 24 &&
            std::find(t_collect->begin(), t_collect->end(), info->Id) == t_collect->end())
            t_collect->push_back(info->Id);
        return false;   // keep walking the whole list
    }
    if (!info || !target)
        return false;
    if (p->HasSpellCooldown(info->Id))
        return false;
    if (info->CheckShapeshift(p->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;
    if (!CanPayCost(p, info))
        return false;
    if (target != p)
    {
        float maxR = info->GetMaxRange(!p->IsValidAttackTarget(target), p);
        if (maxR > 0.0f && !p->IsWithinCombatRange(target, maxR + 1.0f))
            return false;
    }
    if (!t_predicted)
        t_predicted = info->Id;
    return true;
}

bool TryCast(Player* p, Unit* target, uint32 spellId)
{
    if (!spellId || !target)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (t_dryRun)
        return Predict(p, target, info);

    if (!ReadyToCast(p, info))
        return false;

    return p->CastSpell(target, spellId, false) == SPELL_CAST_OK;
}

bool TryCastRank(Player* p, Unit* target, uint32 firstRankId)
{
    return TryCast(p, target, MaxRank(p, firstRankId));
}

bool TryCastAt(Player* p, Unit* where, uint32 firstRankId)
{
    uint32 id = MaxRank(p, firstRankId);
    if (!id || !where)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    if (t_dryRun)
        return Predict(p, where, info);

    if (!ReadyToCast(p, info))
        return false;

    return p->CastSpell(where->GetPositionX(), where->GetPositionY(), where->GetPositionZ(), id, false) == SPELL_CAST_OK;
}

bool KeepAuraOn(Player* p, Unit* target, uint32 firstRankId)
{
    if (!target || target->GetAuraOfRankedSpell(firstRankId, p->GetGUID()))
        return false;

    return TryCastRank(p, target, firstRankId);
}

bool KeepSelfBuff(Player* p, uint32 firstRankId)
{
    if (p->GetAuraOfRankedSpell(firstRankId))
        return false;

    return TryCastRank(p, p, firstRankId);
}

bool IsBleedImmune(Unit* target)
{
    uint32 type = target->GetCreatureType();
    return type == CREATURE_TYPE_MECHANICAL || type == CREATURE_TYPE_ELEMENTAL;
}

void PetAttack(Ctx& c)
{
    if (!c.target || t_dryRun)
        return;

    if (Pet* pet = c.me->GetPet())
        if (pet->IsAlive() && !pet->GetVictim() && pet->AI())
            pet->AI()->AttackStart(c.target);
}

void EngageMelee(Ctx& c)
{
    if (t_dryRun)
        return;

    if (c.target && c.me->GetVictim() != c.target)
        c.me->Attack(c.target, true);

    PetAttack(c);
}

void EngageRanged(Ctx& c)
{
    if (t_dryRun)
        return;

    if (!c.target)
        return;

    PetAttack(c);

    // Tir automatique (75) pour les chasseurs
    if (c.me->getClass() == CLASS_HUNTER && !c.me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        c.me->CastSpell(c.target, 75, false);
}

// Compte les ennemis en combat autour de la cible. C'est ce qui décide de
// l'AoE pour toutes les spécialisations.
//
// Se baser sur getAttackers() (« qui me frappe ») serait faux : un distant
// n'a personne à sa portée de mêlée, et en groupe seul le tank est attaqué —
// l'AoE ne se déclenchait donc jamais pour les autres. Voir l'issue #2.
static uint32 CountEnemiesAround(Player* p, Unit* center, float radius)
{
    typedef Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> Searcher;

    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, p, radius);
    Searcher searcher(center, units, check);

    // Le conteneur de grille d'AzerothCore (AllMapGridStoredObjectTypes)
    // contient Player en plus de Creature : les joueurs hostiles sont donc
    // comptés eux aussi, sans parcours supplémentaire.
    Cell::VisitObjects(center, searcher, radius);

    uint32 count = 0;
    for (Unit* u : units)
    {
        // Hors combat : on ignore, pour ne pas déclencher une AoE au sol qui
        // pullerait un groupe voisin resté paisible.
        if (u != center && !u->IsInCombat())
            continue;

        if (p->IsValidAttackTarget(u))
            ++count;
    }

    return count;
}

static void BuildCtx(Player* p, Ctx& c)
{
    c.me = p;

    Unit* sel = p->GetSelectedUnit();
    if (sel && sel->IsAlive())
    {
        if (p->IsValidAttackTarget(sel))
            c.target = sel;
        else if (sel != p && p->IsValidAssistTarget(sel))
            c.friendly = sel;
    }

    if (!c.friendly)
        c.friendly = p;

    if (c.target)
    {
        c.inMelee = p->IsWithinMeleeRange(c.target);
        c.enemies = CountEnemiesAround(p, c.target, config.AoeRadius);
    }

    c.tree = ClassicTree(p);
}

static void Dispatch(Ctx& c)
{
    switch (c.me->getClass())
    {
        case CLASS_WARRIOR:      Warrior(c);     break;
        case CLASS_PALADIN:      Paladin(c);     break;
        case CLASS_HUNTER:       Hunter(c);      break;
        case CLASS_ROGUE:        Rogue(c);       break;
        case CLASS_PRIEST:       Priest(c);      break;
        case CLASS_DEATH_KNIGHT: DeathKnight(c); break;
        case CLASS_SHAMAN:       Shaman(c);      break;
        case CLASS_MAGE:         Mage(c);        break;
        case CLASS_WARLOCK:      Warlock(c);     break;
        case CLASS_DRUID:        Druid(c);       break;
        default: break;
    }
}

// Runs the same priority list without casting and returns the spell it
// would cast next (0 = nothing usable).
static uint32 PredictNext(Player* p, uint8& tree)
{
    Ctx c;
    BuildCtx(p, c);
    tree = c.tree;
    t_dryRun = true;
    t_predicted = 0;
    Dispatch(c);
    t_dryRun = false;
    return t_predicted;
}

// "CMROTL\t<tree>,<class>,<id>,<id>..." : the spec's priority list for the 47322 tooltip.
static void SendList(Player* p, RotationAssistData* d)
{
    if (!config.AssistBroadcast || !p->GetSession())
        return;
    Ctx c;
    BuildCtx(p, c);
    if (!c.target)
        c.target = p->GetSelectedUnit() ? p->GetSelectedUnit() : p;
    std::vector<uint32> list;
    t_collect = &list;
    t_dryRun = true;
    Dispatch(c);
    t_dryRun = false;
    t_collect = nullptr;

    std::string msg = "CMROTL\t" + std::to_string(uint32(c.tree)) + "," + std::to_string(uint32(p->getClass()));
    for (uint32 id : list)
    {
        std::string part = "," + std::to_string(id);
        if (msg.size() + part.size() > 250)
            break;
        msg += part;
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_ADDON, LANG_ADDON, p, p, msg);
    p->GetSession()->SendPacket(&data);
    d->listTree = c.tree;
}

// Test aid (Rotation.AssistSurveyMs > 0, t4 only): logs what every player or bot in
// combat would be suggested, so all 30 specs can be checked from the server log.
static void Survey(Player* p, RotationAssistData* d, uint32 diff)
{
    if (!config.AssistSurveyMs || !p->IsAlive() || !p->IsInCombat())
        return;
    d->sinceSurvey += diff;
    if (d->sinceSurvey < config.AssistSurveyMs)
        return;
    d->sinceSurvey = 0;
    uint8 tree = 0;
    uint32 next = PredictNext(p, tree);
    SpellInfo const* info = next ? sSpellMgr->GetSpellInfo(next) : nullptr;
    Unit* sel = p->GetSelectedUnit();
    LOG_INFO("module", "ROTSURVEY class={} tree={} role={} lvl={} acct={} spell={} name={} target={}",
        uint32(p->getClass()), uint32(tree), RoleFor(p, tree), uint32(p->GetLevel()), p->GetSession() ? p->GetSession()->GetAccountId() : 0,
        next, info ? info->SpellName[0] : "-", sel ? sel->GetName() : "-");
}

static void SendPrediction(Player* p, RotationAssistData* d, bool force)
{
    uint8 tree = 0;
    uint32 next = PredictNext(p, tree);
    if (force || next != d->lastSpell || d->sinceSend >= 2000)
    {
        SendAssist(p, next, tree);
        d->lastSpell = next;
        d->sinceSend = 0;
    }
}

static void ExecuteRotation(Player* p)
{
    if (!p->IsAlive())
        return;

    // Ne pas couper une incantation ou une canalisation en cours
    // (le Tir automatique n'est pas bloquant).
    if (!p->IsNonMeleeSpellCast(false, false, true))
    {
        Ctx c;
        BuildCtx(p, c);
        Dispatch(c);
    }

    if (config.AssistBroadcast && config.AssistPredict)
    {
        RotationAssistData* d = AssistData(p);
        d->subscribed = true;
        SendPrediction(p, d, true);
    }
}

void AssistTick(Player* player, uint32 diff)
{
    if (config.AssistSurveyMs)
        Survey(player, AssistData(player), diff);

    RotationAssistData* d = player->CustomData.Get<RotationAssistData>(ASSIST_KEY);
    if (!d || !d->subscribed)
        return;

    d->sinceTick += diff;
    d->sinceSend += diff;
    if (d->sinceTick < config.AssistPredictIntervalMs)
        return;
    d->sinceTick = 0;

    if (d->listTree != ClassicTree(player))
        SendList(player, d);

    Unit* sel = player->GetSelectedUnit();
    bool hostile = sel && sel->IsAlive() && player->IsValidAttackTarget(sel);
    if (!player->IsAlive() || (!hostile && !player->IsInCombat()))
    {
        if (d->lastSpell)
        {
            SendAssist(player, 0, ClassicTree(player));
            d->lastSpell = 0;
            d->sinceSend = 0;
        }
        return;
    }

    SendPrediction(player, d, false);
}

void AssistSubscribe(Player* player)
{
    RotationAssistData* d = AssistData(player);
    d->subscribed = true;
    d->listTree = 255;   // resend the priority list on the next tick
}
} // namespace ModRotation

using namespace ModRotation;

class rotation_world_script : public WorldScript
{
public:
    rotation_world_script() : WorldScript("rotation_world_script") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.Enabled             = sConfigMgr->GetOption<bool>("Rotation.Enable", true);
        config.SpellId             = sConfigMgr->GetOption<uint32>("Rotation.SpellId", 47322);
        config.AutoLearn           = sConfigMgr->GetOption<bool>("Rotation.AutoLearn", true);
        config.AssistBroadcast     = sConfigMgr->GetOption<bool>("Rotation.AssistBroadcast", true);
        config.AssistPredict       = sConfigMgr->GetOption<bool>("Rotation.AssistPredict", true);
        config.AssistPredictIntervalMs = sConfigMgr->GetOption<uint32>("Rotation.AssistPredict.IntervalMs", 400);
        config.AssistSurveyMs = sConfigMgr->GetOption<uint32>("Rotation.AssistSurveyMs", 0);
        config.Announce            = sConfigMgr->GetOption<bool>("Rotation.Announce", false);
        config.AnnounceMessage     = sConfigMgr->GetOption<std::string>("Rotation.Announce.Message",
            "One-button |cff4CFF00Rotation|r module active: place the Rotation spell from your spellbook on your action bar.");
        config.AoeThreshold        = sConfigMgr->GetOption<uint32>("Rotation.AoE.Threshold", 2);
        config.AoeRadius           = sConfigMgr->GetOption<float>("Rotation.AoE.Radius", 10.0f);
        config.RageDumpThreshold   = sConfigMgr->GetOption<uint32>("Rotation.RageDump.Threshold", 50);
        config.EnergyDumpThreshold = sConfigMgr->GetOption<uint32>("Rotation.EnergyDump.Threshold", 60);
        config.HealInjuredPct      = sConfigMgr->GetOption<uint32>("Rotation.Heal.InjuredPct", 85);
        config.HealLowPct          = sConfigMgr->GetOption<uint32>("Rotation.Heal.LowPct", 60);
        config.HealUrgentPct       = sConfigMgr->GetOption<uint32>("Rotation.Heal.UrgentPct", 35);
    }
};

class rotation_player_script : public PlayerScript
{
public:
    rotation_player_script() : PlayerScript("rotation_player_script") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!config.Enabled)
            return;

        if (config.AutoLearn && !player->HasSpell(config.SpellId))
            player->learnSpell(config.SpellId);

        if (config.Announce && !config.AnnounceMessage.empty())
            ChatHandler(player->GetSession()).SendSysMessage(config.AnnounceMessage.c_str());
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!config.Enabled || !spell || spell->m_spellInfo->Id != config.SpellId)
            return;

        // Lancer un autre sort pendant la préparation du sort déclencheur n'est
        // pas sûr : on diffère la rotation d'un tick de mise à jour.
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            if (Player* p = ObjectAccessor::FindPlayer(guid))
                ExecuteRotation(p);
        }, Milliseconds(1));
    }

    // ACM assist: refresh the next-spell suggestion for subscribed players.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (config.Enabled && config.AssistBroadcast && config.AssistPredict)
            AssistTick(player, diff);
    }

    // ACM assist handshake: the addon whispers itself "CMROTSUB" on login.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 language, std::string& msg, Player* /*receiver*/) override
    {
        if (language == LANG_ADDON && msg.compare(0, 8, "CMROTSUB") == 0)
            AssistSubscribe(player);
        return true;
    }
};

void AddRotationScripts()
{
    new rotation_world_script();
    new rotation_player_script();
}
