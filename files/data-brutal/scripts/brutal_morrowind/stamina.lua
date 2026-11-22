local settings=require('scripts.brutal_morrowind.settings')
-- Dark Messiah gait and stamina for the player. Default is the run, Alt walks and
-- The assigned sprint button sprints while stamina allows it. The engine owns the speeds; regeneration is
-- the engine's `player fatigue regen fraction` (0.06 per 0.12 s of 9 = 1/18 per second).
local self = require('openmw.self')
local types = require('openmw.types')
local input = require('openmw.input')
require('scripts.brutal_morrowind.input_bindings')
local I = require('openmw.interfaces')
local C = require('scripts.brutal_morrowind.config')

local setGait = types.Actor.setKineticGait
local WALK, RUN, SPRINT = 1, 2, 3
local gait, sprinting, shiftWasDown = nil, false, false
local scripted -- when set, {shift=, alt=} replaces the keyboard state

local function fatigueFraction()
    local fatigue = types.Actor.stats.dynamic.fatigue(self)
    return fatigue.base > 0 and fatigue.current / fatigue.base or 0, fatigue
end

local function update(dt)
    if not setGait then return end
    if not settings.get('movementEnabled') then
        if gait~=nil then setGait(self,0) end
        gait,sprinting,shiftWasDown=nil,false,false
        return
    end
    local shift, alt = false, false
    if scripted then
        shift, alt = scripted.shift == true, scripted.alt == true
    elseif not I.UI.getMode() then
        shift, alt = input.getBooleanActionValue('BrutalSprint'), input.isAltPressed()
    end
    if I.BrutalClinch and I.BrutalClinch.status().active then shift,alt=false,false end
    local forward = self.controls.movement > 0.3 and math.abs(self.controls.sideMovement) <= 0.3
    local fraction, fatigue = fatigueFraction()
    local needed = C.sprintStaminaNeeded / C.staminaMax
    -- The reserve is needed only to start. An active sprint can use all remaining
    -- stamina; once exhausted, the sprint button must be released to start again.
    if shift and not shiftWasDown and (not settings.get('staminaEnabled') or fraction >= needed) then sprinting = true end
    if not shift or (settings.get('staminaEnabled') and fraction <= 0) or types.Actor.isSwimming(self) then sprinting = false end
    shiftWasDown = shift
    if settings.get('staminaEnabled') and sprinting and forward and dt > 0 then
        fatigue.current = math.max(0, fatigue.current
            - fatigue.base * C.sprintStaminaPerSecond / C.staminaMax * dt)
        -- Stop on the depleted frame, before regeneration can hide zero next frame.
        if fatigue.current <= 0 then sprinting = false end
    end
    local wanted = sprinting and SPRINT or (alt and WALK or RUN)
    if wanted ~= gait then
        gait = wanted
        setGait(self, gait)
    end
end

return {
    interfaceName = 'BrutalStamina',
    interface = {
        version = 1,
        status = function()
            return {gait = gait, sprinting = sprinting, fraction = (fatigueFraction())}
        end,
        setScriptedInput = function(keys) scripted = keys end,
    },
    engineHandlers = {
        onUpdate = update,
        -- The engine does not save the gait; choose it again on the next update.
        onLoad = function() gait, sprinting, shiftWasDown = nil, false, false end,
    },
}
