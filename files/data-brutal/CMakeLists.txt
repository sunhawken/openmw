if (NOT DEFINED OPENMW_RESOURCES_ROOT)
    return()
endif()

# The Brutal Morrowind scripts, translations, post-processing shaders and
# textures. They go into vfs-mw next to OpenMW's own game scripts, and their
# script list is appended to builtin.omwscripts (see files/data-mw), so the
# mod is always loaded. Each feature has its own switch in the mod settings.
set(BRUTAL_DATA_FILES
    l10n/BrutalMorrowind/en.yaml
    l10n/BrutalMorrowind/ru.yaml
    scripts/brutal_morrowind/actor.lua
    scripts/brutal_morrowind/adrenaline.lua
    scripts/brutal_morrowind/adrenaline_fx.lua
    scripts/brutal_morrowind/adrenaline_time.lua
    scripts/brutal_morrowind/adrenaline_ui.lua
    scripts/brutal_morrowind/clinch.lua
    scripts/brutal_morrowind/clinch_fx.lua
    scripts/brutal_morrowind/combat_effects.lua
    scripts/brutal_morrowind/config.lua
    scripts/brutal_morrowind/decapitation.lua
    scripts/brutal_morrowind/effects_global.lua
    scripts/brutal_morrowind/feedback_player.lua
    scripts/brutal_morrowind/global.lua
    scripts/brutal_morrowind/input_bindings.lua
    scripts/brutal_morrowind/kick_global.lua
    scripts/brutal_morrowind/native_impulse.lua
    scripts/brutal_morrowind/player.lua
    scripts/brutal_morrowind/ragdoll_global.lua
    scripts/brutal_morrowind/ragdoll_local.lua
    scripts/brutal_morrowind/settings.lua
    scripts/brutal_morrowind/settings_menu.lua
    scripts/brutal_morrowind/settings_player.lua
    scripts/brutal_morrowind/slide.lua
    scripts/brutal_morrowind/slide_speed_fx.lua
    scripts/brutal_morrowind/stamina.lua
    shaders/brutal_adrenaline.omwfx
    shaders/brutal_clinch_blur.omwfx
    shaders/brutal_slide_speed.omwfx
)

foreach (f ${BRUTAL_DATA_FILES})
    copy_resource_file("${CMAKE_CURRENT_SOURCE_DIR}/${f}" "${OPENMW_RESOURCES_ROOT}" "resources/vfs-mw/${f}")
endforeach (f)
