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

/* ScriptData
SDName: Boss_Majordomo_Executus
SD%Complete: 30
SDComment: Correct spawning and Event NYI
SDCategory: Molten Core
EndScriptData */

#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "molten_core.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "MotionMaster.h"

enum Texts
{
    SAY_AGGRO           = 0,
    SAY_SPAWN           = 1,
    SAY_SLAY            = 2,
    SAY_SPECIAL         = 3,
    SAY_DEFEAT          = 4,

    SAY_SUMMON_MAJ      = 5,
    SAY_ARRIVAL2_MAJ    = 6
};

enum Spells
{
    SPELL_SUMMON_RAGNAROS   = 19774,
    SPELL_BLAST_WAVE        = 20229,
    SPELL_TELEPORT          = 20618,
    SPELL_MAGIC_REFLECTION  = 20619,
    SPELL_AEGIS_OF_RAGNAROS = 20620,
    SPELL_DAMAGE_REFLECTION = 21075
};

enum Extras
{
    MENU_OPTION_YOU_CHALLENGED_US = 4108
};

enum Events
{
    EVENT_MAGIC_REFLECTION  = 1,
    EVENT_DAMAGE_REFLECTION = 2,
    EVENT_BLAST_WAVE        = 3,
    EVENT_TELEPORT          = 4,

    EVENT_OUTRO_1           = 5,
    EVENT_OUTRO_2           = 6,
    EVENT_OUTRO_3           = 7
};

Position const SummonPositions[9] =
{
    { 744.162f, -1151.63f, -119.726f, 4.58204f },
    { 751.247f, -1152.82f, -119.744f, 4.49673f },
    { 759.206f, -1155.09f, -120.051f, 4.30104f },
    { 755.973f, -1152.33f, -120.029f, 4.25588f },
    { 731.712f, -1147.56f, -120.195f, 4.95955f },
    { 726.499f, -1149.80f, -120.156f, 5.24055f },
    { 722.408f, -1152.41f, -120.029f, 5.33087f },
    { 718.994f, -1156.36f, -119.805f, 5.75738f },
    { 838.510f, -829.840f, -232.000f, 2.00000f }
};

class boss_majordomo : public CreatureScript
{
    public:
        boss_majordomo() : CreatureScript("boss_majordomo") { }

        struct boss_majordomoAI : public BossAI
        {
            boss_majordomoAI(Creature* creature) : BossAI(creature, BOSS_MAJORDOMO_EXECUTUS) { }

            void KilledUnit(Unit* /*victim*/) override
            {
                if (urand(0, 99) < 25)
                    Talk(SAY_SLAY);
            }

            void JustEngagedWith(Unit* who) override
            {
                BossAI::JustEngagedWith(who);
                Talk(SAY_AGGRO);
                events.ScheduleEvent(EVENT_MAGIC_REFLECTION, 30000);
                events.ScheduleEvent(EVENT_DAMAGE_REFLECTION, 15000);
                events.ScheduleEvent(EVENT_BLAST_WAVE, 10000);
                events.ScheduleEvent(EVENT_TELEPORT, 20000);
            }

            void Reset() override
            {
                _Reset();
                if (instance->GetBossState(BOSS_MAJORDOMO_EXECUTUS) == DONE)
                    return;

                DoSummon(NPC_FLAMEWAKER_HEALER, SummonPositions[0]);
                DoSummon(NPC_FLAMEWAKER_HEALER, SummonPositions[1]);
                DoSummon(NPC_FLAMEWAKER_HEALER, SummonPositions[2]);
                DoSummon(NPC_FLAMEWAKER_HEALER, SummonPositions[3]);
                DoSummon(NPC_FLAMEWAKER_ELITE, SummonPositions[4]);
                DoSummon(NPC_FLAMEWAKER_ELITE, SummonPositions[5]);
                DoSummon(NPC_FLAMEWAKER_ELITE, SummonPositions[6]);
                DoSummon(NPC_FLAMEWAKER_ELITE, SummonPositions[7]);
            }

            void EnterEvadeMode(EvadeReason /*why*/) override
            {
                _EnterEvadeMode();
                events.Reset();
                summons.DespawnAll();
                me->GetMotionMaster()->MoveTargetedHome();
            }

