-- Engine layer for data-driven move sets (see moveset/core.lua for the data
-- format). Feeds input and time to the core and performs what it returns:
--   start/end -> animation playback and cancel
--   sound     -> core.sound.playSound3d       { id, volume?, pitch? }
--   effect    -> animation.addVfx             { model, bone?, loop?, vfxId? }
--   move      -> velocity impulse             { forward?, right?, up? } (units/s)
--   hit       -> Hit event on a target found in the window  { piece?, damage?, fatigue?, reach?, cone?, push? }
-- Other event types are passed to listeners registered with onEvent().
--
-- Content registers move sets through the BrutalMoveset interface:
--   I.BrutalMoveset.register(set)   -- validates; returns true or false, errors
--   I.BrutalMoveset.activate(id)    -- nil deactivates
--   I.BrutalMoveset.press(input)    -- same path as a bound key
local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local nearby = require('openmw.nearby')
local input = require('openmw.input')
local async = require('openmw.async')
local animation = require('openmw.animation')
local storage = require('openmw.storage')
local ui = require('openmw.ui')
local I = require('openmw.interfaces')

local Core = require('scripts.brutal_morrowind.moveset.core')
local Input = require('scripts.brutal_morrowind.moveset.input')

local settings = storage.playerSection('SettingsBrutalMoveset')

local impulseOk, impulse = pcall(require, 'scripts.brutal_morrowind.native_impulse')
if not impulseOk then impulse = nil end

local sets = {} -- id -> definition
local runners = {} -- id -> runner
local savedRunners = {} -- id -> save data waiting for its set to register
local activeId = nil
local pendingActive = nil -- active set from a save, waiting for its registration
local playing = nil -- animation group of the current move
local hitWindows = {} -- key -> true once that window has struck a target
local listeners = {}
local hud = nil
local log = { count = 0 }

local function enabled()
    return settings:get('enabled') ~= false
end

local function say(text)
    print('[BrutalMoveset] ' .. text)
end

-- Condition and cost hooks for the core.
local function makeHooks()
    return {
        check = function(req)
            local stance = types.Actor.getStance(self)
            if req.grounded and not types.Actor.isOnGround(self) then return false end
            if req.airborne and types.Actor.isOnGround(self) then return false end
            if req.weaponDrawn and stance ~= types.Actor.STANCE.Weapon then return false end
            if req.sheathed and stance ~= types.Actor.STANCE.Nothing then return false end
            return true
        end,
        canAfford = function(cost)
            local dyn = types.Actor.stats.dynamic
            if cost.fatigue and dyn.fatigue(self).current < cost.fatigue then return false end
            if cost.magicka and dyn.magicka(self).current < cost.magicka then return false end
            if cost.health and dyn.health(self).current <= cost.health then return false end
            return true
        end,
        spend = function(cost)
            local dyn = types.Actor.stats.dynamic
            if cost.fatigue then dyn.fatigue(self).current = dyn.fatigue(self).current - cost.fatigue end
            if cost.magicka then dyn.magicka(self).current = dyn.magicka(self).current - cost.magicka end
            if cost.health then dyn.health(self).current = dyn.health(self).current - cost.health end
        end,
    }
end

local function actorFrame()
    local rot = self.rotation
    local forward = rot:apply(util.vector3(0, 1, 0))
    local right = rot:apply(util.vector3(1, 0, 0))
    return forward, right
end

local function findTarget(reach, cone)
    local origin = self.position + util.vector3(0, 0, 60)
    local forward = actorFrame()
    local flatForward = util.vector3(forward.x, forward.y, 0):normalize()
    local best, bestDist = nil, reach
    for _, actor in ipairs(nearby.actors) do
        if actor ~= self.object and not types.Actor.isDead(actor) then
            local to = actor.position - self.position
            local flat = util.vector3(to.x, to.y, 0)
            local dist = flat:length()
            if dist < bestDist and dist > 1 and (flat / dist):dot(flatForward) >= cone then
                local ray = nearby.castRay(origin, actor.position + util.vector3(0, 0, 60), { ignore = self })
                -- Anything the ray hits first (terrain, statics with no object,
                -- other objects) blocks the strike; only the target itself may.
                if not ray.hit or ray.hitObject == actor then
                    best, bestDist = actor, dist
                end
            end
        end
    end
    return best
