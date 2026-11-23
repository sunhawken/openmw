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

-- Regression: a 1 ms hit window, narrower than any frame, must still strike.
local NARROW = {
    id = 'narrow_window',
    start = { light = 'n1' },
    moves = { n1 = { duration = 0.3, events = {
        { t = 0.1, type = 'hit', piece = 'p', from = 0.1, to = 0.101, damage = 3, reach = 250, cone = 0.3 },
        -- Same time and piece as above: a separate window, not a duplicate key.
        { t = 0.1, type = 'hit', piece = 'p', from = 0.1, to = 0.101, damage = 3, reach = 250, cone = 0.3 },
    } } },
}
-- Regression: a target behind a wall must not be struck.
local BLOCKED = {
    id = 'occlusion',
    start = { light = 'o1' },
    moves = { o1 = { duration = 0.4, events = {
        { t = 0.1, type = 'hit', piece = 'p', from = 0.1, to = 0.3, damage = 3, reach = 600, cone = 0.3 },
    } } },
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
        MovesetTestTargetMoved = function() phase, timer = 'moved', 0 end,
        MovesetTestWallPlaced = function() phase, timer = 'walled', 0 end,
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
                I.BrutalMoveset.register(NARROW)
                I.BrutalMoveset.activate('narrow_window')
                seen.struck = 0
                I.BrutalMoveset.press('light')
                phase, timer = 'narrow', 0
            elseif phase == 'narrow' and timer > 0.8 then
                check((seen.struck or 0) == 2, 'two 1 ms same-time windows on one piece each struck once (got '
                    .. tostring(seen.struck) .. ')')
                core.sendGlobalEvent('MovesetTestMoveTarget', { target = target, distance = 400 })
                phase, timer = 'wait_move', 0
            elseif phase == 'moved' and timer > 0.5 then
                core.sendGlobalEvent('MovesetTestWall', { target = target })
                phase, timer = 'wait_wall', 0
            elseif phase == 'walled' and timer > 1 then
                local nearby = require('openmw.nearby')
                local util = require('openmw.util')
                local ray = nearby.castRay(self.position + util.vector3(0, 0, 60),
                    target.position + util.vector3(0, 0, 60), { ignore = self })
                check(ray.hit and ray.hitObject ~= target, 'precondition: wall blocks the line of sight (hit '
                    .. tostring(ray.hitObject and ray.hitObject.recordId) .. ')')
                I.BrutalMoveset.register(BLOCKED)
                I.BrutalMoveset.activate('occlusion')
                seen.struck = 0
                I.BrutalMoveset.press('light')
                phase, timer = 'blocked', 0
            elseif phase == 'blocked' and timer > 0.8 then
                check((seen.struck or 0) == 0, 'no strike through the wall (got ' .. tostring(seen.struck) .. ')')
                print('[moveset-test] finished')
                phase = nil
                core.quit()
            end
        end,
    },
}
