/*
 * mod-dynamic-scaling: server side of the DynamicScaling addon (addon/DynamicScaling).
 *
 * The addon whispers to the player themself with the prefix DSCALE:
 *   client -> server  "HELLO"            your choice and the place you are in
 *                     "SET\t<0|1|2>"     choose Off / Simple / Advanced
 *                     "NEAR"             creatures around you, with their levels in each mode
 *   server -> client  "M\t<mode>\t<can choose 0/1>\t<off quest credit levels>\t<simple trail>\t<advanced trail>"
 *                     "Z\t<zone>\t<area>\t<scaled 0/1>\t<advanced min>\t<advanced max>\t<instance 0/1>"
 *                     "N\t<entry>\t<name>\t<own>\t<simple>\t<advanced>\t<now>\t<count>"  (0 = not scaled there)
 *                     "E\t<request>\t<count>"  end of a reply;  "X\t<error>"
 */

#include "DynamicScaling.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"

#include <algorithm>
#include <map>
#include <vector>

namespace
{
    constexpr char const* ADDON_PREFIX = "DSCALE";
    constexpr std::size_t NEAR_LIMIT = 40;

    void Send(Player* player, std::string const& body)
    {
        std::string text = std::string(ADDON_PREFIX) + "\t" + body;
        if (text.size() > 254)
            text.resize(254);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    std::string Clean(std::string text)
    {
        std::replace(text.begin(), text.end(), '\t', ' ');
        return text;
    }

    void SendMode(Player* player)
    {
        DSConfig const& config = sDynamicScaling->Config();
        Send(player, Acore::StringFormat("M\t{}\t{}\t{}\t{}\t{}", uint32(sDynamicScaling->GetPlayerMode(player)),
            config.playerToggle ? 1 : 0, config.offQuestCreditLevels, config.worldSimpleTrail,
            config.worldTrailLevels));
    }

    void SendZone(Player* player)
    {
        Map* map = player->GetMap();
        ZoneSettings settings;
        bool scaled = !map->Instanceable() && sDynamicScaling->GetZoneSettings(player->GetMapId(),
            player->GetZoneId(), player->GetAreaId(), settings, ScalingMode::Advanced);
        Send(player, Acore::StringFormat("Z\t{}\t{}\t{}\t{}\t{}\t{}", player->GetZoneId(), player->GetAreaId(),
            scaled ? 1 : 0, settings.levelMin, settings.levelMax, map->Instanceable() ? 1 : 0));
    }

    void SendNear(Player* player)
    {
        Map* map = player->GetMap();
        if (map->Instanceable())
        {
            Send(player, "X\tInstances use the instance settings, not your open world choice.");
            Send(player, "E\tNEAR\t0");
            return;
        }

        float range = sDynamicScaling->GetWorldRange(map);
        std::list<Creature*> creatures;
        Acore::AnyUnitInObjectRangeCheck check(player, range);
        Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(player, creatures, check);
        Cell::VisitObjects(player, searcher, range);

        // One line per entry: the closest one stands for the others
        struct Line
        {
            Creature* creature = nullptr;
            float distance = 0.0f;
            uint32 count = 0;
        };
        std::map<uint32, Line> byEntry;
        for (Creature* creature : creatures)
        {
            if (creature->IsPet() || creature->IsTotem() || creature->IsTrigger() || !player->CanSeeOrDetect(creature))
                continue;

            Line& line = byEntry[creature->GetEntry()];
            float distance = player->GetDistance(creature);
            if (!line.creature || distance < line.distance)
            {
                line.creature = creature;
                line.distance = distance;
            }
            ++line.count;
        }

        std::vector<Line> lines;
        for (auto const& [entry, line] : byEntry)
            lines.push_back(line);
        std::sort(lines.begin(), lines.end(), [](Line const& a, Line const& b) { return a.distance < b.distance; });
        if (lines.size() > NEAR_LIMIT)
            lines.resize(NEAR_LIMIT);

        for (Line const& line : lines)
        {
            uint8 own = 0;
            uint8 simple = 0;
            uint8 advanced = 0;
            sDynamicScaling->PreviewWorldLevels(line.creature, player, own, simple, advanced);
            Send(player, Acore::StringFormat("N\t{}\t{}\t{}\t{}\t{}\t{}\t{}", line.creature->GetEntry(),
                Clean(line.creature->GetName()), own, simple, advanced, line.creature->GetLevel(), line.count));
        }

        Send(player, Acore::StringFormat("E\tNEAR\t{}", lines.size()));
    }

    bool HandleAddonMessage(Player* player, std::string const& msg)
    {
        std::string prefix = std::string(ADDON_PREFIX) + "\t";
        if (msg.compare(0, prefix.size(), prefix) != 0)
            return false;

        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable)
        {
            Send(player, "X\tDynamic Scaling is off on this server.");
            return true;
        }

        std::string body = msg.substr(prefix.size());
        if (body == "HELLO")
        {
            SendMode(player);
            SendZone(player);
            Send(player, "E\tHELLO\t0");
        }
        else if (body.compare(0, 4, "SET\t") == 0)
        {
            if (!config.playerToggle)
                Send(player, "X\tScaling can't be changed on this server.");
            else
            {
                uint32 value = uint32(std::max(0, std::atoi(body.c_str() + 4)));
                ScalingMode mode = value == 1 ? ScalingMode::Simple :
                    value == 2 ? ScalingMode::Advanced : ScalingMode::Off;
                sDynamicScaling->SetPlayerChoice(player, mode);
            }

            SendMode(player);
            Send(player, "E\tSET\t0");
        }
        else if (body == "NEAR")
            SendNear(player);
        else
            Send(player, "X\tUnknown request.");

        return true;
    }
}

class DynamicScalingAddonScript : public PlayerScript
{
public:
    DynamicScalingAddonScript() : PlayerScript("DynamicScalingAddonScript",
        { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || receiver != player)
            return true;

        return !HandleAddonMessage(player, msg);
    }
};

void AddDynamicScalingAddonScripts()
{
    new DynamicScalingAddonScript();
}