end

local function notify(e)
    for _, fn in ipairs(listeners) do
        local ok, err = pcall(fn, e)
        if not ok then say('listener error: ' .. tostring(err)) end
    end
end

local function playMove(e)
    local move = sets[activeId].moves[e.move]
    if not move.anim then return end
    if not animation.hasGroup(self, move.anim) then
        say('move ' .. e.move .. ': no animation group "' .. move.anim .. '"')
        notify({ type = 'missingAnimation', move = e.move, anim = move.anim })
        return
    end
    I.AnimationController.playBlendedAnimation(move.anim, {
        priority = animation.PRIORITY.Scripted,
        blendMask = move.blendMask or animation.BLEND_MASK.All,
        startKey = move.startKey or 'start',
        stopKey = move.stopKey or 'stop',
        speed = e.speed,
        autoDisable = true,
        loops = 0,
    })
    playing = move.anim
end

local function stopMove()
    if playing and animation.getCompletion(self, playing) ~= nil then
        animation.cancel(self, playing)
    end
    playing = nil
end

local moveSerial = 0 -- counts started moves, so a repeated move gets fresh windows

local function strike(e)
    local key = moveSerial .. '#' .. tostring(e.ordinal)
    if hitWindows[key] then return end
    local target = findTarget(e.reach or 160, e.cone or 0.6)
    if not target then return end
    hitWindows[key] = true
    target:sendEvent('Hit', {
        attacker = self, successful = true, sourceType = 'melee', strength = 1,
        brutalMoveset = true, movesetMove = e.move, movesetPiece = e.piece,
        damage = { health = e.damage or 0, fatigue = e.fatigue or 0 },
        hitPos = target.position + util.vector3(0, 0, 60),
    })
    if e.push and e.push > 0 then
        local forward = actorFrame()
        target:sendEvent('BrutalImpulse', { attacker = self, direction = forward, kick = false, speed = e.push })
    end
    say('hit ' .. tostring(target.recordId) .. ' with ' .. e.move .. ' piece ' .. tostring(e.piece))
    notify({ type = 'struck', move = e.move, piece = e.piece, target = target })
end

local function dispatch(events)
    for _, e in ipairs(events) do
        log.count = log.count + 1
        if e.type == 'start' then
            hitWindows = {}
            moveSerial = moveSerial + 1
            playMove(e)
            say('start ' .. e.move .. ' chain ' .. e.chain)
        elseif e.type == 'end' then
            if e.reason ~= 'finished' then stopMove() else playing = nil end
            say('end ' .. e.move .. ' (' .. e.reason .. ')')
        elseif e.type == 'sound' then
            if e.id then core.sound.playSound3d(e.id, self, { volume = e.volume or 1, pitch = e.pitch or 1 }) end
            say('sound ' .. tostring(e.id))
        elseif e.type == 'effect' then
            if e.model then
                animation.addVfx(self, e.model, { boneName = e.bone, loop = e.loop == true, vfxId = e.vfxId })
            end
            say('effect ' .. tostring(e.model))
        elseif e.type == 'move' then
            if impulse then
                local forward, right = actorFrame()
                local v = forward * (e.forward or 0) + right * (e.right or 0) + util.vector3(0, 0, e.up or 0)
                impulse.apply(self, v)
            end
            say('move ' .. tostring(e.forward or 0))
        elseif e.type == 'hit' then
            -- The window opens here, on the segmented timeline: this runs even
            -- when the whole window, or the move itself, ends inside one frame.
            strike(e)
        elseif e.type == 'rejected' then
            say('rejected ' .. tostring(e.input) .. ' (' .. tostring(e.reason) .. ')')
        end
        notify(e)
    end
