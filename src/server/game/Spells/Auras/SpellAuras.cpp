/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CellImpl.h"
#include "Common.h"
#include "Config.h"
#include "DynamicObject.h"
#include "EnumFlag.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellPackets.h"
#include "SpellScript.h"
#include "Unit.h"
#include "Util.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"

AuraApplication::AuraApplication(Unit* target, Unit* caster, Aura* aura, uint8 effMask)
    : _target(target), _base(aura), _removeMode(AuraRemoveFlags::None), _slot(MAX_AURAS), _flags(AFLAG_NONE), _effectsToApply(effMask), _needClientUpdate(false)
{
    ASSERT(GetTarget() && GetBase());

    if (GetBase()->CanBeSentToClient())
    {
        // Try find slot for aura
        uint8 slot = MAX_AURAS;
        // Lookup for auras already applied from spell
        if (AuraApplication* foundAura = GetTarget()->GetAuraApplication(GetBase()->GetId(), GetBase()->GetCasterGUID(), GetBase()->GetCastItemGUID()))
        {
            // allow use single slot only by auras from same caster
            slot = foundAura->GetSlot();
        }
        else
        {
            Unit::VisibleAuraMap const* visibleAuras = GetTarget()->GetVisibleAuras();
            // lookup for free slots in units visibleAuras
            Unit::VisibleAuraMap::const_iterator itr = visibleAuras->find(0);
            for (uint32 freeSlot = 0; freeSlot < MAX_AURAS; ++itr, ++freeSlot)
            {
                if (itr == visibleAuras->end() || itr->first != freeSlot)
                {
                    slot = freeSlot;
                    break;
                }
            }
        }

        // Register Visible Aura
        if (slot < MAX_AURAS)
        {
            _slot = slot;
            GetTarget()->SetVisibleAura(slot, this);
            SetNeedClientUpdate();
            LOG_DEBUG("spells", "Aura: %u Effect: %d put to unit visible auras slot: %u", GetBase()->GetId(), GetEffectMask(), slot);
        }
        else
            LOG_DEBUG("spells", "Aura: %u Effect: %d could not find empty unit visible slot", GetBase()->GetId(), GetEffectMask());
    }

    _InitFlags(caster, effMask);
}

void AuraApplication::_Remove()
{
    uint8 slot = GetSlot();

    if (slot >= MAX_AURAS)
        return;

    if (AuraApplication* foundAura = _target->GetAuraApplication(GetBase()->GetId(), GetBase()->GetCasterGUID(), GetBase()->GetCastItemGUID()))
    {
        // Reuse visible aura slot by aura which is still applied - prevent storing dead pointers
        if (slot == foundAura->GetSlot())
        {
            if (GetTarget()->GetVisibleAura(slot) == this)
            {
                GetTarget()->SetVisibleAura(slot, foundAura);
                foundAura->SetNeedClientUpdate();
            }
            // set not valid slot for aura - prevent removing other visible aura
            slot = MAX_AURAS;
        }
    }

    // update for out of range group members
    if (slot < MAX_AURAS)
    {
        GetTarget()->RemoveVisibleAura(slot);
        ClientUpdate(true);
    }
}

void AuraApplication::_InitFlags(Unit* caster, uint8 effMask)
{
    // mark as selfcast if needed
    _flags |= (GetBase()->GetCasterGUID() == GetTarget()->GetGUID()) ? AFLAG_CASTER : AFLAG_NONE;

    // aura is cast by self or an enemy
    // one negative effect and we know aura is negative
    if (IsSelfcast() || !caster || !caster->IsFriendlyTo(GetTarget()))
    {
        bool negativeFound = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (((1 << i) & effMask) && !GetBase()->GetSpellInfo()->IsPositiveEffect(i))
            {
                negativeFound = true;
                break;
            }
        }
        _flags |= negativeFound ? AFLAG_NEGATIVE : AFLAG_POSITIVE;
    }
    // aura is cast by friend
    // one positive effect and we know aura is positive
    else
    {
        bool positiveFound = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (((1 << i) & effMask) && GetBase()->GetSpellInfo()->IsPositiveEffect(i))
            {
                positiveFound = true;
                break;
            }
        }
        _flags |= positiveFound ? AFLAG_POSITIVE : AFLAG_NEGATIVE;
    }

    if (GetBase()->GetSpellInfo()->HasAttribute(SPELL_ATTR8_AURA_SEND_AMOUNT) || GetBase()->HasEffectType(SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS))
        _flags |= AFLAG_ANY_EFFECT_AMOUNT_SENT;
}

void AuraApplication::_HandleEffect(uint8 effIndex, bool apply)
{
    AuraEffect* aurEff = GetBase()->GetEffect(effIndex);
    ASSERT(aurEff);
    ASSERT(HasEffect(effIndex) == (!apply));
    ASSERT((1 << effIndex) & _effectsToApply);
    LOG_DEBUG("spells", "AuraApplication::_HandleEffect: %u, apply: %u: amount: %u", aurEff->GetAuraType(), apply, aurEff->GetAmount());

    if (apply)
    {
        ASSERT(!(_flags & (1 << effIndex)));
        _flags |= 1 << effIndex;
        aurEff->HandleEffect(this, AURA_EFFECT_HANDLE_REAL, true);
    }
    else
    {
        ASSERT(_flags & (1 << effIndex));
        _flags &= ~(1 << effIndex);
        aurEff->HandleEffect(this, AURA_EFFECT_HANDLE_REAL, false);
    }
    SetNeedClientUpdate();
}

void AuraApplication::BuildUpdatePacket(WorldPackets::Spells::AuraInfo& auraInfo, bool remove) const
{
    auraInfo.Slot = GetSlot();

    WorldPackets::Spells::AuraDataInfo& auraData = auraInfo.AuraData;

    if (remove)
    {
        ASSERT(!_target->GetVisibleAura(_slot));
        return;
    }
    ASSERT(_target->GetVisibleAura(_slot));

    Aura const* aura = GetBase();
    auraData.SpellID = aura->GetId();
    auraData.Flags = GetFlags();
    if (aura->GetType() != DYNOBJ_AURA_TYPE && aura->GetMaxDuration() > 0 && !aura->GetSpellInfo()->HasAttribute(SPELL_ATTR5_HIDE_DURATION))
        auraData.Flags |= AFLAG_DURATION;

    auraData.CastLevel = uint16(aura->GetCasterLevel());

    // send stack amount for aura which could be stacked (never 0 - causes incorrect display) or charges
    // stack amount has priority over charges (checked on retail with spell 50262)
    auraData.Applications = aura->GetSpellInfo()->StackAmount ? aura->GetStackAmount() : aura->GetCharges();

    if (!(auraData.Flags & AFLAG_CASTER))
        auraData.CastUnit = aura->GetCasterGUID();

    if (auraData.Flags & AFLAG_DURATION)
    {
        auraData.Duration = aura->GetMaxDuration();
        auraData.Remaining = aura->GetDuration();
    }

    if (auraData.Flags & AFLAG_ANY_EFFECT_AMOUNT_SENT)
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* effect = aura->GetEffect(i))
                if (HasEffect(i)) // Not all of aura's effects have to be applied on every target
                    auraData.Points[i] = effect->GetAmount();
}

void AuraApplication::ClientUpdate(bool remove)
{
    _needClientUpdate = false;

    WorldPackets::Spells::AuraUpdate update;
    update.UnitGUID = GetTarget()->GetGUID();

    WorldPackets::Spells::AuraInfo auraInfo;
    BuildUpdatePacket(auraInfo, remove);
    update.Auras.push_back(auraInfo);

    _target->SendMessageToSet(update.Write(), true);
}

uint8 Aura::BuildEffectMaskForOwner(SpellInfo const* spellProto, uint8 availableEffectMask, WorldObject* owner)
{
    ASSERT(spellProto);
    ASSERT(owner);
    uint8 effMask = 0;
    switch (owner->GetTypeId())
    {
    case TYPEID_UNIT:
    case TYPEID_PLAYER:
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellProto->Effects[i].IsUnitOwnedAuraEffect())
                effMask |= 1 << i;
        }
        break;
    case TYPEID_DYNAMICOBJECT:
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellProto->Effects[i].Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
                effMask |= 1 << i;
        }
        break;
    default:
        ABORT();
        break;
    }

    return effMask & availableEffectMask;
}

