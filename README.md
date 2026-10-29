# mod-dynamic-scaling

One AzerothCore module for scaling, with a **Simple** and an **Advanced** setting wherever it applies.

| Where | Simple | Advanced |
|---|---|---|
| Instances (dungeons, raids) | Players get stats and spell power for every missing player (SoloCraft style) | Creatures get less health, damage and mana for fewer players (AutoBalance style) |
| Open world (each player picks) | Creatures trail behind your level anywhere, 3 levels by default (Ascension style) | Creatures trail behind you inside each zone's level window, with per row trail, health, damage and XP floor |

Extras:

- **Level scaling in instances**: creature levels move with the group, inside a level window per instance.
  Every creature moves by the same amount, so bosses stay above their trash.
- **Enter at any level**: per instance `entry_level_min` replaces the core level requirement
  (1 = any level).
- **Dungeon respawn** (DungeonRespawn style): release your spirit inside an instance and you come back alive
  at the instance entrance. The entrance comes from `areatrigger_teleport`, so no coordinates need typing.
  A map row can set manual coordinates instead.
- **XP always awarded**: gray kills and low quests still give some XP in scaled zones.
- **Player choice**: each player picks `.scaling simple`, `.scaling advanced` or `.scaling off` for the open world;
  the server decides if they may and the default (Simple). With scaling off, creatures keep their levels, and those
  5 or more levels below you give no quest progress or quest items (`World.Off.NoQuestCreditLevels`).
- **Quest level requirements**: by a quest giver in a scaled place, Simple players can take quests at any level and
  Advanced players from the zone's `level_min`. This uses the `OnPlayerGetQuestMinLevel` core hook when CMake finds
  it (`mod-dynamic-scaling.cmake`); without it, quests of scaled zones are lowered in memory for everyone at startup,
  following `PlayerToggle.Default`.
- **Rewards follow the scaled level**: within a margin (3 levels by default) rewards stay normal. Beyond it, quests
  far above you give the XP and money of a quest at the margin, a creature's gold follows its scaled level, its
  level range loot tables ("Vanilla Greens 8-12 Level Range") swap to those of its scaled level, and a creature
  scaled down drops no items far above it.
- **No level up effect**: a creature changing level is sent again to whoever sees it, instead of "dinging"
  (`World.SilentLevelChange`).

## With mod-world-randomizer (collective)

When both modules run and both collective switches are on (`DynamicScaling.Collective.WorldRandomizer` here,
`Randomizer.Collective.DynamicScaling` there, both on by default), each module reads the other's settings from the
config, and they split the work:

- the randomizer writes each dungeon's bracket (from its entrance zone) to `world_randomizer_dungeon_bracket` at
  startup and stops shifting dungeon levels itself,
- this module reads it at startup and scales those dungeons in Simple mode, to the group's level but inside that
  bracket: Ragefire Chasm in a 31-40 bracket scales between 31 and 40,
- open world scaling here is off, since the randomizer sets zone levels.

Turn either switch off and the two modules ignore each other.

## Where settings come from

The most specific row wins:

1. `dynamic_scaling_zone` row for the **area** (for example Northshire Valley)
2. `dynamic_scaling_zone` row for the **zone** (for example Elwynn Forest)
3. `dynamic_scaling_map` row for the **map**: an instance (per difficulty, or 255 for all) or a whole continent
4. The defaults in `mod-dynamic-scaling.conf`

### Level windows

`level_min` / `level_max` work the same in both tables:

| Value | Meaning |
|---|---|
| `N` | Creatures never go below / above level N |
| `0` | Creatures never go below / above their own level |
| `0` and `0` | Creatures keep their own level (cities, starting areas) |

So `level_min = 0, level_max = 80` means "only scale up", and `level_min = 1, level_max = 0` means "only scale down".

## Tables

- `dynamic_scaling_map` (world DB): per instance + difficulty, or per continent. Instances with no row use the
  config defaults, which scale every instance from level 1 to 80 in Simple mode.
- `dynamic_scaling_zone` (world DB): per zone or area. Ships with every outdoor zone and its level range taken
  from your aowow (`Stuff/zone-extract`), with cities, starting areas, Wintergrasp and the Scarlet Enclave at `0/0`.
- `dynamic_scaling_creature` (world DB): creature entries that always (`scale` 1) or never (0) scale in the
  open world, over every category rule.
- `dynamic_scaling_category` (world DB): which kinds of creature scale, used instead of `World.NeverScale` when
  `World.UseDBOverride = 1`.
