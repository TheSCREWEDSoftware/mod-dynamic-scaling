/*
 * mod-dynamic-scaling: GM commands.
 *
 *   .dscale info     settings for your map / zone, and for the selected creature
 *   .dscale reload   reload both tables (config: use .reload config)
 *   .dscale rescale  rescale the selected creature now
 */

#include "DynamicScaling.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "Map.h"
#include "Player.h"

#include <algorithm>

using namespace Acore::ChatCommands;

namespace
{
    char const* ModeName(ScalingMode mode)
    {
        switch (mode)
        {
            case ScalingMode::Simple:
                return "Simple";
            case ScalingMode::Advanced:
                return "Advanced";
            case ScalingMode::Off:
                return "Off";
            default:
                return "Default";
        }
    }
}

class DynamicScalingCommandScript : public CommandScript
{
public:
    DynamicScalingCommandScript() : CommandScript("DynamicScalingCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dscaleTable =
        {
            { "info",    HandleInfoCommand,    SEC_GAMEMASTER,    Console::No },
            { "reload",  HandleReloadCommand,  SEC_ADMINISTRATOR, Console::Yes },
            { "rescale", HandleRescaleCommand, SEC_GAMEMASTER,    Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "dscale",  dscaleTable },
            { "scaling", HandleScalingCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

    static bool HandleInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();

        if (map->IsDungeon())
        {
            InstanceSettings settings;
            bool active = sDynamicScaling->GetInstanceSettings(map, settings);
            DSMapInfo const* mapInfo = sDynamicScaling->GetMapInfo(map);

            handler->PSendSysMessage("Instance {} ({}) difficulty {}: {}", map->GetMapName(), map->GetId(),
                uint32(map->GetDifficulty()), active ? "scaling" : "not scaled");
            handler->PSendSysMessage("  mode {}, level scaling {}, window {}-{} (0 = creature's own), base level {}",
                ModeName(settings.mode), settings.levelScaling ? "on" : "off", settings.levelMin, settings.levelMax,
                settings.baseLevel);
            handler->PSendSysMessage("  expected players {}, in map {}, reference level {}, respawn {}",
                settings.expectedPlayers, mapInfo ? mapInfo->playerCount : 0,
                sDynamicScaling->GetReferenceLevel(mapInfo), settings.respawn ? "on" : "off");
        }
        else
        {
            // Show the settings for your own choice (Advanced ones when your scaling is off)
            ScalingMode playerMode = sDynamicScaling->GetPlayerMode(player);
            ZoneSettings settings;
            bool active = sDynamicScaling->GetZoneSettings(player->GetMapId(), player->GetZoneId(), player->GetAreaId(),
                settings, playerMode == ScalingMode::Off ? ScalingMode::Advanced : playerMode);
            handler->PSendSysMessage("Zone {} / area {}: {}, your choice: {}", player->GetZoneId(),
                player->GetAreaId(), active ? "scaling" : "not scaled", DynamicScalingMgr::ModeName(playerMode));
            if (active)
                handler->PSendSysMessage("  {}: window {}-{} (0 = creature's own), trail {} levels, XP floor {}%",
                    ModeName(settings.mode), settings.levelMin, settings.levelMax, settings.trailLevels,
                    settings.xpMinPct);
        }

        if (Creature* creature = handler->getSelectedCreature())
        {
            DSCreatureInfo const* info = creature->CustomData.Get<DSCreatureInfo>("DynamicScalingCreatureInfo");
            if (!info)
                handler->PSendSysMessage("Selected {}: no scaling data.", creature->GetName());
            else
                handler->PSendSysMessage("Selected {}: level {} (own {}, scaled {}), health x{:.2f}, damage x{:.2f}, "
                    "mana x{:.2f}, armor x{:.2f}", creature->GetName(), creature->GetLevel(), info->originalLevel,
                    info->scaledLevel ? DynamicScalingMgr::ModeName(info->worldMode) : "no", info->healthMult,
                    info->damageMult, info->manaMult, info->armorMult);

            if (!creature->GetMap()->Instanceable())
                handler->PSendSysMessage("  distance {:.0f} yd, scaling range {:.0f} yd, {}, {}",
                    creature->GetDistance(player), sDynamicScaling->GetWorldRange(creature->GetMap()),
                    sDynamicScaling->IsCreatureEligible(creature, true) ? "can scale" : "never scales (NPC, "
                    "civilian, unattackable or world boss)", creature->IsInCombat() ? "in combat" : "out of combat");
        }

        return true;
    }

    // .scaling [simple|advanced|off|on]: players choose their open world scaling
    static bool HandleScalingCommand(ChatHandler* handler, Optional<std::string> choice)
    {
        Player* player = handler->GetSession()->GetPlayer();
        DSConfig const& config = sDynamicScaling->Config();

        if (!config.enable || !config.playerToggle)
        {
            handler->SendErrorMessage("Scaling can't be toggled on this server.");
            return false;
        }

        if (!choice)
        {
            handler->PSendSysMessage("Your scaling: {}. Use .scaling simple, .scaling advanced or .scaling off.",
                DynamicScalingMgr::ModeName(sDynamicScaling->GetPlayerMode(player)));
            handler->SendSysMessage("  Simple: creatures in the open world trail behind your level anywhere, and "
                "always give experience.");
            handler->SendSysMessage("  Advanced: creatures trail behind you inside each zone's level range.");
            if (config.offQuestCreditLevels)
                handler->PSendSysMessage("  Off: creatures keep their own levels; those {} or more levels below you "
                    "give no quest items or progress.", config.offQuestCreditLevels);
            else
                handler->SendSysMessage("  Off: creatures keep their own levels.");
            return true;
        }

        std::string value = *choice;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);

        ScalingMode mode;
        if (value == "simple" || value == "1")
            mode = ScalingMode::Simple;
        else if (value == "advanced" || value == "complex" || value == "2")
            mode = ScalingMode::Advanced;
        else if (value == "off" || value == "0" || value == "no")
            mode = ScalingMode::Off;
        else if (value == "on" || value == "yes")
            mode = config.playerDefault == ScalingMode::Off ? ScalingMode::Simple : config.playerDefault;
        else
        {
            handler->SendErrorMessage("Use .scaling simple, .scaling advanced or .scaling off.");
            return false;
        }

        sDynamicScaling->SetPlayerChoice(player, mode);
        handler->PSendSysMessage("Your scaling is now {}. Nearby creatures update within a few seconds once out of "
            "combat.", DynamicScalingMgr::ModeName(mode));
        return true;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sDynamicScaling->LoadTables();
        sDynamicScaling->ApplyEntryLevelOverrides();
        sDynamicScaling->LoadCollective();
        handler->PSendSysMessage("Dynamic Scaling: reloaded {} map rows and {} zone/area rows. Creatures rescale "
            "the next time they are out of combat.", sDynamicScaling->InstanceRowCount(),
            sDynamicScaling->ZoneRowCount());
        return true;
    }

    static bool HandleRescaleCommand(ChatHandler* handler)
    {
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->SendErrorMessage("Select a creature first.");
            return false;
        }

        DSCreatureInfo* info = creature->CustomData.GetDefault<DSCreatureInfo>("DynamicScalingCreatureInfo");
        info->updateTimer = 0;
        info->cooldown = 0;
        info->mapVersion = 0;
        info->settingsVersion = 0;
        handler->PSendSysMessage("{} will rescale on its next update (it must be out of combat).", creature->GetName());
        return true;
    }
};

void AddDynamicScalingCommandScripts()
{
    new DynamicScalingCommandScript();
}