Aura* Aura::TryRefreshStackOrCreate(SpellInfo const* spellproto, uint8 tryEffMask, WorldObject* owner, Unit* caster, int32* baseAmount /*= nullptr*/, Item* castItem /*= nullptr*/,
    ObjectGuid casterGUID /*= ObjectGuid::Empty*/, bool* refresh /*= nullptr*/)
{
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    ASSERT(tryEffMask <= MAX_EFFECT_MASK);
    if (refresh)
        *refresh = false;

    uint8 effMask = Aura::BuildEffectMaskForOwner(spellproto, tryEffMask, owner);
    if (!effMask)
        return nullptr;

    if (Aura* foundAura = owner->ToUnit()->_TryStackingOrRefreshingExistingAura(spellproto, effMask, caster, baseAmount, castItem, casterGUID))
    {
        // we've here aura, which script triggered removal after modding stack amount
        // check the state here, so we won't create new Aura object
        if (foundAura->IsRemoved())
            return nullptr;

        if (refresh)
            *refresh = true;

        return foundAura;
    }
    else
        return Create(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
}

Aura* Aura::TryCreate(
    SpellInfo const* spellproto, uint8 tryEffMask, WorldObject* owner, Unit* caster, int32* baseAmount /*= nullptr*/, Item* castItem /*= nullptr*/, ObjectGuid casterGUID /*= ObjectGuid::Empty*/)
{
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    ASSERT(tryEffMask <= MAX_EFFECT_MASK);
    uint8 effMask = Aura::BuildEffectMaskForOwner(spellproto, tryEffMask, owner);
    if (!effMask)
        return nullptr;

    return Create(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
}

Aura* Aura::Create(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32* baseAmount, Item* castItem, ObjectGuid casterGUID)
{
    ASSERT(effMask);
    ASSERT(spellproto);
    ASSERT(owner);
    ASSERT(caster || casterGUID);
    ASSERT(effMask <= MAX_EFFECT_MASK);
    // try to get caster of aura
    if (casterGUID)
    {
        if (owner->GetGUID() == casterGUID)
            caster = owner->ToUnit();
        else
            caster = ObjectAccessor::GetUnit(*owner, casterGUID);
    }
    else
        casterGUID = caster->GetGUID();

    // check if aura can be owned by owner
    if (owner->isType(TYPEMASK_UNIT))
        if (!owner->IsInWorld() || ((Unit*)owner)->IsDuringRemoveFromWorld())
            // owner not in world so don't allow to own not self cast single target auras
            if (casterGUID != owner->GetGUID() && (spellproto->IsSingleTarget() || spellproto->GetAuraTargetLimit()))
                return nullptr;

    Aura* aura = nullptr;
    switch (owner->GetTypeId())
    {
    case TYPEID_UNIT:
    case TYPEID_PLAYER:
        aura = new UnitAura(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
        break;
    case TYPEID_DYNAMICOBJECT:
        aura = new DynObjAura(spellproto, effMask, owner, caster, baseAmount, castItem, casterGUID);
        break;
    default:
        ABORT();
        return nullptr;
    }

    // aura can be removed in Unit::_AddAura call
    if (aura->IsRemoved())
        return nullptr;
    return aura;
}

Aura::Aura(SpellInfo const* spellproto, WorldObject* owner, Unit* caster, Item* castItem, ObjectGuid casterGUID)
    : m_spellInfo(spellproto), m_casterGuid(casterGUID ? casterGUID : caster->GetGUID()), m_castItemGuid(castItem ? castItem->GetGUID() : ObjectGuid::Empty), m_applyTime(GameTime::GetGameTime()),
      m_owner(owner), m_timeCla(0), m_updateTargetMapInterval(0), _casterInfo(), m_procCharges(0), m_stackAmount(1), m_isRemoved(false), m_isLimitedTarget(false), m_isUsingCharges(false),
      m_dropEvent(nullptr), m_procCooldown(std::chrono::steady_clock::time_point::min())
{
    if (m_spellInfo->ManaPerSecond)
        m_timeCla = 1 * IN_MILLISECONDS;

    m_maxDuration = CalcMaxDuration(caster);
    m_duration = m_maxDuration;
    m_rolledOverDuration = 0;

    m_procCharges = CalcMaxCharges(caster);
    m_isUsingCharges = m_procCharges != 0;
    memset(m_effects, 0, sizeof(m_effects));
    // m_casterLevel = cast item level/caster level, caster level should be saved to db, confirmed with sniffs
    _casterInfo.Level = m_spellInfo->SpellLevel;
    if (caster)
    {
        _casterInfo.Level = caster->getLevel();
        _casterInfo.ApplyResilience = caster->CanApplyResilience();
        SaveCasterInfo(caster);
    }
}

AuraScript* Aura::GetScriptByName(std::string const& scriptName) const
{
    for (auto itr = m_loadedScripts.begin(); itr != m_loadedScripts.end(); ++itr)
        if ((*itr)->_GetScriptName()->compare(scriptName) == 0)
            return *itr;
    return nullptr;
}

void Aura::_InitEffects(uint8 effMask, Unit* caster, int32* baseAmount)
{
    // shouldn't be in constructor - functions in AuraEffect::AuraEffect use polymorphism
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (effMask & (uint8(1) << i))
            m_effects[i] = new AuraEffect(this, i, baseAmount ? baseAmount + i : nullptr, caster);
        else
            m_effects[i] = nullptr;
    }
}

bool Aura::CanPeriodicTickCrit(Unit const* caster) const
{
    if (m_spellInfo->HasAttribute(SPELL_ATTR8_PERIODIC_CAN_CRIT))
        return true;

    if (m_spellInfo->HasAttribute(SPELL_ATTR2_CANT_CRIT))
        return false;

    return caster->HasAuraTypeWithAffectMask(SPELL_AURA_ABILITY_PERIODIC_CRIT, GetSpellInfo());
}

float Aura::CalcPeriodicCritChance(Unit const* caster) const
{
    Player* modOwner = caster->GetSpellModOwner();
    if (!modOwner || !CanPeriodicTickCrit(modOwner))
        return 0.f;

    float critChance = modOwner->SpellCritChanceDone(GetSpellInfo(), GetSpellInfo()->GetSchoolMask(), GetSpellInfo()->GetAttackType());
    return std::max(0.f, critChance);
}

void Aura::SaveCasterInfo(Unit* caster)
{
    _casterInfo.CritChance = CalcPeriodicCritChance(caster);

    if (GetType() == UNIT_AURA_TYPE)
    {
        /*
         * Get critical chance from last effect type (damage or healing)
         * this could potentialy be wrong if any spell has both damage and heal periodics
         * The only two spells in 3.3.5 with those conditions are 17484 and 50344
         * which shouldn't be allowed to crit, so we're fine
         */
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            switch (GetSpellInfo()->Effects[i].ApplyAuraName)
            {
            case SPELL_AURA_PERIODIC_HEAL:
                _casterInfo.BonusDonePct = caster->SpellHealingPctDone(GetUnitOwner(), GetSpellInfo());
                break;
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_LEECH:
                _casterInfo.BonusDonePct = caster->SpellDamagePctDone(GetUnitOwner(), GetSpellInfo(), DOT);
                break;
            default:
                break;
            }
        }
    }
}

Aura::~Aura()
{
    // unload scripts
    for (auto itr = m_loadedScripts.begin(); itr != m_loadedScripts.end(); ++itr)
    {
        (*itr)->_Unload();
        delete (*itr);
    }

    // free effects memory
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        delete m_effects[i];

    ASSERT(m_applications.empty());
    _DeleteRemovedApplications();
}

Unit* Aura::GetCaster() const
{
    if (GetOwner()->GetGUID() == GetCasterGUID())
        return GetUnitOwner();
    if (AuraApplication const* aurApp = GetApplicationOfTarget(GetCasterGUID()))
        return aurApp->GetTarget();

    return ObjectAccessor::GetUnit(*GetOwner(), GetCasterGUID());
}

AuraObjectType Aura::GetType() const
{
    return (m_owner->GetTypeId() == TYPEID_DYNAMICOBJECT) ? DYNOBJ_AURA_TYPE : UNIT_AURA_TYPE;
}

void Aura::_ApplyForTarget(Unit* target, Unit* caster, AuraApplication* auraApp)
{
    ASSERT(target);
    ASSERT(auraApp);
    // aura mustn't be already applied on target
    ASSERT(!IsAppliedOnTarget(target->GetGUID()) && "Aura::_ApplyForTarget: aura musn't be already applied on target");

    m_applications[target->GetGUID()] = auraApp;

    // set infinity cooldown state for spells
    if (caster && caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (m_spellInfo->IsCooldownStartedOnEvent())
        {
            Item* castItem = m_castItemGuid ? caster->ToPlayer()->GetItemByGuid(m_castItemGuid) : nullptr;
            caster->GetSpellHistory()->StartCooldown(m_spellInfo, castItem ? castItem->GetEntry() : 0, nullptr, true);
        }
    }
}

void Aura::_UnapplyForTarget(Unit* target, Unit* caster, AuraApplication* auraApp)
{
    ASSERT(target);
    ASSERT(auraApp->GetRemoveMode().HasAnyFlag());
    ASSERT(auraApp);

    ApplicationMap::iterator itr = m_applications.find(target->GetGUID());

    /// @todo Figure out why this happens
    if (itr == m_applications.end())
    {
        LOG_ERROR("spells", "Aura::_UnapplyForTarget, target:%u, caster:%u, spell:%u was not found in owners application map!", target->GetGUID().GetCounter(),
            caster ? caster->GetGUID().GetCounter() : 0, auraApp->GetBase()->GetSpellInfo()->Id);
        ABORT();
    }

    // aura has to be already applied
    ASSERT(itr->second == auraApp);
    m_applications.erase(itr);

    m_removedApplications.push_back(auraApp);

    // reset cooldown state for spells
    if (caster && GetSpellInfo()->IsCooldownStartedOnEvent())
        // note: item based cooldowns and cooldown spell mods with charges ignored (unknown existed cases)
        caster->GetSpellHistory()->SendCooldownEvent(GetSpellInfo());
}

// removes aura from all targets
// and marks aura as removed
void Aura::_Remove(AuraRemoveFlags removeMode)
{
    ASSERT(!m_isRemoved);
    ASSERT(!EnumFlag<AuraRemoveFlags>{removeMode}.HasFlag(AuraRemoveFlags::DontResetPeriodicTimer), "Aura must not be removed with AuraRemoveFlags::DontResetPeriodicTimer");

    m_isRemoved = true;
    ApplicationMap::iterator appItr = m_applications.begin();
    for (appItr = m_applications.begin(); appItr != m_applications.end();)
    {
        AuraApplication* aurApp = appItr->second;
        Unit* target = aurApp->GetTarget();
        target->_UnapplyAura(aurApp, removeMode);
        appItr = m_applications.begin();
    }

    if (m_dropEvent)
    {
        m_dropEvent->ScheduleAbort();
        m_dropEvent = nullptr;
    }
}

void Aura::UpdateTargetMap(Unit* caster, bool apply)
{
    if (IsRemoved())
        return;

    m_updateTargetMapInterval = UPDATE_TARGET_MAP_INTERVAL;

    // fill up to date target list
    //                 target, effMask
    std::unordered_map<Unit*, uint8> targets;
    FillTargetMap(targets, caster);

    std::deque<Unit*> targetsToRemove;

    // mark all auras as ready to remove
    for (ApplicationMap::iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
    {
        auto itr = targets.find(appIter->second->GetTarget());
        // not found in current area - remove the aura
        if (itr == targets.end())
            targetsToRemove.push_back(appIter->second->GetTarget());
        else
        {
            // needs readding - remove now, will be applied in next update cycle
            // (dbcs do not have auras which apply on same type of targets but have different radius, so this is not really needed)
            if (appIter->second->GetEffectMask() != itr->second || !CanBeAppliedOn(itr->first))
                targetsToRemove.push_back(appIter->second->GetTarget());
            // nothing todo - aura already applied
            // remove from auras to register list
            targets.erase(itr);
        }
    }

    // register auras for units
    for (auto itr = targets.begin(); itr != targets.end();)
    {
        // aura mustn't be already applied on target
        if (AuraApplication* aurApp = GetApplicationOfTarget(itr->first->GetGUID()))
        {
            // the core created 2 different units with same guid
            // this is a major failue, which i can't fix right now
            // let's remove one unit from aura list
            // this may cause area aura "bouncing" between 2 units after each update
            // but because we know the reason of a crash we can remove the assertion for now
            if (aurApp->GetTarget() != itr->first)
            {
                // remove from auras to register list
                itr = targets.erase(itr);
                continue;
            }
            else
            {
                // ok, we have one unit twice in target map (impossible, but...)
                ABORT();
            }
        }

        bool addUnit = true;
        // check target immunities
        for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
        {
            if (itr->first->IsImmunedToSpellEffect(GetSpellInfo(), effIndex, caster))
                itr->second &= ~(1 << effIndex);
        }
        if (!itr->second || itr->first->IsImmunedToSpell(GetSpellInfo(), caster) || !CanBeAppliedOn(itr->first))
            addUnit = false;

        if (addUnit && !itr->first->IsHighestExclusiveAura(this, true))
            addUnit = false;

        // Dynobj auras don't hit flying targets
        if (GetType() == DYNOBJ_AURA_TYPE && itr->first->IsInFlight())
            addUnit = false;

        // Do not apply aura if it cannot stack with existing auras
        if (addUnit)
        {
            // Allow to remove by stack when aura is going to be applied on owner
            if (itr->first != GetOwner())
            {
                // check if not stacking aura already on target
                // this one prevents unwanted usefull buff loss because of stacking and prevents overriding auras periodicaly by 2 near area aura owners
                for (Unit::AuraApplicationMap::iterator iter = itr->first->GetAppliedAuras().begin(); iter != itr->first->GetAppliedAuras().end(); ++iter)
                {
                    Aura const* aura = iter->second->GetBase();
                    if (!CanStackWith(aura))
                    {
                        addUnit = false;
                        break;
                    }
                }
            }
        }
        if (!addUnit)
            itr = targets.erase(itr);
        else
        {
            // owner has to be in world, or effect has to be applied to self
            if (!GetOwner()->IsSelfOrInSameMap(itr->first))
            {
                /// @todo There is a crash caused by shadowfiend load addon
                LOG_FATAL("spells", "Aura %u: Owner %s (map %u) is not in the same map as target %s (map %u).", GetSpellInfo()->Id, GetOwner()->GetName().c_str(),
                    GetOwner()->IsInWorld() ? GetOwner()->GetMap()->GetId() : uint32(-1), itr->first->GetName().c_str(), itr->first->IsInWorld() ? itr->first->GetMap()->GetId() : uint32(-1));
                ABORT();
            }
            itr->first->_CreateAuraApplication(this, itr->second);
            ++itr;
        }
    }

    // remove auras from units no longer needing them
    for (Unit* unit : targetsToRemove)
        if (AuraApplication* aurApp = GetApplicationOfTarget(unit->GetGUID()))
            unit->_UnapplyAura(aurApp, AuraRemoveFlags::ByDefault);

    if (!apply)
        return;

    // apply aura effects for units
    for (auto itr = targets.begin(); itr != targets.end(); ++itr)
    {
        if (AuraApplication* aurApp = GetApplicationOfTarget(itr->first->GetGUID()))
        {
            // owner has to be in world, or effect has to be applied to self
            ASSERT((!GetOwner()->IsInWorld() && GetOwner() == itr->first) || GetOwner()->IsInMap(itr->first));
            itr->first->_ApplyAura(aurApp, itr->second);
        }
    }
}

// targets have to be registered and not have effect applied yet to use this function
void Aura::_ApplyEffectForTargets(uint8 effIndex)
{
    // prepare list of aura targets
    UnitList targetList;
    for (ApplicationMap::iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
    {
        if ((appIter->second->GetEffectsToApply() & (1 << effIndex)) && !appIter->second->HasEffect(effIndex))
            targetList.push_back(appIter->second->GetTarget());
    }

    // apply effect to targets
    for (UnitList::iterator itr = targetList.begin(); itr != targetList.end(); ++itr)
    {
        if (GetApplicationOfTarget((*itr)->GetGUID()))
        {
            // owner has to be in world, or effect has to be applied to self
            ASSERT((!GetOwner()->IsInWorld() && GetOwner() == *itr) || GetOwner()->IsInMap(*itr));
            (*itr)->_ApplyAuraEffect(this, effIndex);
        }
    }
}
void Aura::UpdateOwner(uint32 diff, WorldObject* owner)
{
    ASSERT(owner == m_owner);

    Unit* caster = GetCaster();
    // Apply spellmods for channeled auras
    // used for example when triggered spell of spell:10 is modded
    Spell* modSpell = nullptr;
    Player* modOwner = nullptr;
    if (caster)
    {
        modOwner = caster->GetSpellModOwner();
        if (modOwner)
        {
            modSpell = modOwner->FindCurrentSpellBySpellId(GetId());
            if (modSpell)
                modOwner->SetSpellModTakingSpell(modSpell, true);
        }
    }

    Update(diff, caster);

    if (m_updateTargetMapInterval <= int32(diff))
        UpdateTargetMap(caster);
    else
        m_updateTargetMapInterval -= diff;

    // update aura effects
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (m_effects[i])
            m_effects[i]->Update(diff, caster);

    // remove spellmods after effects update
    if (modSpell)
        modOwner->SetSpellModTakingSpell(modSpell, false);

    _DeleteRemovedApplications();
}

void Aura::Update(uint32 diff, Unit* caster)
{
    if (m_duration > 0)
    {
        m_duration -= diff;
        if (m_duration < 0)
            m_duration = 0;

        // handle manaPerSecond/manaPerSecondPerLevel
        if (m_timeCla)
        {
            if (m_timeCla > int32(diff))
                m_timeCla -= diff;
            else if (caster)
            {
                if (int32 manaPerSecond = m_spellInfo->ManaPerSecond)
                {
                    m_timeCla += 1000 - diff;

                    Powers powertype = Powers(m_spellInfo->PowerType);
                    if (powertype == POWER_HEALTH)
                    {
                        if (int32(caster->GetHealth()) > manaPerSecond)
                            caster->ModifyHealth(-manaPerSecond);
                        else
                            Remove();
                    }
                    else if (int32(caster->GetPower(powertype)) >= manaPerSecond)
                        caster->ModifyPower(powertype, -manaPerSecond);
                    else
                        Remove();
                }
            }
        }
    }
}

int32 Aura::CalcMaxDuration(Unit* caster) const
{
    return m_spellInfo->CalcDuration(caster);
}

void Aura::SetDuration(int32 duration, bool withMods)
{
    if (withMods)
        if (Unit* caster = GetCaster())
            if (Player* modOwner = caster->GetSpellModOwner())
                modOwner->ApplySpellMod(GetId(), SPELLMOD_DURATION, duration);

    m_duration = duration;
    SetNeedClientUpdateForTargets();
}

void Aura::RefreshDuration(bool withMods)
{
    Unit* caster = GetCaster();
    if (withMods && caster)
    {
        int32 duration = m_spellInfo->GetMaxDuration();
        // Calculate duration of periodics affected by haste.
        if (caster->HasAuraTypeWithAffectMask(SPELL_AURA_PERIODIC_HASTE, m_spellInfo) || m_spellInfo->HasAttribute(SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC))
            duration = int32(duration * caster->GetFloatValue(UNIT_MOD_CAST_SPEED));

        SetMaxDuration(duration);
        SetDuration(duration);
    }
    else
        SetDuration(GetMaxDuration());

    if (m_spellInfo->ManaPerSecond)
        m_timeCla = 1 * IN_MILLISECONDS;
}

void Aura::RefreshTimers()
{
    m_maxDuration = CalcMaxDuration();

    bool resetPeriodicTimer = !m_spellInfo->IsRollingDurationOver();
    if (!resetPeriodicTimer)
    {
        int32 minAmplitude = m_maxDuration;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* eff = GetEffect(i))
                if (int32 ampl = eff->GetPeriodic())
                    minAmplitude = std::min(ampl, minAmplitude);

        // Roll over the time to next tick into the new duration
        m_rolledOverDuration = GetDuration() % minAmplitude;
        m_maxDuration += m_rolledOverDuration;
    }

    RefreshDuration();

    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (AuraEffect* aurEff = m_effects[i])
            aurEff->CalculatePeriodic(caster, resetPeriodicTimer, false);
}

void Aura::SetCharges(uint8 charges)
{
    if (m_procCharges == charges)
        return;

    m_procCharges = charges;
    m_isUsingCharges = m_procCharges != 0;
    SetNeedClientUpdateForTargets();
}

uint8 Aura::CalcMaxCharges(Unit* caster) const
{
    uint32 maxProcCharges = m_spellInfo->ProcCharges;
    if (SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId()))
        maxProcCharges = procEntry->Charges;

    if (caster)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_CHARGES, maxProcCharges);

    return uint8(maxProcCharges);
}