- `dynamic_scaling_player` (characters DB): each player's `.scaling` choice (`enabled`: 0 off, 1 Simple, 2 Advanced).
- `spell_dbc` 150100: the server side aura that carries the Simple mode stat buff.

The SQL in `data/sql` is applied by the core's DB updater (or `dbimport`) like any module.

## Commands

| Command | Who | What |
|---|---|---|
| `.scaling [simple\|advanced\|off]` | players | Show or change your own open world scaling |
| `.dscale info` | GM | Settings for your map / zone, and for the selected creature |
| `.dscale reload` | admin | Reload both tables; creatures rescale once out of combat |
| `.dscale rescale` | GM | Rescale the selected creature on its next update |
| `.dscale lootswap <reference> [level]` | GM | Which level range loot table a scaled creature rolls instead |

Config changes need `.reload config`.

## Curve worksheet

`tools/DynamicScaling_Curve_Worksheet.xlsx` (like AutoBalance's Curve Worksheet): change the yellow cells to see
the Advanced instance multipliers for every group size (curve or straight line, bosses too), the Simple buff, and
open world creature levels and rewards for every player level. It uses the same math as the module.

## The addon

`addon/DynamicScaling` is a client addon (3.3.5): copy it to `Interface/AddOns`. On login it asks which leveling
experience you want (No Scaling, Simple or Advanced), with "Do not ask again on next login" (this character) and
"Do not ask on new characters" (every character). `/dscale` opens a window with your choice and the creatures
around you: their own level, and what they would be for you in Simple and Advanced. It talks to the server with
addon whispers (prefix `DSCALE`, see `src/DynamicScalingAddon.cpp`).

## How it works

- Creature levels go through the core's `SelectLevel()`. The module only picks the level, so health, mana,
  damage, attack power and armor come from `creature_classlevelstats` exactly like an unscaled creature of that
  level. Multipliers go on top.
- Instance creatures rescale (out of combat) when players join, leave or level up.
- Open world creatures check for players nearby every `World.UpdateInterval` ms and only change while out of combat,
  at most once per `World.RescaleCooldown`. Creatures with NPC flags, civilians, critters, triggers and
  unattackable creatures are left alone.
- A creature's level is shared by everyone who sees it (3.3.5 clients get one level per creature), so the
  reference player is the nearest one by default, or the group average, and that player's choice (Simple or
  Advanced) decides how it scales.
- Creatures look for players within `World.Range` yards; 0 (the default) means the map's visibility distance, so
  every creature you can see scales to you.
- In a zone row, `mode` 0 or a `0/0` window means "never scaled"; any other row is scaled the way the player chose.
  Simple ignores the window; Advanced uses it and the row's other fields.

## Uninstall

`data/sql/uninstall/` has a `world.sql` and a `characters.sql` that take the module back out: they drop its tables,
remove its custom spells (and any saved auras of them) and its rows in `updates`, so a later reinstall starts clean.
Creature levels and stats, instance entry levels and respawn points are only changed in memory, so they are the core's
again once the module is gone.

1. Take the module out of the build (move it out of `modules/`, or add it to `-DDISABLED_AC_MODULES`) and rebuild.
   With the module still built, the DB updater installs everything again on the next start.
2. Run `world.sql` on the world database and `characters.sql` on the characters database.

## Known limits

- Scaling works however you enter (walking in or the Dungeon Finder). `entry_level_min` also unlocks a dungeon in
  the Dungeon Finder (`Instance.LFG.EntryLevel`), but the random dungeon pool still uses `LFGDungeons.dbc`'s level
  ranges, and the client may still list a dungeon as too high until the server's unlock reaches it.
- `entry_level_min` edits the access requirements the core loaded. A core `.reload` of those requirements
  undoes it until `.dscale reload`.
- Gray kill XP goes to the killer and nearby group members only, split evenly.

## Credits

Ideas and mechanisms from:

- [mod-solocraft](https://github.com/azerothcore/mod-solocraft): the per-missing-player stat buff and the
  "flat spell power only" rule.
- [mod-autobalance](https://github.com/azerothcore/mod-autobalance): scaling creatures through the
  `SelectLevel` hooks and the damage hooks.
- [DungeonRespawn](https://github.com/AnchyDev/DungeonRespawn) by AnchyDev: respawning inside the instance
  after releasing.
- TrinityCore's level scaling (`Creature::GetLevelForTarget` / content tuning) was read for comparison; it
  needs per-player levels that a 3.3.5 client can't show, so this module uses one level per creature instead.
