/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ObjectAccessor.h"
#include "Unit.h"
#include "SpellInfo.h"
#include "Log.h"
#include "AreaTrigger.h"
#include "GridNotifiers.h"
#include "Chat.h"

AreaTrigger::AreaTrigger() : WorldObject(false), _duration(0), _activationDelay(0), _updateDelay(0)
{
    m_objectType |= TYPEMASK_AREATRIGGER;
    m_objectTypeId = TYPEID_AREATRIGGER;

    m_updateFlag = UPDATEFLAG_STATIONARY_POSITION;

    m_valuesCount = AREATRIGGER_END;
}

AreaTrigger::~AreaTrigger()
{
}

void AreaTrigger::AddToWorld()
{
    ///- Register the AreaTrigger for guid lookup and for caster
    if (!IsInWorld())
    {
        sObjectAccessor->AddObject(this);
        WorldObject::AddToWorld();
    }
}

void AreaTrigger::RemoveFromWorld()
{
    ///- Remove the AreaTrigger from the accessor and from all lists of objects in world
    if (IsInWorld())
    {
        WorldObject::RemoveFromWorld();
        sObjectAccessor->RemoveObject(this);
    }
}

bool AreaTrigger::CreateAreaTrigger(uint32 guidlow, uint32 triggerEntry, Unit* caster, SpellInfo const* spell, Position const& pos)
{
    SetMap(caster->GetMap());
    Relocate(pos);
    if (!IsPositionValid())
    {
        sLog->outError(LOG_FILTER_GENERAL, "AreaTrigger (spell %u) not created. Invalid coordinates (X: %f Y: %f)", spell->Id, GetPositionX(), GetPositionY());
        return false;
    }

    if (AreaTriggerInfo const* info = sObjectMgr->GetAreaTriggerInfo(triggerEntry))
    {
        atInfo = *info;
        _activationDelay = atInfo.activationDelay;
        _updateDelay = atInfo.updateDelay;

        for (AreaTriggerActionList::const_iterator itr = atInfo.actions.begin(); itr != atInfo.actions.end(); ++itr)
            _actionInfo[itr->id] = ActionInfo(&*itr);
    }

    WorldObject::_Create(guidlow, HIGHGUID_AREATRIGGER, caster->GetPhaseMask());

    SetEntry(triggerEntry);
    uint32 duration = spell->GetDuration();
    SetDuration(duration);
    SetObjectScale(1);

    SetUInt64Value(AREATRIGGER_CASTER, caster->GetGUID());
    SetUInt32Value(AREATRIGGER_SPELLID, spell->Id);
    SetUInt32Value(AREATRIGGER_SPELLVISUALID, spell->SpellVisual[0] ? spell->SpellVisual[0] : spell->SpellVisual[1]);
    SetUInt32Value(AREATRIGGER_DURATION, duration);
    SetFloatValue(AREATRIGGER_EXPLICIT_SCALE, GetRadius());

    if (!GetMap()->AddToMap(this))
        return false;

    // hack me
    if (atInfo.maxCount)
    {
        std::list<AreaTrigger*> oldTriggers;
        GetAreaTriggersWithEntryInRange(oldTriggers, triggerEntry, caster->GetGUID(), 150.0f);
        oldTriggers.sort(Trinity::GuidValueSorterPred());
        while (oldTriggers.size() > atInfo.maxCount)
        {
            AreaTrigger* at = oldTriggers.front();
            oldTriggers.remove(at);
            if (at->GetCasterGuid() == caster->GetGUID())
                at->Remove();
        }
    }

    return true;
}

void AreaTrigger::UpdateAffectedList(uint32 p_time, bool despawn)
{
    if (atInfo.actions.empty())
        return;

    if (!despawn)
    {
        for (std::list<uint64>::iterator itr = affectedPlayers.begin(); itr != affectedPlayers.end();)
        {
            Unit* unit = ObjectAccessor::GetUnit(*this, *itr);
            if (!unit)
            {
                affectedPlayers.erase(itr++);
                continue;
            }

            if (!unit->IsWithinDistInMap(this, GetRadius()))
            {
                affectedPlayers.erase(itr++);
                AffectUnit(unit, false);
                continue;
            }

            UpdateOnUnit(unit, p_time);
            ++itr;
        }

        std::list<Unit*> unitList;
        GetAttackableUnitListInRange(unitList, GetRadius());

        for (std::list<Unit*>::iterator itr = unitList.begin(); itr != unitList.end(); ++itr)
        {
            if (!IsUnitAffected((*itr)->GetGUID()))
            {
                affectedPlayers.push_back((*itr)->GetGUID());
                AffectUnit(*itr, true);
            }
        }
    }
    else
    {
        for (std::list<uint64>::iterator itr = affectedPlayers.begin(); itr != affectedPlayers.end();)
        {
            Unit* unit = ObjectAccessor::GetUnit(*this, *itr);
            if (!unit)
            {
                affectedPlayers.erase(itr++);
                continue;
            }

            AffectUnit(unit, false);
            affectedPlayers.erase(itr++);
        }
    }
}

void AreaTrigger::UpdateActionCharges(uint32 p_time)
{
    for (ActionInfoMap::iterator itr = _actionInfo.begin(); itr != _actionInfo.end(); ++itr)
    {
        ActionInfo& info = itr->second;
        if (!info.charges || !info.action->chargeRecoveryTime)
            continue;
        if (info.charges >= info.action->maxCharges)
            continue;

        info.recoveryTime += p_time;
        if (info.recoveryTime >= info.action->chargeRecoveryTime)
        {
            info.recoveryTime -= info.action->chargeRecoveryTime;
            ++info.charges;
            if (info.charges == info.action->maxCharges)
                info.recoveryTime = 0;
        }
    }
}