bool Aura::ModCharges(int32 num, AuraRemoveFlags removeMode)
{
    if (IsUsingCharges())
    {
        int32 charges = m_procCharges + num;
        int32 maxCharges = CalcMaxCharges();

        // limit charges (only on charges increase, charges may be changed manually)
        if ((num > 0) && (charges > int32(maxCharges)))
            charges = maxCharges;
        // we're out of charges, remove
        else if (charges <= 0)
        {
            Remove(removeMode);
            return true;
        }

        SetCharges(charges);
    }

    return false;
}

void Aura::ModChargesDelayed(int32 num, AuraRemoveFlags removeMode)
{
    m_dropEvent = nullptr;
    ModCharges(num, removeMode);
}

void Aura::DropChargeDelayed(uint32 delay, AuraRemoveFlags removeMode)
{
    // aura is already during delayed charge drop
    if (m_dropEvent)
        return;
    // only units have events
    Unit* owner = m_owner->ToUnit();
    if (!owner)
        return;

    m_dropEvent = new ChargeDropEvent(this, removeMode);
    owner->m_Events.AddEvent(m_dropEvent, owner->m_Events.CalculateTime(delay));
}

void Aura::SetStackAmount(uint8 stackAmount)
{
    m_stackAmount = stackAmount;
    Unit* caster = GetCaster();

    std::list<AuraApplication*> applications;
    GetApplicationList(applications);

    for (AuraApplication* aurApp : applications)
        if (!aurApp->GetRemoveMode().HasAnyFlag())
            HandleAuraSpecificMods(aurApp, caster, false, true);

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (AuraEffect* aurEff = GetEffect(i))
            aurEff->ChangeAmount(aurEff->CalculateAmount(caster), false, true);

    for (AuraApplication* aurApp : applications)
        if (!aurApp->GetRemoveMode().HasAnyFlag())
            HandleAuraSpecificMods(aurApp, caster, true, true);

    SetNeedClientUpdateForTargets();
}

