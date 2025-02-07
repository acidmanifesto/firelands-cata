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
#ifndef __FIRELANDS_CHANNELMGR_H
#define __FIRELANDS_CHANNELMGR_H

#include "Define.h"
#include "Hash.h"
#include <string>
#include <unordered_map>

class Channel;
class Player;
class WorldPacket;
struct AreaTableEntry;

class FC_GAME_API ChannelMgr
{
    typedef std::unordered_map<std::wstring, Channel*> CustomChannelContainer; // custom channels only differ in name
    typedef std::unordered_map<std::pair<uint32 /*channelId*/, uint32 /*zoneId*/>, Channel*> BuiltinChannelContainer; //identify builtin (DBC) channels by zoneId instead, since name changes by client locale

    protected:
        explicit ChannelMgr(uint32 team) : _team(team) { }
        ~ChannelMgr();

    public:
        static ChannelMgr* forTeam(uint32 team);
        static Channel* GetChannelForPlayerByNamePart(std::string const& namePart, Player* playerSearcher);

        Channel* GetJoinChannel(uint32 channelId, std::string const& name, AreaTableEntry const* zoneEntry = nullptr);
        Channel* GetChannel(uint32 channelId, std::string const& name, Player* player, bool pkt = true, AreaTableEntry const* zoneEntry = nullptr) const;
        void LeftChannel(std::string const& name);
        void LeftChannel(uint32 channelId, AreaTableEntry const* zoneEntry);

    private:
        CustomChannelContainer _customChannels;
        BuiltinChannelContainer _channels;
        uint32 const _team;

        static void MakeNotOnPacket(WorldPacket* data, std::string const& name);
};

#endif
