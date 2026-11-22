-- Authored screen-space streaks; this module only observes slide movement.
local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local I = require('openmw.interfaces')
local postprocessing = require('openmw.postprocessing')
local shader, checked, warmed, warmFrames = nil, false, false, nil
local alpha, clock, speed, phase = 0, 0, 0, 0
local previousRealTime, previousSimulationTime, previousPosition, previousCell, sampleAge

local function clamp(value, low, high)
    return math.max(low, math.min(high, value))
end

local function setEnabled(enabled)
    -- Re-enabling a current member rebuilds OpenMW's postprocess render targets.
    if shader and shader:isEnabled() ~= enabled then
        if enabled then shader:enable() else shader:disable() end
    end
end

local function resetSamples()
    previousSimulationTime, previousPosition, previousCell = nil, nil, nil
    sampleAge = 0
    speed, phase = 0, 0
end

local function destroy()
    if shader and alpha ~= 0 then shader:setFloat('brutalSlideAlpha', 0) end
    alpha, clock, previousRealTime = 0, 0, nil
    warmFrames = nil
    resetSamples()
end

local function visible()
    return not types.Actor.isDead(self) and types.Actor.canMove(self)
        and not I.UI.getMode() and I.UI.isHudVisible()
        and types.Player.getControlSwitch(self, types.Player.CONTROL_SWITCH.Controls)
        and types.Player.getControlSwitch(self, types.Player.CONTROL_SWITCH.Fighting)
end

local function sampleMovement(state, simulationTime, realDt)
    local position = self.position
    local nextPhase = state and state.phase or 0
    local simDt = previousSimulationTime and simulationTime - previousSimulationTime or 0
    sampleAge = (sampleAge or 0) + realDt
    if nextPhase == 0 then
        speed = 0
    elseif previousPosition and previousCell == self.cell and simDt > 0 and simDt <= 0.25
        and sampleAge >= 0.035 then
        local measured = (position - previousPosition):length() / simDt
        -- Ignore teleports and cell changes. Account for downhill travel in 3D.
        if measured == measured and measured < 1600 then
            speed = speed + (measured - speed) * (1 - math.exp(-sampleAge / 0.065))
        else
            speed = 0
        end
    elseif simDt < 0 or simDt > 0.25 or previousCell ~= self.cell then
        speed = 0
    end
    -- Average over a few render frames so fixed physics steps at a high render
    -- rate cannot make an unmoving/moving alternation flash the screen effect.
    if nextPhase == 0 or not previousPosition or sampleAge >= 0.035
        or previousCell ~= self.cell or simDt < 0 or simDt > 0.25 then
        previousPosition, previousCell, previousSimulationTime = position, self.cell, simulationTime
        sampleAge = 0
    end
    phase = nextPhase
end

local function draw(state)
    local now = core.getRealTime()
    local rawDt = previousRealTime and now - previousRealTime or 0
    previousRealTime = now
    if not visible() then
        destroy()
        return
    end
    -- A resumed frame must not replay an old slide's screen flash.
    if rawDt < 0 or rawDt > 0.5 then
        if shader and alpha ~= 0 then shader:setFloat('brutalSlideAlpha', 0) end
        alpha = 0
        resetSamples()
        rawDt = 0
    end
    local dt = math.max(rawDt, 0)
    sampleMovement(state, core.getSimulationTime(), dt)
    if not checked then
        checked = true
        shader = postprocessing.load('brutal_slide_speed')
    end
    if not shader then return end
    if not warmed then
        -- Identity warmup avoids compiling the technique on the first slide.
        shader:setFloat('brutalSlideAlpha', 0)
        shader:setFloat('brutalSlideClock', 0)
        setEnabled(true)
        warmFrames = (warmFrames or 0) + 1
        if warmFrames >= 4 then
            warmed, warmFrames = true, nil
        end
        return
    end
    local motion = clamp((speed - 25) / 545, 0, 1)
    local blend = state and clamp(state.blend or 0, 0, 1) or 0
    local weight = phase == 1 and (0.4 + 0.6 * math.sqrt(blend))
        or phase == 2 and 1 or phase == 3 and math.sqrt(blend) * 0.7 or 0
    local target = weight * math.sqrt(motion)
    local previousAlpha = alpha
    alpha = alpha + clamp(target - alpha, -dt / 0.18, dt / 0.11)
    -- A real-time phase stays fluid during adrenaline's 0.1 simulation scale.
    -- Both shader layers repeat exactly at 1024, avoiding long-session drift.
    clock = (clock + dt * (1.1 + motion * 0.6)) % 1024
    if alpha > 0 or previousAlpha > 0 then
        shader:setFloat('brutalSlideClock', clock)
        shader:setFloat('brutalSlideAlpha', alpha)
    end
    -- Retain the warmed pass, which is an exact identity at alpha zero. Native
    -- chain toggles rebuild render targets and caused a measured 77--90ms hitch
    -- at slide entry, even after precompiling this same shader during setup.
    setEnabled(true)
end

return {
    draw = draw,
    destroy = destroy,
    status = function()
        return {visible=shader and shader:isEnabled() and alpha > 0.001 or false,
            alpha=alpha, clock=clock, speed=speed, phase=phase, warmed=warmed}
    end,
}