bool Aura::ModStackAmount(int32 num, AuraRemoveFlags removeMode /*= AuraRemoveFlags::ByDefault*/)
{
    int32 stackAmount = m_stackAmount + num;

    // limit the stack amount (only on stack increase, stack amount may be changed manually)
    if ((num > 0) && (stackAmount > int32(m_spellInfo->StackAmount)))
    {
        // not stackable aura - set stack amount to 1
        if (!m_spellInfo->StackAmount)
            stackAmount = 1;
        else
            stackAmount = m_spellInfo->StackAmount;
    }
    // we're out of stacks, remove
    else if (stackAmount <= 0)
    {
        Remove(removeMode & ~AuraRemoveFlags::DontResetPeriodicTimer);
        return true;
    }

    EnumFlag<AuraRemoveFlags> removeFlags = removeMode;

    bool refresh = stackAmount >= GetStackAmount() && (m_spellInfo->StackAmount || !m_spellInfo->HasAttribute(SPELL_ATTR1_DONT_REFRESH_DURATION_ON_RECAST));

    // Update stack amount
    SetStackAmount(stackAmount);

    if (refresh && !removeFlags.HasFlag(AuraRemoveFlags::DontResetPeriodicTimer))
    {
        RefreshTimers();

        // reset charges
        SetCharges(CalcMaxCharges());
    }

    SetNeedClientUpdateForTargets();
    return false;
}

bool Aura::HasMoreThanOneEffectForType(AuraType auraType) const
{
    uint32 count = 0;
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i) && AuraType(GetSpellInfo()->Effects[i].ApplyAuraName) == auraType)
            ++count;

    return count > 1;
}

bool Aura::IsArea() const
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i) && GetSpellInfo()->Effects[i].IsAreaAuraEffect())
            return true;

    return false;
}

bool Aura::IsPassive() const
{
    return GetSpellInfo()->IsPassive();
}

bool Aura::IsDeathPersistent() const
{
    return GetSpellInfo()->IsDeathPersistent();
}

bool Aura::CanBeSaved() const
{
    if (IsPassive())
        return false;

    if (GetSpellInfo()->IsChanneled())
        return false;

    // Check if aura is single target, not only spell info
    if (GetCasterGUID() != GetOwner()->GetGUID())
    {
        // owner == caster for area auras, check for possible bad data in DB
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!GetSpellInfo()->Effects[i].IsEffect())
                continue;

            if (GetSpellInfo()->Effects[i].IsTargetingArea() || GetSpellInfo()->Effects[i].IsAreaAuraEffect())
                return false;
        }

        if (IsLimitedTarget() || GetSpellInfo()->IsSingleTarget() || GetSpellInfo()->GetAuraTargetLimit())
            return false;
    }

    // don't save liquid auras
    if (GetSpellInfo()->HasAttribute(SPELL_ATTR0_CU_LIQUID_AURA))
        return false;

    // Can't be saved - aura handler relies on calculated amount and changes it
    if (HasEffectType(SPELL_AURA_CONVERT_RUNE))
        return false;

    // No point in saving this, since the stable dialog can't be open on aura load anyway.
    if (HasEffectType(SPELL_AURA_OPEN_STABLE))
        return false;

    // Can't save vehicle auras, it requires both caster & target to be in world
    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
        return false;

    // do not save bind sight auras
    if (HasEffectType(SPELL_AURA_BIND_SIGHT))
        return false;

    // no charming auras (taking direct control)
    if (HasEffectType(SPELL_AURA_MOD_POSSESS))
        return false;

    // no charming auras can be saved
    if (HasEffectType(SPELL_AURA_MOD_CHARM) || HasEffectType(SPELL_AURA_AOE_CHARM))
        return false;

    // Incanter's Absorbtion - considering the minimal duration and problems with aura stacking
    // we skip saving this aura
    // Also for some reason other auras put as MultiSlot crash core on keeping them after restart,
    // so put here only these for which you are sure they get removed
    switch (GetId())
    {
    case 44413: // Incanter's Absorption
    case 40075: // Fel Flak Fire
    case 55849: // Power Spark
    case 96206: // Nature's Bounty
    case 81206: // Chakra: Sanctuary
    case 81207: // Chakra: Sanctuary
    case 81208: // Chakra: Serenity
    case 81209: // Chakra: Chastise
    case 68631: // Curse of the Worgen
    case 89912: // Chakra Flow
        return false;
    }

    // When a druid logins, he doesnt have either eclipse power, nor the marker auras, nor the eclipse buffs. Dont save them.
    if (GetId() == 67483 || GetId() == 67484 || GetId() == 48517 || GetId() == 48518 || GetId() == 94338)
        return false;

    // don't save auras removed by proc system
    if (IsUsingCharges() && !GetCharges())
        return false;

    // don't save permanent auras triggered by items, they'll be recasted on login if necessary
    if (GetCastItemGUID() && IsPermanent())
        return false;

    return true;
}

bool Aura::CanBeSentToClient() const
{
    if (!IsPassive() || GetSpellInfo()->HasAreaAuraEffect())
        return true;

    return HasEffectType(SPELL_AURA_ABILITY_IGNORE_AURASTATE) || HasEffectType(SPELL_AURA_CAST_WHILE_WALKING) || HasEffectType(SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS) ||
           HasEffectType(SPELL_AURA_MOD_SPELL_COOLDOWN_BY_HASTE);
}

bool Aura::IsLimitedTargetWith(Aura const* aura) const
{
    // Same spell?
    if (GetSpellInfo()->IsRankOf(aura->GetSpellInfo()))
        return true;

    SpellSpecificType spec = GetSpellInfo()->GetSpellSpecific();
    // spell with single target specific types
    switch (spec)
    {
    case SPELL_SPECIFIC_JUDGEMENT:
    case SPELL_SPECIFIC_MAGE_POLYMORPH:
        if (aura->GetSpellInfo()->GetSpellSpecific() == spec)
            return true;
        break;
    default:
        break;
    }

    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE) && aura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
        return true;

    return false;
}

void Aura::UnregisterLimitedTarget()
{
    ASSERT(m_isLimitedTarget);
    Unit* caster = GetCaster();
    ASSERT(caster);
    caster->GetLimitedCastAuras(GetId()).remove(this);
    SetIsLimitedTarget(false);
}

int32 Aura::CalcDispelChance(Unit const* auraTarget, bool offensive) const
{
    // we assume that aura dispel chance is 100% on start
    // need formula for level difference based chance
    int32 resistChance = 0;

    // Apply dispel mod from aura caster
    if (Unit* caster = GetCaster())
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_RESIST_DISPEL_CHANCE, resistChance);

    // Dispel resistance from target SPELL_AURA_MOD_DISPEL_RESIST
    // Only affects offensive dispels
    if (offensive && auraTarget)
        resistChance += auraTarget->GetTotalAuraModifier(SPELL_AURA_MOD_DISPEL_RESIST);

    RoundToInterval(resistChance, 0, 100);
    return 100 - resistChance;
}

void Aura::SetLoadedState(int32 maxduration, int32 duration, int32 charges, uint8 stackamount, uint8 recalculateMask, float critChance, bool applyResilience, int32* amount)
{
    m_maxDuration = maxduration;
    m_duration = duration;
    m_procCharges = charges;
    m_isUsingCharges = m_procCharges != 0;
    m_stackAmount = stackamount;
    SetCritChance(critChance);
    SetCanApplyResilience(applyResilience);
    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (AuraEffect* aurEff = m_effects[i])
        {
            aurEff->SetAmount(amount[i]);
            aurEff->SetCanBeRecalculated((recalculateMask & (1 << i)) != 0);
            aurEff->CalculatePeriodic(caster, false, true);
            aurEff->CalculateSpellMod();
            aurEff->RecalculateAmount(caster);
        }
    }
}

bool Aura::HasEffectType(AuraType type) const
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (HasEffect(i) && m_effects[i]->GetAuraType() == type)
            return true;
    }
    return false;
}

bool Aura::EffectTypeNeedsSendingAmount(AuraType type)
{
    switch (type)
    {
    case SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS:
    case SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS_TRIGGERED:
    case SPELL_AURA_MOD_SPELL_CATEGORY_COOLDOWN:
        return true;
    default:
        break;
    }

    return false;
}

void Aura::RecalculateAmountOfEffects()
{
    ASSERT(!IsRemoved());
    Unit* caster = GetCaster();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (HasEffect(i))
            m_effects[i]->RecalculateAmount(caster);
}

void Aura::HandleAllEffects(AuraApplication* aurApp, uint8 mode, bool apply)
{
    ASSERT(!IsRemoved());
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (m_effects[i] && !IsRemoved())
            m_effects[i]->HandleEffect(aurApp, mode, apply);
}

