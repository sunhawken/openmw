-- Shared by MENU and PLAYER: register before the stock binding renderer/player
-- resolver inspects these IDs. Action/trigger registries are shared globally.
local input=require('openmw.input')
local storage=require('openmw.storage')
local S=require('scripts.brutal_morrowind.settings')
local KICK,SPRINT='BrutalKick','BrutalSprint'
if not input.triggers[KICK] then
    input.registerTrigger{key=KICK,l10n='BrutalMorrowind',name='kickBinding',description='kickBindingDescription'}
end
if not input.actions[SPRINT] then
    input.registerAction{key=SPRINT,type=input.ACTION_TYPE.Boolean,defaultValue=false,
        l10n='BrutalMorrowind',name='sprintBinding',description='sprintBindingDescription'}
end
local bindings=storage.playerSection('OMWInputBindings')
local initialized=storage.playerSection('BrutalMorrowindBindingDefaults')
bindings:setLifeTime(storage.LIFE_TIME.Persistent)
initialized:setLifeTime(storage.LIFE_TIME.Persistent)
local function default(id,kind,key,button)
    -- A separate marker distinguishes first installation from an intentionally
    -- cleared binding. Never restore a default after Escape/unbind or reload.
    if initialized:get(id) then return end
    if bindings:get(id)==nil then
        bindings:set(id,{device='keyboard',button=button,type=kind,key=key})
    end
    initialized:set(id,true)
end
default(S.DEFAULTS.kickBinding,'trigger',KICK,input.KEY.V)
default(S.DEFAULTS.sprintBinding,'action',SPRINT,input.KEY.LeftShift)
return {KICK=KICK,SPRINT=SPRINT}
