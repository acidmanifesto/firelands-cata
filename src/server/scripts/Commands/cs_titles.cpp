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
Name: titles_commandscript
%Complete: 100
Comment: All titles related commands
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"

class titles_commandscript : public CommandScript
{
public:
    titles_commandscript() : CommandScript("titles_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> titlesSetCommandTable =
        {
            { "mask", rbac::RBAC_PERM_COMMAND_TITLES_SET_MASK, false, &HandleTitlesSetMaskCommand, "" },
        };
        static std::vector<ChatCommand> titlesCommandTable =
        {
            { "add",     rbac::RBAC_PERM_COMMAND_TITLES_ADD,     false, &HandleTitlesAddCommand,     "" },
            { "current", rbac::RBAC_PERM_COMMAND_TITLES_CURRENT, false, &HandleTitlesCurrentCommand, "" },
            { "remove",  rbac::RBAC_PERM_COMMAND_TITLES_REMOVE,  false, &HandleTitlesRemoveCommand,  "" },
            { "set",     rbac::RBAC_PERM_COMMAND_TITLES_SET,     false, nullptr,       "", titlesSetCommandTable },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "titles", rbac::RBAC_PERM_COMMAND_TITLES, false, nullptr, "", titlesCommandTable },
        };
        return commandTable;
    }

    static bool HandleTitlesCurrentCommand(ChatHandler* handler, char const* args)
    {
        // number or [name] Shift-click form |color|Htitle:title_id|h[name]|h|r
        char* id_p = handler->extractKeyFromLink((char*)args, "Htitle");
        if (!id_p)
            return false;

        int32 id = atoi(id_p);
        if (id <= 0)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendSysMessage(LANG_NO_CHAR_SELECTED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // check online security
        if (handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return false;

        CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(id);
        if (!titleInfo)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string tNameLink = handler->GetNameLink(target);

        target->SetTitle(titleInfo);                            // to be sure that title now known
        target->SetUInt32Value(PLAYER_CHOSEN_TITLE, titleInfo->Mask_ID);

        handler->PSendSysMessage(LANG_TITLE_CURRENT_RES, id, target->getGender() == GENDER_MALE ? titleInfo->Name : titleInfo->Name1, tNameLink.c_str());
        return true;
    }

    static bool HandleTitlesAddCommand(ChatHandler* handler, char const* args)
    {
        // number or [name] Shift-click form |color|Htitle:title_id|h[name]|h|r
        char* id_p = handler->extractKeyFromLink((char*)args, "Htitle");
        if (!id_p)
            return false;

        int32 id = atoi(id_p);
        if (id <= 0)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendSysMessage(LANG_NO_CHAR_SELECTED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // check online security
        if (handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return false;

        CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(id);
        if (!titleInfo)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string tNameLink = handler->GetNameLink(target);
        std::string titleNameStr = Firelands::StringFormat(target->getGender() == GENDER_MALE ? titleInfo->Name : titleInfo->Name1, target->GetName().c_str());

        target->SetTitle(titleInfo);
        handler->PSendSysMessage(LANG_TITLE_ADD_RES, id, titleNameStr.c_str(), tNameLink.c_str());

        return true;
    }

    static bool HandleTitlesRemoveCommand(ChatHandler* handler, char const* args)
    {
        // number or [name] Shift-click form |color|Htitle:title_id|h[name]|h|r
        char* id_p = handler->extractKeyFromLink((char*)args, "Htitle");
        if (!id_p)
            return false;

        int32 id = atoi(id_p);
        if (id <= 0)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendSysMessage(LANG_NO_CHAR_SELECTED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // check online security
        if (handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return false;

        CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(id);
        if (!titleInfo)
        {
            handler->PSendSysMessage(LANG_INVALID_TITLE_ID, id);
            handler->SetSentErrorMessage(true);
            return false;
        }

        target->SetTitle(titleInfo, true);

        std::string tNameLink = handler->GetNameLink(target);

        char titleNameStr[80];
        snprintf(titleNameStr, 80, target->getGender() == GENDER_MALE ? titleInfo->Name : titleInfo->Name1, target->GetName().c_str());

        handler->PSendSysMessage(LANG_TITLE_REMOVE_RES, id, titleNameStr, tNameLink.c_str());

        if (!target->HasTitle(target->GetInt32Value(PLAYER_CHOSEN_TITLE)))
        {
            target->SetUInt32Value(PLAYER_CHOSEN_TITLE, 0);
            handler->PSendSysMessage(LANG_CURRENT_TITLE_RESET, tNameLink.c_str());
        }

        return true;
    }

    //Edit Player KnownTitles
    static bool HandleTitlesSetMaskCommand(ChatHandler* handler, char const* args)
    {
        if (!*args)
            return false;

        uint64 titles = atoull(args);

        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendSysMessage(LANG_NO_CHAR_SELECTED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // check online security
        if (handler->HasLowerSecurity(target, ObjectGuid::Empty))
            return false;

        uint64 allValidTitleMask = 0;
        for (CharTitlesEntry const* tEntry : sCharTitlesStore)
            allValidTitleMask |= (uint64(1) << tEntry->Mask_ID);

        titles &= allValidTitleMask;                // remove non-existing titles

        target->SetUInt64Value(PLAYER__FIELD_KNOWN_TITLES, titles);
        handler->SendSysMessage(LANG_DONE);

        if (!target->HasTitle(target->GetInt32Value(PLAYER_CHOSEN_TITLE)))
        {
            target->SetUInt32Value(PLAYER_CHOSEN_TITLE, 0);
            handler->PSendSysMessage(LANG_CURRENT_TITLE_RESET, handler->GetNameLink(target).c_str());
        }

        return true;
    }
};

void AddSC_titles_commandscript()
{
    new titles_commandscript();
}