void Aura::GetApplicationList(Unit::AuraApplicationList& applicationList) const
{
    for (Aura::ApplicationMap::const_iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
    {
        if (appIter->second->GetEffectMask())
            applicationList.push_back(appIter->second);
    }
}

void Aura::SetNeedClientUpdateForTargets() const
{
    for (ApplicationMap::const_iterator appIter = m_applications.begin(); appIter != m_applications.end(); ++appIter)
        appIter->second->SetNeedClientUpdate();
}

// trigger effects on real aura apply/remove
void Aura::HandleAuraSpecificMods(AuraApplication const* aurApp, Unit* caster, bool apply, bool onReapply)
{
    Unit* target = aurApp->GetTarget();
    EnumFlag<AuraRemoveFlags> removeMode = aurApp->GetRemoveMode();

    // handle spell_area table
    SpellAreaForAreaMapBounds saBounds = sSpellMgr->GetSpellAreaForAuraMapBounds(GetId());
    if (saBounds.first != saBounds.second)
    {
        uint32 zone, area;
        target->GetZoneAndAreaId(zone, area);

        for (SpellAreaForAreaMap::const_iterator itr = saBounds.first; itr != saBounds.second; ++itr)
        {
            // some auras remove at aura remove
            if (itr->second->flags & SPELL_AREA_FLAG_AUTOREMOVE && !itr->second->IsFitToRequirements(target->ToPlayer(), zone, area))
                target->RemoveAurasDueToSpell(itr->second->spellId);
            // some auras applied at aura apply
            else if (itr->second->flags & SPELL_AREA_FLAG_AUTOCAST)
            {
                if (!target->HasAura(itr->second->spellId))
                    target->CastSpell(target, itr->second->spellId, true);
            }
        }
    }

    // handle spell_linked_spell table
    if (!onReapply)
    {
        // apply linked auras
        if (apply)
        {
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->ApplySpellImmune(GetId(), IMMUNITY_ID, -(*itr), true);
                    else if (caster)
                        caster->AddAura(*itr, target);
                }
            }
        }
        else
        {
            // remove linked auras
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(-(int32)GetId()))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->RemoveAurasDueToSpell(-(*itr));
                    else if (!removeMode.HasFlag(AuraRemoveFlags::ByDeath))
                        target->CastSpell(target, *itr, GetCasterGUID());
                }
            }
            if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
            {
                for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                {
                    if (*itr < 0)
                        target->ApplySpellImmune(GetId(), IMMUNITY_ID, -(*itr), false);
                    else
                        target->RemoveAura(*itr, GetCasterGUID(), 0, removeMode);
                }
            }
        }
    }
    else if (apply)
    {
        // modify stack amount of linked auras
        if (std::vector<int32> const* spellTriggered = sSpellMgr->GetSpellLinked(GetId() + SPELL_LINK_AURA))
        {
            for (std::vector<int32>::const_iterator itr = spellTriggered->begin(); itr != spellTriggered->end(); ++itr)
                if (*itr > 0)
                    if (Aura* triggeredAura = target->GetAura(*itr, GetCasterGUID()))
                        triggeredAura->ModStackAmount(GetStackAmount() - triggeredAura->GetStackAmount());
        }
    }

    // mods at aura apply
    if (apply)
    {
        switch (GetSpellInfo()->SpellFamilyName)
        {
        case SPELLFAMILY_GENERIC:
            switch (GetId())
            {
            case 32474: // Buffeting Winds of Susurrus
                if (target->GetTypeId() == TYPEID_PLAYER)
                    target->ToPlayer()->ActivateTaxiPathTo(506, GetId());
                break;
            case 33572: // Gronn Lord's Grasp, becomes stoned
                if (GetStackAmount() >= 5 && !target->HasAura(33652))
                    target->CastSpell(target, 33652, true);
                break;
            case 50836: // Petrifying Grip, becomes stoned
                if (GetStackAmount() >= 5 && !target->HasAura(50812))
                    target->CastSpell(target, 50812, true);
                break;
            case 60970: // Heroic Fury (remove Intercept cooldown)
                if (target->GetTypeId() == TYPEID_PLAYER)
                    target->GetSpellHistory()->ResetCooldown(20252, true);
                break;
            }
            break;
        case SPELLFAMILY_DRUID:
            if (!caster)
                break;
            // Rejuvenation
            if (GetSpellInfo()->SpellFamilyFlags[0] & 0x10 && GetEffect(EFFECT_0))
            {
                // Druid T8 Restoration 4P Bonus
                if (caster->HasAura(64760))
                {
                    CastSpellExtraArgs args(GetEffect(EFFECT_0));
                    args.AddSpellMod(SPELLVALUE_BASE_POINT0, GetEffect(EFFECT_0)->GetAmount());
                    caster->CastSpell(target, 64801, args);
                }
            }
            break;
        case SPELLFAMILY_MAGE:
            if (!caster)
                break;
            /// @todo This should be moved to similar function in spell::hit
            if (GetSpellInfo()->SpellFamilyFlags[0] & 0x01000000)
            {
                // Polymorph Sound - Sheep && Penguin
                if (GetSpellInfo()->SpellIconID == 82 && GetSpellInfo()->SpellVisual[0] == 12978)
                {
                    if (caster->HasAura(52648)) // Glyph of the Penguin
                        caster->CastSpell(target, 61635, true);
                    else if (caster->HasAura(57927)) // Glyph of the Monkey
                        caster->CastSpell(target, 89729, true);
                    else
                        caster->CastSpell(target, 61634, true);
                }
            }
            break;
        case SPELLFAMILY_PRIEST:
            if (!caster)
                break;

            // Power Word: Shield
            else if (m_spellInfo->SpellFamilyFlags[0] & 0x1 && m_spellInfo->SpellFamilyFlags[2] & 0x400 && GetEffect(0))
            {
                // Glyph of Power Word: Shield
                if (AuraEffect* glyph = caster->GetAuraEffect(55672, 0))
                {
                    // instantly heal m_amount% of the absorb-value
                    int32 heal = glyph->GetAmount() * GetEffect(0)->GetAmount() / 100;
                    CastSpellExtraArgs args(GetEffect(0));
                    args.AddSpellMod(SPELLVALUE_BASE_POINT0, heal);
                    caster->CastSpell(GetUnitOwner(), 56160, args);
                }
            }
            break;
        case SPELLFAMILY_ROGUE:
            // Sprint (skip non player cast spells by category)
            if (GetSpellInfo()->SpellFamilyFlags[0] & 0x40 && GetSpellInfo()->GetCategory() == 44)
                // in official maybe there is only one icon?
                if (target->HasAura(58039))                 // Glyph of Blurred Speed
                    target->CastSpell(target, 61922, true); // Sprint (waterwalk)
            break;
        }
    }
    // mods at aura remove
    else
    {
        switch (GetSpellInfo()->SpellFamilyName)
        {
        case SPELLFAMILY_GENERIC:
            switch (GetId())
            {
            case 61987: // Avenging Wrath
                // Remove the immunity shield marker on Avenging Wrath removal if Forbearance is not present
                if (target->HasAura(61988) && !target->HasAura(25771))
                    target->RemoveAura(61988);
                break;
            default:
                break;
            }
            break;
        case SPELLFAMILY_MAGE:
            switch (GetId())
            {
            case 66: // Invisibility
            {
                if (!removeMode.HasFlag(AuraRemoveFlags::Expired))
                    break;

                target->CastSpell(target, 32612, GetEffect(EFFECT_1));
                break;
            }
            default:
                break;
            }
            break;
        case SPELLFAMILY_WARLOCK:
            if (!caster)
                break;
            // Improved Fear
            if (GetSpellInfo()->SpellFamilyFlags[1] & 0x00000400)
            {
                if (AuraEffect* aurEff = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, 98, 0))
                {
                    uint32 spellId = 0;
                    switch (aurEff->GetId())
                    {
                    case 53759:
                        spellId = 60947;
                        break;
                    case 53754:
                        spellId = 60946;
                        break;
                    default:
                        LOG_ERROR("spells", "Aura::HandleAuraSpecificMods: Unknown rank of Improved Fear (%d) found", aurEff->GetId());
                    }
                    if (spellId)
                        caster->CastSpell(target, spellId, true);
                }
            }
            break;
        case SPELLFAMILY_PRIEST:
            if (!caster)
                break;
            // Power word: shield
            if (removeMode.HasFlag(AuraRemoveFlags::ByEnemySpell) && GetSpellInfo()->SpellFamilyFlags[0] & 0x00000001)
            {
                // Rapture
                if (Aura const* aura = caster->GetAuraOfRankedSpell(47535))
                {
                    // check cooldown
                    if (caster->GetTypeId() == TYPEID_PLAYER)
                    {
                        if (caster->GetSpellHistory()->HasCooldown(aura->GetId()))
                        {
                            // This additional check is needed to add a minimal delay before cooldown in in effect
                            // to allow all bubbles broken by a single damage source proc mana return
                            if (caster->GetSpellHistory()->GetRemainingCooldown(aura->GetSpellInfo()) <= 11 * IN_MILLISECONDS)
                                break;
                        }
                        else // and add if needed
                            caster->GetSpellHistory()->AddCooldown(aura->GetId(), 0, std::chrono::seconds(12));
                    }

                    // effect on caster
                    if (AuraEffect const* aurEff = aura->GetEffect(0))
                    {
                        float multiplier = float(aurEff->GetAmount());

                        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
                        args.AddSpellBP0(CalculatePct(caster->GetMaxPower(POWER_MANA), multiplier));
                        caster->CastSpell(caster, 47755, args);
                    }
                }
            }
            break;
        case SPELLFAMILY_ROGUE:
            // Remove Vanish on stealth remove
            if (GetId() == 1784)
                target->RemoveAurasWithFamily(SPELLFAMILY_ROGUE, 0x0000800, 0, 0, target->GetGUID());
            break;
        case SPELLFAMILY_PALADIN:
            // Remove the immunity shield marker on Forbearance removal if AW marker is not present
            if (GetId() == 25771 && target->HasAura(61988) && !target->HasAura(61987))
                target->RemoveAura(61988);
            break;
        case SPELLFAMILY_DEATHKNIGHT:
            // Blood of the North
            // Reaping
            // Death Rune Mastery
            if (GetSpellInfo()->SpellIconID == 3041 || GetSpellInfo()->SpellIconID == 22 || GetSpellInfo()->SpellIconID == 2622)
            {
                if (!GetEffect(0) || GetEffect(0)->GetAuraType() != SPELL_AURA_PERIODIC_DUMMY)
                    break;
                if (target->GetTypeId() != TYPEID_PLAYER)
                    break;
                if (target->ToPlayer()->getClass() != CLASS_DEATH_KNIGHT)
                    break;

                // aura removed - remove death runes
                target->ToPlayer()->RemoveRunesByAuraEffect(GetEffect(0));
            }
            break;
        case SPELLFAMILY_HUNTER:
            // Glyph of Freezing Trap
            if (GetSpellInfo()->SpellFamilyFlags[0] & 0x00000008)
                if (caster && caster->HasAura(56845))
                    target->CastSpell(target, 61394, true);
            break;
        }
    }

    // mods at aura apply or remove
    switch (GetSpellInfo()->SpellFamilyName)
    {
    case SPELLFAMILY_HUNTER:
        switch (GetId())
        {
        case 19574: // Bestial Wrath
            // The Beast Within cast on owner if talent present
            if (Unit* owner = target->GetOwner())
            {
                // Search talent
                if (owner->HasAura(34692))
                {
                    if (apply)
                        owner->CastSpell(owner, 34471, GetEffect(EFFECT_0));
                    else
                        owner->RemoveAurasDueToSpell(34471);
                }
            }
            break;
        }
        break;
    case SPELLFAMILY_PALADIN:
        switch (GetId())
        {
        case 31842: // Divine Favor
            // Item - Paladin T10 Holy 2P Bonus
            if (target->HasAura(70755))
            {
                if (apply)
                    target->CastSpell(target, 71166, true);
                else
                    target->RemoveAurasDueToSpell(71166);
            }
            break;
        }
        break;
    }
}

