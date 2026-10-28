# mod-dynamic-scaling: is the OnPlayerGetQuestMinLevel hook in this core? The answer goes into a generated header,
# DynamicScalingCoreHooks.h. Without the hook, quest levels are lowered for everyone at startup instead.
file(READ "${CMAKE_SOURCE_DIR}/src/server/game/Scripting/ScriptDefines/PlayerScript.h" DS_PLAYER_SCRIPT_H)
string(FIND "${DS_PLAYER_SCRIPT_H}" "OnPlayerGetQuestMinLevel" DS_QUEST_HOOK_POS)
if(DS_QUEST_HOOK_POS EQUAL -1)
  set(DS_HAS_QUEST_MIN_LEVEL_HOOK 0)
  message(STATUS "mod-dynamic-scaling: no OnPlayerGetQuestMinLevel hook, quest levels are lowered for everyone")
else()
  set(DS_HAS_QUEST_MIN_LEVEL_HOOK 1)
  message(STATUS "mod-dynamic-scaling: OnPlayerGetQuestMinLevel hook found, quest levels follow each player")
endif()

set(DS_GENERATED_DIR "${CMAKE_BINARY_DIR}/modules/mod-dynamic-scaling")
file(WRITE "${DS_GENERATED_DIR}/DynamicScalingCoreHooks.h.tmp"
  "#define DS_HAS_QUEST_MIN_LEVEL_HOOK ${DS_HAS_QUEST_MIN_LEVEL_HOOK}\n")
configure_file("${DS_GENERATED_DIR}/DynamicScalingCoreHooks.h.tmp" "${DS_GENERATED_DIR}/DynamicScalingCoreHooks.h"
  COPYONLY)

string(TOLOWER "mod_${SOURCE_MODULE}" DS_DYNAMIC_TARGET)
foreach(DS_TARGET modules ${DS_DYNAMIC_TARGET})
  if(TARGET ${DS_TARGET})
    target_include_directories(${DS_TARGET} PRIVATE "${DS_GENERATED_DIR}")
  endif()
endforeach()