end

local function runner()
    return activeId and runners[activeId] or nil
end

local function press(name)
    local r = runner()
    if not r or not enabled() then return false end
    if types.Actor.isDead(self) then return false end
    dispatch(r.press(name))
    return true
end

local function updateHud()
    local want = settings:get('hud') == true and runner() ~= nil
    if not want then
        if hud then hud:destroy(); hud = nil end
        return
    end
    local st = runner().state()
    local text = st.move and string.format('%s  x%d%s', st.move, st.chain, st.canCancel and '  >' or '') or '-'
    if not hud then
        hud = ui.create({
            layer = 'HUD',
            type = ui.TYPE.Text,
            props = {
                relativePosition = util.vector2(0.5, 0.85), anchor = util.vector2(0.5, 0.5),
                text = text, textSize = 18, textColor = util.color.rgb(0.9, 0.85, 0.7),
            },
        })
    elseif hud.layout.props.text ~= text then
        hud.layout.props.text = text
        hud:update()
    end
end

for name, key in pairs(Input.TRIGGERS) do
    input.registerTriggerHandler(key, async:callback(function() press(name) end))
end

return {
    interfaceName = 'BrutalMoveset',
    interface = {
        version = 1,
        register = function(set)
            local errors = Core.validate(set)
            if #errors > 0 then
                say('rejected move set ' .. tostring(set and set.id) .. ': ' .. table.concat(errors, '; '))
                return false, errors
            end
            sets[set.id] = set
            runners[set.id] = Core.new(set, makeHooks())
            if savedRunners[set.id] then
                runners[set.id].load(savedRunners[set.id])
                savedRunners[set.id] = nil
            end
            say('registered move set ' .. set.id)
            if pendingActive == set.id then
                activeId, pendingActive = set.id, nil
                say('active move set ' .. set.id .. ' (restored)')
            end
            return true
        end,
        activate = function(id)
            if id ~= nil and not sets[id] then return false end
            if runner() then dispatch(runner().interrupt('switched')) end
            activeId = id
            say('active move set ' .. tostring(id))
            return true
        end,
        active = function() return activeId end,
        press = press,
        state = function() return runner() and runner().state() or nil end,
        onEvent = function(fn) listeners[#listeners + 1] = fn end,
    },
    engineHandlers = {
        onUpdate = function(dt)
            local r = runner()
            if r then
                if not enabled() or types.Actor.isDead(self) then
                    if r.state().move then dispatch(r.interrupt(types.Actor.isDead(self) and 'dead' or 'disabled')) end
                else
                    dispatch(r.update(dt))
                    -- Windows still open at the end of the frame keep trying,
                    -- so a target stepping into reach mid-window is struck.
                    for _, h in ipairs(r.activeHits()) do
                        local e = {}
                        for k, v in pairs(h.event) do e[k] = v end
                        e.move, e.ordinal = h.move, h.ordinal
                        strike(e)
                    end
                end
            end
            updateHud()
        end,
        onSave = function()
            local data = { version = 1, active = activeId, runners = {} }
            for id, r in pairs(runners) do data.runners[id] = r.save() end
            for id, saved in pairs(savedRunners) do data.runners[id] = data.runners[id] or saved end
            if not activeId and pendingActive then data.active = pendingActive end
            return data
        end,
        onLoad = function(data)
            if type(data) ~= 'table' or data.version ~= 1 then return end
            for id, saved in pairs(data.runners or {}) do
                if runners[id] then runners[id].load(saved) else savedRunners[id] = saved end
            end
            if data.active and sets[data.active] then activeId = data.active
            elseif data.active then pendingActive = data.active end
        end,
    },
}