bool Aura::CanBeAppliedOn(Unit* target)
{
    // unit not in world or during remove from world
    if (!target->IsInWorld() || target->IsDuringRemoveFromWorld())
    {
        // area auras mustn't be applied
        if (GetOwner() != target)
            return false;
        // do not apply non-selfcast single target auras
        if (GetCasterGUID() != GetOwner()->GetGUID() && (GetSpellInfo()->IsSingleTarget() || GetSpellInfo()->GetAuraTargetLimit()))
            return false;
        return true;
    }
    else
        return CheckAreaTarget(target);
}

bool Aura::CheckAreaTarget(Unit* target)
{
    return CallScriptCheckAreaTargetHandlers(target);
}

bool Aura::CanStackWith(Aura const* existingAura) const
{
    // Can stack with self
    if (this == existingAura)
        return true;

    bool sameCaster = GetCasterGUID() == existingAura->GetCasterGUID();
    SpellInfo const* existingSpellInfo = existingAura->GetSpellInfo();

    // Dynobj auras do not stack when they come from the same spell cast by the same caster
    if (GetType() == DYNOBJ_AURA_TYPE || existingAura->GetType() == DYNOBJ_AURA_TYPE)
    {
        if (sameCaster && m_spellInfo->Id == existingSpellInfo->Id)
            return false;
        return true;
    }

    // passive auras don't stack with another rank of the spell cast by same caster
    if (IsPassive() && sameCaster && (m_spellInfo->IsDifferentRankOf(existingSpellInfo) || (m_spellInfo->Id == existingSpellInfo->Id && m_castItemGuid.IsEmpty())))
        return false;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        // prevent remove triggering aura by triggered aura
        if (existingSpellInfo->Effects[i].TriggerSpell == GetId()
            // prevent remove triggered aura by triggering aura refresh
            || m_spellInfo->Effects[i].TriggerSpell == existingAura->GetId())
            return true;
    }

    // Check for custom server setting to allow tracking both Herbs and Minerals
    // Note: The following are client limitations and cannot be coded for:
    //  * The minimap tracking icon will display whichever skill is activated second
    //  * The minimap tracking list will only show a check mark next to the last skill activated
    //    Sometimes this bugs out and doesn't switch the check mark. It has no effect on the actual tracking though.
    //  * The minimap dots are yellow for both resources
    if (m_spellInfo->HasAura(SPELL_AURA_TRACK_RESOURCES) && existingSpellInfo->HasAura(SPELL_AURA_TRACK_RESOURCES))
        return sWorld->getBoolConfig(CONFIG_ALLOW_TRACK_BOTH_RESOURCES);

    // check spell specific stack rules
    if (m_spellInfo->IsAuraExclusiveBySpecificWith(existingSpellInfo) || (sameCaster && m_spellInfo->IsAuraExclusiveBySpecificPerCasterWith(existingSpellInfo)))
        return false;

    // check spell group stack rules
    switch (sSpellMgr->CheckSpellGroupStackRules(m_spellInfo, existingSpellInfo))
    {
    case SPELL_GROUP_STACK_RULE_EXCLUSIVE:
    case SPELL_GROUP_STACK_RULE_EXCLUSIVE_HIGHEST: // if it reaches this point, existing aura is lower/equal
        return false;
    case SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER:
        if (sameCaster)
            return false;
        break;
    case SPELL_GROUP_STACK_RULE_DEFAULT:
    case SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT:
    default:
        break;
    }

    if (m_spellInfo->SpellFamilyName != existingSpellInfo->SpellFamilyName)
        return true;

    if (!sameCaster)
    {
        // Channeled auras can stack if not forbidden by db or aura type
        if (existingAura->GetSpellInfo()->IsChanneled())
            return true;

        if (m_spellInfo->HasAttribute(SPELL_ATTR3_STACK_FOR_DIFF_CASTERS))
            return true;

        // check same periodic auras
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            switch (m_spellInfo->Effects[i].ApplyAuraName)
            {
            // DOT or HOT from different casters will stack
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DUMMY:
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
            case SPELL_AURA_PERIODIC_ENERGIZE:
            case SPELL_AURA_PERIODIC_MANA_LEECH:
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_POWER_BURN:
            case SPELL_AURA_OBS_MOD_POWER:
            case SPELL_AURA_OBS_MOD_HEALTH:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                // periodic auras which target areas are not allowed to stack this way (replenishment for example)
                if (m_spellInfo->Effects[i].IsTargetingArea() || existingSpellInfo->Effects[i].IsTargetingArea())
                    break;
                return true;
            default:
                break;
            }
        }
    }

    if (HasEffectType(SPELL_AURA_CONTROL_VEHICLE) && existingAura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE))
    {
        Vehicle* veh = nullptr;
        if (GetOwner()->ToUnit())
            veh = GetOwner()->ToUnit()->GetVehicleKit();

        if (!veh) // We should probably just let it stack. Vehicle system will prevent undefined behaviour later
            return true;

        if (!veh->GetAvailableSeatCount())
            return false; // No empty seat available

        return true; // Empty seat available (skip rest)
    }

    // spell of same spell rank chain
    if (m_spellInfo->IsRankOf(existingSpellInfo))
    {
        // don't allow passive area auras to stack
        if (m_spellInfo->IsMultiSlotAura() && !IsArea())
            return true;
        if (GetCastItemGUID() && existingAura->GetCastItemGUID())
            if (GetCastItemGUID() != existingAura->GetCastItemGUID() && m_spellInfo->HasAttribute(SPELL_ATTR0_CU_ENCHANT_PROC))
                return true;
        // same spell with same caster should not stack
        return false;
    }

    return true;
}

bool Aura::IsProcOnCooldown(std::chrono::steady_clock::time_point now) const
{
    return m_procCooldown > now;
}

void Aura::AddProcCooldown(std::chrono::steady_clock::time_point cooldownEnd)
{
    m_procCooldown = cooldownEnd;
}

void Aura::ResetProcCooldown()
{
    m_procCooldown = std::chrono::steady_clock::now();
}

void Aura::PrepareProcToTrigger(AuraApplication* aurApp, ProcEventInfo& eventInfo, std::chrono::steady_clock::time_point now)
{
    bool prepare = CallScriptPrepareProcHandlers(aurApp, eventInfo);
    if (!prepare)
        return;

    SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId());
    ASSERT(procEntry);

    // take one charge, aura expiration will be handled in Aura::TriggerProcOnEvent (if needed)
    if (!(procEntry->AttributesMask & PROC_ATTR_USE_STACKS_FOR_CHARGES) && IsUsingCharges() &&
        (!eventInfo.GetSpellInfo() || !eventInfo.GetSpellInfo()->HasAttribute(SPELL_ATTR6_DONT_CONSUME_PROC_CHARGES)))
    {
        --m_procCharges;
        SetNeedClientUpdateForTargets();
    }

    // cooldowns should be added to the whole aura (see 51698 area aura)
    AddProcCooldown(now + procEntry->Cooldown);
}