            void UpdateAI(uint32 diff) override
            {
                if (instance->GetBossState(BOSS_MAJORDOMO_EXECUTUS) != DONE)
                {
                    if (!UpdateVictim())
                        return;

                    events.Update(diff);

                    if (!me->FindNearestCreature(NPC_FLAMEWAKER_HEALER, 100.0f) && !me->FindNearestCreature(NPC_FLAMEWAKER_ELITE, 100.0f))
                    {
                        instance->UpdateEncounterStateForKilledCreature(me->GetEntry(), me);
                        me->SetFaction(FACTION_FRIENDLY);
                        Talk(SAY_DEFEAT);
                        _JustDied();
                        me->AttackStop();
                        me->SetReactState(REACT_PASSIVE);
                        me->GetMotionMaster()->MoveTargetedHome();
                        me->GetThreatManager().ClearAllThreat();
                        me->CombatStop(true);
                        events.ScheduleEvent(EVENT_OUTRO_1, 32000);
                        return;
                    }

                    if (me->HasUnitState(UNIT_STATE_CASTING))
                        return;

                    if (HealthBelowPct(50))
                        DoCast(me, SPELL_AEGIS_OF_RAGNAROS, true);

                    while (uint32 eventId = events.ExecuteEvent())
                    {
                        switch (eventId)
                        {
                            case EVENT_MAGIC_REFLECTION:
                                DoCast(me, SPELL_MAGIC_REFLECTION);
                                events.ScheduleEvent(EVENT_MAGIC_REFLECTION, 30000);
                                break;
                            case EVENT_DAMAGE_REFLECTION:
                                DoCast(me, SPELL_DAMAGE_REFLECTION);
                                events.ScheduleEvent(EVENT_DAMAGE_REFLECTION, 30000);
                                break;
                            case EVENT_BLAST_WAVE:
                                DoCastVictim(SPELL_BLAST_WAVE);
                                events.ScheduleEvent(EVENT_BLAST_WAVE, 10000);
                                break;
                            case EVENT_TELEPORT:
                                if (Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 1))
                                    DoCast(target, SPELL_TELEPORT);
                                events.ScheduleEvent(EVENT_TELEPORT, 20000);
                                break;
                            default:
                                break;
                        }

                        if (me->HasUnitState(UNIT_STATE_CASTING))
                            return;
                    }

                    DoMeleeAttackIfReady();
                }
                else
                {
                    events.Update(diff);

                    while (uint32 eventId = events.ExecuteEvent())
                    {
                        switch (eventId)
                        {
                            case EVENT_OUTRO_1:
                                me->NearTeleportTo(RagnarosTelePos.GetPositionX(), RagnarosTelePos.GetPositionY(), RagnarosTelePos.GetPositionZ(), RagnarosTelePos.GetOrientation());
                                me->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
                                break;
                            case EVENT_OUTRO_2:
                                instance->instance->SummonCreature(NPC_RAGNAROS, RagnarosSummonPos);
                                break;
                            case EVENT_OUTRO_3:
                                Talk(SAY_ARRIVAL2_MAJ);
                                break;
                            default:
                                break;
                        }
                    }
                }
            }

            void DoAction(int32 action) override
            {
                if (action == ACTION_START_RAGNAROS)
                {
                    me->RemoveFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
                    Talk(SAY_SUMMON_MAJ);
                    events.ScheduleEvent(EVENT_OUTRO_2, 8000);
                    events.ScheduleEvent(EVENT_OUTRO_3, 24000);
                }
                else if (action == ACTION_START_RAGNAROS_ALT)
                {
                    me->SetFaction(FACTION_FRIENDLY);
                    me->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
                }
            }

            bool GossipSelect(Player* player, uint32 menuId, uint32 /*gossipListId*/) override
            {
                if (menuId != MENU_OPTION_YOU_CHALLENGED_US)
                    return false;

                CloseGossipMenuFor(player);
                DoAction(ACTION_START_RAGNAROS);
                return true;
            }

            void DamageTaken(Unit* /*attacker*/, uint32& damage) override
            {
                if (damage >= me->GetHealth())
                    damage = me->GetHealth() - 1;
            }
        };

        CreatureAI* GetAI(Creature* creature) const override
        {
            return GetMoltenCoreAI<boss_majordomoAI>(creature);
        }
};

void AddSC_boss_majordomo()
{
    new boss_majordomo();
}
