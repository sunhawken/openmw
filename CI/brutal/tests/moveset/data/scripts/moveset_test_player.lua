-- In-engine test for the move set engine layer, with a synthetic move set
-- that uses only stock Morrowind animation groups, sounds and effects.
local self = require('openmw.self')
local types = require('openmw.types')
local core = require('openmw.core')
local I = require('openmw.interfaces')

local seen = {}
local missingGroup = false
local target, healthBefore, startPos
local phase, timer = nil, 0

local SET = {
    id = 'synthetic_test',
    bufferTime = 0.4,
    start = { light = 't1' },
    moves = {
        t1 = {
            anim = 'handtohand', startKey = 'chop start', stopKey = 'chop small follow stop',
            duration = 0.8, cancel = { 0.3, 0.6 }, next = { light = 't2' },
            events = {
                { t = 0.05, type = 'sound', id = 'Swish' },
                { t = 0.1, type = 'effect', model = 'meshes/e/magic_hit_dst.nif' },
                { t = 0.2, type = 'hit', piece = 'right', from = 0.2, to = 0.3, damage = 7, reach = 250, cone = 0.3 },
                { t = 0.25, type = 'custom_marker', value = 42 },
                { t = 0.32, type = 'move', forward = 300 },
            },
        },
        t2 = {
            anim = 'handtohand', startKey = 'thrust start', stopKey = 'thrust small follow stop',
            duration = 0.5,
            events = { { t = 0.1, type = 'hit', piece = 'left', from = 0.1, to = 0.2, damage = 5, reach = 250, cone = 0.3 } },
        },
    },
}

local function check(ok, what)
    print('[moveset-test] ' .. (ok and 'PASS ' or 'FAIL ') .. what)
end

return {
    eventHandlers = {
        MovesetTestBegin = function()
            local bad = I.BrutalMoveset.register({ id = 'broken', start = { light = 'missing' }, moves = {} })
            check(bad == false, 'invalid move set rejected')
            check(I.BrutalMoveset.register(SET) == true, 'synthetic move set registered')
            check(I.BrutalMoveset.activate('synthetic_test') == true, 'move set activated')
            I.BrutalMoveset.onEvent(function(e)
                seen[e.type] = (seen[e.type] or 0) + 1
                if e.type == 'custom_marker' then seen.markerValue = e.value end
                if e.type == 'missingAnimation' then missingGroup = true end
            end)
            core.sendGlobalEvent('MovesetTestSpawnTarget', {})
        end,
        MovesetTestTarget = function(e)
            target = e.target
            phase, timer = 'settle', 0
        end,
    },
    engineHandlers = {
        onUpdate = function(dt)
            if not phase then return end
            timer = timer + dt
            if phase == 'settle' and timer > 2 then
                healthBefore = types.Actor.stats.dynamic.health(target).current
                startPos = self.position
                check(I.BrutalMoveset.press('light') == true, 'light press accepted')
                check(I.BrutalMoveset.state().move == 't1', 'first move t1 running')
                phase, timer = 'first', 0
            elseif phase == 'first' and timer > 0.35 then
                I.BrutalMoveset.press('light')
                phase, timer = 'second', 0
            elseif phase == 'second' and timer > 1.5 then
                local healthAfter = types.Actor.stats.dynamic.health(target).current
                check((seen.start or 0) >= 2, 'two moves started (chain)')
                check((seen.sound or 0) >= 1, 'sound event fired')
                check((seen.effect or 0) >= 1, 'effect event fired')
                check((seen.move or 0) >= 1, 'movement event fired')
                check((seen.struck or 0) >= 2, 'both hit windows struck the target')
                check(not missingGroup, 'animation groups found and played')
                check(seen.markerValue == 42, 'custom event passed to listeners with its data')
                check(healthAfter < healthBefore, string.format('target health %.1f -> %.1f', healthBefore, healthAfter))
                check((self.position - startPos):length() > 1, 'player moved by the movement event')
                check(I.BrutalMoveset.state().move == nil, 'back to neutral after the combo')
                print('[moveset-test] finished')
                phase = nil
                core.quit()
            end
        end,
    },
}