uint8 Aura::GetProcEffectMask(AuraApplication* aurApp, ProcEventInfo& eventInfo, std::chrono::steady_clock::time_point now) const
{
    SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(GetId());
    // only auras with spell proc entry can trigger proc
    if (!procEntry)
        return 0;

    // check spell triggering us
    if (Spell const* spell = eventInfo.GetProcSpell())
    {
        // Do not allow auras to proc from effect triggered from itself
        if (spell->IsTriggeredByAura(m_spellInfo))
            return 0;

        // check if aura can proc when spell is triggered (exception for hunter auto shot & wands)
        if (!GetSpellInfo()->HasAttribute(SPELL_ATTR3_CAN_PROC_FROM_PROCS) && !(procEntry->AttributesMask & PROC_ATTR_TRIGGERED_CAN_PROC) && !(eventInfo.GetTypeMask() & AUTO_ATTACK_PROC_FLAG_MASK))
            if (spell->IsTriggered() && !spell->GetSpellInfo()->HasAttribute(SPELL_ATTR3_NOT_A_PROC))
                return 0;

        if (spell->m_CastItem && (procEntry->AttributesMask & PROC_ATTR_CANT_PROC_FROM_ITEM_CAST))
            return 0;
    }

    // check don't break stealth attr present
    if (m_spellInfo->HasAura(SPELL_AURA_MOD_STEALTH))
    {
        if (SpellInfo const* spellInfo = eventInfo.GetSpellInfo())
            if (spellInfo->HasAttribute(SPELL_ATTR0_CU_DONT_BREAK_STEALTH))
                return 0;
    }

    // check if we have charges to proc with
    if (IsUsingCharges())
    {
        if (!GetCharges())
            return 0;

        if (procEntry->AttributesMask & PROC_ATTR_REQ_SPELLMOD)
            if (Spell const* spell = eventInfo.GetProcSpell())
                if (!spell->m_appliedMods.count(const_cast<Aura*>(this)))
                    return 0;
    }

    // check proc cooldown
    if (IsProcOnCooldown(now))
        return 0;

    // do checks against db data
    if (!SpellMgr::CanSpellTriggerProcOnEvent(*procEntry, eventInfo))
        return 0;

    // do checks using conditions table
    if (!sConditionMgr->IsObjectMeetingNotGroupedConditions(CONDITION_SOURCE_TYPE_SPELL_PROC, GetId(), eventInfo.GetActor(), eventInfo.GetActionTarget()))
        return 0;

    // AuraScript Hook
    bool check = const_cast<Aura*>(this)->CallScriptCheckProcHandlers(aurApp, eventInfo);
    if (!check)
        return 0;

    // At least one effect has to pass checks to proc aura
    uint8 procEffectMask = aurApp->GetEffectMask();
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (procEffectMask & (1u << i))
            if ((procEntry->DisableEffectsMask & (1u << i)) || !GetEffect(i)->CheckEffectProc(aurApp, eventInfo))
                procEffectMask &= ~(1u << i);

    if (!procEffectMask)
        return 0;

    /// @todo
    // do allow additional requirements for procs
    // this is needed because this is the last moment in which you can prevent aura charge drop on proc
    // and possibly a way to prevent default checks (if there're going to be any)

    // Check if current equipment meets aura requirements
    // do that only for passive spells
    /// @todo this needs to be unified for all kinds of auras
    Unit* target = aurApp->GetTarget();
    if (IsPassive() && target->GetTypeId() == TYPEID_PLAYER && GetSpellInfo()->EquippedItemClass != -1)
    {
        if (!GetSpellInfo()->HasAttribute(SPELL_ATTR3_IGNORE_PROC_SUBCLASS_MASK))
        {
            Item* item = nullptr;
            if (GetSpellInfo()->EquippedItemClass == ITEM_CLASS_WEAPON)
            {
                if (target->ToPlayer()->IsInFeralForm())
                    return 0;

                if (DamageInfo const* damageInfo = eventInfo.GetDamageInfo())
                {
                    switch (damageInfo->GetAttackType())
                    {
                    case BASE_ATTACK:
                        item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                        break;
                    case OFF_ATTACK:
                        item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                        break;
                    default:
                        item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
                        break;
                    }
                }
            }
            else if (GetSpellInfo()->EquippedItemClass == ITEM_CLASS_ARMOR)
            {
                // Check if player is wearing shield
                item = target->ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            }

            if (!item || item->IsBroken() || !item->IsFitToSpellRequirements(GetSpellInfo()))
                return 0;
        }
    }

    if (roll_chance_f(CalcProcChance(*procEntry, eventInfo)))
        return procEffectMask;

    return 0;
}

float Aura::CalcProcChance(SpellProcEntry const& procEntry, ProcEventInfo& eventInfo) const
{
    float chance = procEntry.Chance;
    // calculate chances depending on unit with caster's data
    // so talents modifying chances and judgements will have properly calculated proc chance
    if (Unit* caster = GetCaster())
    {
        // calculate ppm chance if present and we're using weapon
        if (eventInfo.GetDamageInfo() && procEntry.ProcsPerMinute != 0)
        {
            uint32 WeaponSpeed = caster->GetAttackTime(eventInfo.GetDamageInfo()->GetAttackType());
            chance = caster->GetPPMProcChance(WeaponSpeed, procEntry.ProcsPerMinute, GetSpellInfo());
        }
        // apply chance modifer aura, applies also to ppm chance (see improved judgement of light spell)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_CHANCE_OF_SUCCESS, chance);
    }

    // proc chance is reduced by an additional 3.333% per level past 60
    if ((procEntry.AttributesMask & PROC_ATTR_REDUCE_PROC_60) && eventInfo.GetActor()->getLevel() > 60)
        chance = std::max(0.f, (1.f - ((eventInfo.GetActor()->getLevel() - 60) * 1.f / 30.f)) * chance);

    return chance;
}

void Aura::TriggerProcOnEvent(uint8 procEffectMask, AuraApplication* aurApp, ProcEventInfo& eventInfo)
{
    bool prevented = CallScriptProcHandlers(aurApp, eventInfo);
    if (!prevented)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!(procEffectMask & (1 << i)))
                continue;

            // OnEffectProc / AfterEffectProc hooks handled in AuraEffect::HandleProc()
            if (aurApp->HasEffect(i))
                GetEffect(i)->HandleProc(aurApp, eventInfo);
        }

        CallScriptAfterProcHandlers(aurApp, eventInfo);
    }

    // Remove aura if we've used last charge to proc
    if (ASSERT_NOTNULL(sSpellMgr->GetSpellProcEntry(GetId()))->AttributesMask & PROC_ATTR_USE_STACKS_FOR_CHARGES)
    {
        ModStackAmount(-1);
    }
    else if (IsUsingCharges())
    {
        if (!GetCharges())
            Remove();
    }
}

void Aura::_DeleteRemovedApplications()
{
    while (!m_removedApplications.empty())
    {
        delete m_removedApplications.front();
        m_removedApplications.pop_front();
    }
}

void Aura::LoadScripts()
{
    sScriptMgr->CreateAuraScripts(m_spellInfo->Id, m_loadedScripts, this);
    for (auto itr = m_loadedScripts.begin(); itr != m_loadedScripts.end(); ++itr)
    {
        LOG_DEBUG("spells", "Aura::LoadScripts: Script `%s` for aura `%u` is loaded now", (*itr)->_GetScriptName()->c_str(), m_spellInfo->Id);
        (*itr)->Register();
    }
}

bool Aura::CallScriptCheckAreaTargetHandlers(Unit* target)
{
    bool result = true;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_CHECK_AREA_TARGET);
        auto hookItrEnd = (*scritr)->DoCheckAreaTarget.end(), hookItr = (*scritr)->DoCheckAreaTarget.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            result &= hookItr->Call(target);

        (*scritr)->_FinishScriptCall();
    }
    return result;
}

void Aura::CallScriptDispel(DispelInfo* dispelInfo)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_DISPEL);
        auto hookItrEnd = (*scritr)->OnDispel.end(), hookItr = (*scritr)->OnDispel.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            hookItr->Call(dispelInfo);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptAfterDispel(DispelInfo* dispelInfo)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_AFTER_DISPEL);
        auto hookItrEnd = (*scritr)->AfterDispel.end(), hookItr = (*scritr)->AfterDispel.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            hookItr->Call(dispelInfo);

        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptEffectApplyHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    bool preventDefault = false;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_APPLY, aurApp);
        auto effEndItr = (*scritr)->OnEffectApply.end(), effItr = (*scritr)->OnEffectApply.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex()), mode))
                effItr->Call(aurEff, mode);

        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }

    return preventDefault;
}

bool Aura::CallScriptEffectRemoveHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    bool preventDefault = false;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_REMOVE, aurApp);
        auto effEndItr = (*scritr)->OnEffectRemove.end(), effItr = (*scritr)->OnEffectRemove.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex()), mode))
                effItr->Call(aurEff, mode);

        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

void Aura::CallScriptAfterEffectApplyHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_APPLY, aurApp);
        auto effEndItr = (*scritr)->AfterEffectApply.end(), effItr = (*scritr)->AfterEffectApply.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex()), mode))
                effItr->Call(aurEff, mode);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptAfterEffectRemoveHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, AuraEffectHandleModes mode)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_REMOVE, aurApp);
        auto effEndItr = (*scritr)->AfterEffectRemove.end(), effItr = (*scritr)->AfterEffectRemove.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex()), mode))
                effItr->Call(aurEff, mode);

        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptEffectPeriodicHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp)
{
    bool preventDefault = false;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_PERIODIC, aurApp);
        auto effEndItr = (*scritr)->OnEffectPeriodic.end(), effItr = (*scritr)->OnEffectPeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff);

        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }

    return preventDefault;
}

void Aura::CallScriptEffectUpdatePeriodicHandlers(AuraEffect* aurEff)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_UPDATE_PERIODIC);
        auto effEndItr = (*scritr)->OnEffectUpdatePeriodic.end(), effItr = (*scritr)->OnEffectUpdatePeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcAmountHandlers(AuraEffect const* aurEff, int32& amount, bool& canBeRecalculated)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_AMOUNT);
        auto effEndItr = (*scritr)->DoEffectCalcAmount.end(), effItr = (*scritr)->DoEffectCalcAmount.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, amount, canBeRecalculated);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcPeriodicHandlers(AuraEffect const* aurEff, bool& isPeriodic, int32& amplitude)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_PERIODIC);
        auto effEndItr = (*scritr)->DoEffectCalcPeriodic.end(), effItr = (*scritr)->DoEffectCalcPeriodic.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, isPeriodic, amplitude);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectCalcSpellModHandlers(AuraEffect const* aurEff, SpellModifier*& spellMod)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_CALC_SPELLMOD);
        auto effEndItr = (*scritr)->DoEffectCalcSpellMod.end(), effItr = (*scritr)->DoEffectCalcSpellMod.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, spellMod);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAbsorbHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo& dmgInfo, uint32& absorbAmount, bool& defaultPrevented)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_ABSORB, aurApp);
        auto effEndItr = (*scritr)->OnEffectAbsorb.end(), effItr = (*scritr)->OnEffectAbsorb.begin();
        for (; effItr != effEndItr; ++effItr)

            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, dmgInfo, absorbAmount);

        if (!defaultPrevented)
            defaultPrevented = (*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAfterAbsorbHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo& dmgInfo, uint32& absorbAmount)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_ABSORB, aurApp);
        auto effEndItr = (*scritr)->AfterEffectAbsorb.end(), effItr = (*scritr)->AfterEffectAbsorb.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, dmgInfo, absorbAmount);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectManaShieldHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo& dmgInfo, uint32& absorbAmount, bool& /*defaultPrevented*/)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_MANASHIELD, aurApp);
        auto effEndItr = (*scritr)->OnEffectManaShield.end(), effItr = (*scritr)->OnEffectManaShield.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, dmgInfo, absorbAmount);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectAfterManaShieldHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo& dmgInfo, uint32& absorbAmount)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_MANASHIELD, aurApp);
        auto effEndItr = (*scritr)->AfterEffectManaShield.end(), effItr = (*scritr)->AfterEffectManaShield.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, dmgInfo, absorbAmount);

        (*scritr)->_FinishScriptCall();
    }
}