void AreaTrigger::Update(uint32 p_time)
{
    if (GetDuration() > int32(p_time))
    {
        UpdateActionCharges(p_time);

        _duration -= p_time;

        if (_activationDelay >= p_time)
            _activationDelay -= p_time;
        else
            _activationDelay = 0;

        if (!_activationDelay)
            UpdateAffectedList(p_time, false);
    }
    else
    {
        UpdateAffectedList(p_time, true);
        for (ActionInfoMap::iterator itr =_actionInfo.begin(); itr != _actionInfo.end(); ++itr)
        {
            ActionInfo& info = itr->second;
            if (info.action->moment != AT_ACTION_MOMENT_DESPAWN)
                continue;

            DoAction(NULL, info);
        }
        Remove(); // expired
    }

    WorldObject::Update(p_time);
}

bool AreaTrigger::IsUnitAffected(uint64 guid) const
{
    return std::find(affectedPlayers.begin(), affectedPlayers.end(), guid) != affectedPlayers.end();
}

void AreaTrigger::AffectUnit(Unit* unit, bool enter)
{
    if (unit->GetTypeId() == TYPEID_PLAYER)
        ChatHandler(unit->ToPlayer()).PSendSysMessage("AreaTrigger::AffectUnit %s %u", unit->GetName(), enter);

    for (ActionInfoMap::iterator itr =_actionInfo.begin(); itr != _actionInfo.end(); ++itr)
    {
        ActionInfo& info = itr->second;
        switch (info.action->moment)
        {
            case AT_ACTION_MOMENT_ENTER:
                if (!enter)
                    continue;
                break;
            case AT_ACTION_MOMENT_LEAVE:
                if (enter)
                    continue;
                break;
            default:
                continue;
        }

        DoAction(unit, info);
    }
}

void AreaTrigger::UpdateOnUnit(Unit* unit, uint32 p_time)
{
    if (unit->GetTypeId() == TYPEID_PLAYER)
        ChatHandler(unit->ToPlayer()).PSendSysMessage("AreaTrigger::UpdateOnUnit %s %u", unit->GetName(), p_time);

    if (atInfo.updateDelay)
    {
        if (_updateDelay > p_time)
        {
            _updateDelay -= p_time;
            return;
        }
        else
            _updateDelay = atInfo.updateDelay;
    }

    for (ActionInfoMap::iterator itr =_actionInfo.begin(); itr != _actionInfo.end(); ++itr)
    {
        ActionInfo& info = itr->second;
        if (info.action->moment != AT_ACTION_MOMENT_UPDATE)
            continue;

        DoAction(unit, info);
    }
}

void AreaTrigger::DoAction(Unit* unit, ActionInfo& action)
{
    // do not process depleted actions
    if (!action.charges && action.action->maxCharges)
        return;

    Unit* caster = GetCaster();

    if (action.action->targetFlags & AT_TARGET_FLAG_FRIENDLY)
        if (!caster || !caster->IsFriendlyTo(unit))
            return;
    if (action.action->targetFlags & AT_TARGET_FLAG_HOSTILE)
        if (!caster || !caster->IsHostileTo(unit))
            return;
    if (action.action->targetFlags & AT_TARGET_FLAG_OWNER)
        if (unit->GetGUID() != GetCasterGuid())
            return;
    if (action.action->targetFlags & AT_TARGET_FLAG_PLAYER)
        if (!unit->ToPlayer())
            return;
    if (action.action->targetFlags & AT_TARGET_FLAG_NOT_PET)
        if (unit->isPet())
            return;

    if (!CheckActionConditions(*action.action, unit))
        return;

    switch (action.action->actionType)
    {
        case AT_ACTION_TYPE_CAST_SPELL:
        {
            if (caster)
            {
                if (action.action->targetFlags & AT_TARGET_FLAG_CAST_AT_SRC)
                    caster->CastSpell(GetPositionX(), GetPositionY(), GetPositionZ(), action.action->spellId, TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_CASTED_BY_AREATRIGGER));
                else
                    caster->CastSpell(unit, action.action->spellId, TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_CASTED_BY_AREATRIGGER));
            }
            break;
        }
        case AT_ACTION_TYPE_REMOVE_AURA:
        {
            unit->RemoveAurasDueToSpell(action.action->spellId);
            break;
        }
    }

    if (action.charges > 0)
    {
        --action.charges;
        if (!action.charges)
            Remove();
    }
}

void AreaTrigger::Remove()
{
    if (IsInWorld())
    {
        SendObjectDeSpawnAnim(GetGUID());
        RemoveFromWorld();
        AddObjectToRemoveList();
    }
}

float AreaTrigger::GetRadius() const
{
    return atInfo.radius;
}

Unit* AreaTrigger::GetCaster() const
{
    return ObjectAccessor::GetUnit(*this, GetCasterGuid());
}

bool AreaTrigger::CheckActionConditions(AreaTriggerAction const& action, Unit* unit)
{
    Unit* caster = GetCaster();
    if (!caster)
        return false;

    ConditionSourceInfo srcInfo = ConditionSourceInfo(caster, unit);
    return sConditionMgr->IsObjectMeetToConditions(srcInfo, sConditionMgr->GetConditionsForAreaTriggerAction(GetEntry(), action.id));
}