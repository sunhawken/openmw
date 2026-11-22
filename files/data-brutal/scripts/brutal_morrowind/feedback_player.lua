local settings=require('scripts.brutal_morrowind.settings')
local ambient = require('openmw.ambient')
local vfs = require('openmw.vfs')
local self = require('openmw.self')
local types = require('openmw.types')
local I = require('openmw.interfaces')
local loaded, assets = pcall(require, 'scripts.brutal_assets.effects')
local played={}
local function cue(category, volume, pitch)
    if not settings.get('combatSounds') then return end
    local entry = loaded and assets.sounds[category]
    if not entry or not entry.files or #entry.files == 0 then return end
    local file = entry.files[math.random(#entry.files)]
    if vfs.fileExists(file) then
        ambient.playSoundFile(file, {volume = volume, pitch=pitch or 1, scale = false})
        played[category]=(played[category] or 0)+1
    end
end
return {interfaceName='BrutalPlayerFeedback',interface={count=function(category)return played[category] or 0 end},eventHandlers = {
    -- Keep the boot impact audible at its original duration in bullet
    -- time, just as the kick animation itself runs at real-time speed.
    BrutalRagdollKickLanded = function() cue('kick_hit', 0.4) end,
    BrutalAdrenalineReady = function() cue('adrenaline_ready', 0.6) end,
    BrutalAdrenalineSpent = function() cue('critical_voice', 0.5,math.random(90,105)/100) end,
    BrutalAdrenalineSlowMotionStarted = function()
        if not I.UI.getMode() and not types.Actor.isDead(self)
            and types.Player.getControlSwitch(self,types.Player.CONTROL_SWITCH.Fighting) then
            cue('slow_motion_in',0.4)
        end
    end,
    BrutalAdrenalineSlowMotionEnded = function(e)
        if e.reason=='expired' then cue('slow_motion_out',0.2) end
    end,
}}