void Aura::CallScriptEffectSplitHandlers(AuraEffect* aurEff, AuraApplication const* aurApp, DamageInfo& dmgInfo, uint32& splitAmount)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_SPLIT, aurApp);
        auto effEndItr = (*scritr)->OnEffectSplit.end(), effItr = (*scritr)->OnEffectSplit.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, dmgInfo, splitAmount);

        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptCheckProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool result = true;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_CHECK_PROC, aurApp);
        auto hookItrEnd = (*scritr)->DoCheckProc.end(), hookItr = (*scritr)->DoCheckProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            result &= hookItr->Call(eventInfo);

        (*scritr)->_FinishScriptCall();
    }

    return result;
}

bool Aura::CallScriptPrepareProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool prepare = true;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_PREPARE_PROC, aurApp);
        auto effEndItr = (*scritr)->DoPrepareProc.end(), effItr = (*scritr)->DoPrepareProc.begin();
        for (; effItr != effEndItr; ++effItr)
            effItr->Call(eventInfo);

        if (prepare)
            prepare = !(*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }

    return prepare;
}

bool Aura::CallScriptProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool handled = false;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_PROC, aurApp);
        auto hookItrEnd = (*scritr)->OnProc.end(), hookItr = (*scritr)->OnProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            hookItr->Call(eventInfo);

        handled |= (*scritr)->_IsDefaultActionPrevented();
        (*scritr)->_FinishScriptCall();
    }

    return handled;
}

void Aura::CallScriptAfterProcHandlers(AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_AFTER_PROC, aurApp);
        auto hookItrEnd = (*scritr)->AfterProc.end(), hookItr = (*scritr)->AfterProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            hookItr->Call(eventInfo);

        (*scritr)->_FinishScriptCall();
    }
}

bool Aura::CallScriptCheckEffectProcHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool result = true;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_CHECK_EFFECT_PROC, aurApp);
        auto hookItrEnd = (*scritr)->DoCheckEffectProc.end(), hookItr = (*scritr)->DoCheckEffectProc.begin();
        for (; hookItr != hookItrEnd; ++hookItr)
            if (hookItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                result &= hookItr->Call(aurEff, eventInfo);

        (*scritr)->_FinishScriptCall();
    }

    return result;
}

bool Aura::CallScriptEffectProcHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    bool preventDefault = false;
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_PROC, aurApp);
        auto effEndItr = (*scritr)->OnEffectProc.end(), effItr = (*scritr)->OnEffectProc.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, eventInfo);

        if (!preventDefault)
            preventDefault = (*scritr)->_IsDefaultActionPrevented();

        (*scritr)->_FinishScriptCall();
    }
    return preventDefault;
}

void Aura::CallScriptAfterEffectProcHandlers(AuraEffect const* aurEff, AuraApplication const* aurApp, ProcEventInfo& eventInfo)
{
    for (auto scritr = m_loadedScripts.begin(); scritr != m_loadedScripts.end(); ++scritr)
    {
        (*scritr)->_PrepareScriptCall(AURA_SCRIPT_HOOK_EFFECT_AFTER_PROC, aurApp);
        auto effEndItr = (*scritr)->AfterEffectProc.end(), effItr = (*scritr)->AfterEffectProc.begin();
        for (; effItr != effEndItr; ++effItr)
            if (effItr->Filter(m_spellInfo, SpellEffIndex(aurEff->GetEffIndex())))
                effItr->Call(aurEff, eventInfo);

        (*scritr)->_FinishScriptCall();
    }
}

UnitAura::UnitAura(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32* baseAmount, Item* castItem, ObjectGuid casterGUID)
    : Aura(spellproto, owner, caster, castItem, casterGUID)
{
    m_AuraDRGroup = DIMINISHING_NONE;
    LoadScripts();
    _InitEffects(effMask, caster, baseAmount);
    GetUnitOwner()->_AddAura(this, caster);
}

void UnitAura::_ApplyForTarget(Unit* target, Unit* caster, AuraApplication* aurApp)
{
    Aura::_ApplyForTarget(target, caster, aurApp);

    // register aura diminishing on apply
    if (DiminishingGroup group = GetDiminishGroup())
        target->ApplyDiminishingAura(group, true);
}

void UnitAura::_UnapplyForTarget(Unit* target, Unit* caster, AuraApplication* aurApp)
{
    Aura::_UnapplyForTarget(target, caster, aurApp);

    // unregister aura diminishing (and store last time)
    if (DiminishingGroup group = GetDiminishGroup())
        target->ApplyDiminishingAura(group, false);
}

void UnitAura::Remove(AuraRemoveFlags removeMode)
{
    if (IsRemoved())
        return;
    GetUnitOwner()->RemoveOwnedAura(this, removeMode);
}

void UnitAura::FillTargetMap(std::unordered_map<Unit*, uint8>& targets, Unit* caster)
{
    Unit* ref = caster;
    if (!ref)
        ref = GetUnitOwner();

    for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
    {
        if (!HasEffect(effIndex))
            continue;

        std::vector<Unit*> units;
        // non-area aura
        ConditionContainer* condList = m_spellInfo->Effects[effIndex].ImplicitTargetConditions;
        // non-area aura
        if (GetSpellInfo()->Effects[effIndex].Effect == SPELL_EFFECT_APPLY_AURA || GetSpellInfo()->Effects[effIndex].Effect == SPELL_EFFECT_APPLY_AURA_2)
        {
            if (!condList || sConditionMgr->IsObjectMeetToConditions(GetUnitOwner(), ref, *condList))
                units.push_back(GetUnitOwner());
        }
        else
        {
            // skip area update if owner is not in world!
            if (!GetUnitOwner()->IsInWorld())
                continue;

            if (GetUnitOwner()->HasUnitState(UNIT_STATE_ISOLATED))
                continue;

            float radius = GetSpellInfo()->Effects[effIndex].CalcRadius(ref);
            SpellTargetCheckTypes selectionType = TARGET_CHECK_DEFAULT;
            switch (GetSpellInfo()->Effects[effIndex].Effect)
            {
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                selectionType = TARGET_CHECK_PARTY;
                break;
            case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                selectionType = TARGET_CHECK_RAID;
                break;
            case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                selectionType = TARGET_CHECK_ALLY;
                break;
            case SPELL_EFFECT_APPLY_AREA_AURA_ENEMY:
                selectionType = TARGET_CHECK_ENEMY;
                break;
            case SPELL_EFFECT_APPLY_AREA_AURA_PET:
                if (!condList || sConditionMgr->IsObjectMeetToConditions(GetUnitOwner(), ref, *condList))
                    units.push_back(GetUnitOwner());
                [[fallthrough]];
            case SPELL_EFFECT_APPLY_AREA_AURA_OWNER:
            {
                if (Unit* owner = GetUnitOwner()->GetCharmerOrOwner())
                    if (GetUnitOwner()->IsWithinDistInMap(owner, radius))
                        if (!condList || sConditionMgr->IsObjectMeetToConditions(owner, ref, *condList))
                            units.push_back(owner);
                break;
            }
            }

            if (selectionType != TARGET_CHECK_DEFAULT)
            {
                Firelands::WorldObjectSpellAreaTargetCheck check(radius, GetUnitOwner(), ref, GetUnitOwner(), m_spellInfo, selectionType, condList);
                Firelands::UnitListSearcher<Firelands::WorldObjectSpellAreaTargetCheck> searcher(GetUnitOwner(), units, check);
                Cell::VisitAllObjects(GetUnitOwner(), searcher, radius);
            }
        }

        for (Unit* unit : units)
        {
            auto itr = targets.find(unit);
            if (itr != targets.end())
                itr->second |= 1 << effIndex;
            else
                targets[unit] = 1 << effIndex;
        }
    }
}

DynObjAura::DynObjAura(SpellInfo const* spellproto, uint8 effMask, WorldObject* owner, Unit* caster, int32* baseAmount, Item* castItem, ObjectGuid casterGUID)
    : Aura(spellproto, owner, caster, castItem, casterGUID)
{
    LoadScripts();
    ASSERT(GetDynobjOwner());
    ASSERT(GetDynobjOwner()->IsInWorld());
    ASSERT(GetDynobjOwner()->GetMap() == caster->GetMap());
    _InitEffects(effMask, caster, baseAmount);
    GetDynobjOwner()->SetAura(this);
}

bool ChargeDropEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    // _base is always valid (look in Aura::_Remove())
    _base->ModChargesDelayed(-1, _mode);
    return true;
}

void DynObjAura::Remove(AuraRemoveFlags removeMode)
{
    if (IsRemoved())
        return;
    _Remove(removeMode);
}

void DynObjAura::FillTargetMap(std::unordered_map<Unit*, uint8>& targets, Unit* /*caster*/)
{
    Unit* dynObjOwnerCaster = GetDynobjOwner()->GetCaster();
    float radius = GetDynobjOwner()->GetRadius();

    for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
    {
        if (!HasEffect(effIndex))
            continue;

               // we can't use effect type like area auras to determine check type, check targets
        SpellTargetCheckTypes selectionType = m_spellInfo->Effects[effIndex].TargetA.GetCheckType();
        if (m_spellInfo->Effects[effIndex].TargetB.GetReferenceType() == TARGET_REFERENCE_TYPE_DEST)
            selectionType = m_spellInfo->Effects[effIndex].TargetB.GetCheckType();

        std::vector<Unit*> units;
        ConditionContainer* condList = m_spellInfo->Effects[effIndex].ImplicitTargetConditions;

        Firelands::WorldObjectSpellAreaTargetCheck check(radius, GetDynobjOwner(), dynObjOwnerCaster, dynObjOwnerCaster, m_spellInfo, selectionType, condList);
        Firelands::UnitListSearcher<Firelands::WorldObjectSpellAreaTargetCheck> searcher(GetDynobjOwner(), units, check);
        Cell::VisitAllObjects(GetDynobjOwner(), searcher, radius);

        for (Unit* unit : units)
        {
            auto itr = targets.find(unit);
            if (itr != targets.end())
                itr->second |= 1 << effIndex;
            else
                targets[unit] = 1 << effIndex;
        }
    }
}
