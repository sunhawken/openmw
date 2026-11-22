local settings=require('scripts.brutal_morrowind.settings')
-- A transient simulation-speed lease. Timing comes from the player's reference
-- configuration; an unrelated change of speed must never be overwritten on exit.
local core = require('openmw.core')
local world = require('openmw.world')
local types = require('openmw.types')
local lease
local accepted, rejected, restored = 0, 0, 0
local seen, order = {}, {}
local lastReason = 'idle'
local function finite(n) return type(n) == 'number' and n == n and math.abs(n) < math.huge end
local function same(a, b) return math.abs(a-b) < 0.00001 end
local function validPlayer(p)
    return p and p:isValid() and types.Player.objectIsInstance(p)
end
local function status()
    return {active = lease ~= nil, scale = core.getSimulationTimeScale(),
        previous = lease and lease.previous or nil, owned = lease and lease.owned or nil,
        attackId = lease and lease.attackId or nil, extended = lease and lease.extended or 0,
        remaining = lease and math.max(0, lease.expires-core.getRealTime()) or 0,
        accepted = accepted, rejected = rejected, restored = restored, reason = lastReason}
end
local function finish(reason)
    if not lease then return end
    local old = lease
    lease = nil
    local current = core.getSimulationTimeScale()
    -- setSimulationTimeScale is queued. If cancellation arrives before our first
    -- setter runs, queue its inverse too, otherwise slow motion could remain stuck.
    if same(current, old.owned) or (old.pending and same(current, old.previous)) then
        world.setSimulationTimeScale(old.previous)
        restored = restored + 1
    end
    lastReason = reason
    if validPlayer(old.player) then
        old.player:sendEvent('BrutalAdrenalineSlowMotionEnded',
            {attackId = old.attackId, reason = reason, extended = old.extended or 0})
    end
end
local function reject(data, reason)
    rejected = rejected + 1
    if type(data) == 'table' and validPlayer(data.player) then
        data.player:sendEvent('BrutalAdrenalineSlowMotionRejected',
            {attackId = data.attackId, reason = reason})
    end
end
local function start(data)
    if not settings.get('adrenalineEnabled') or not settings.get('slowMotionEnabled') then reject(data,'disabled');return end
    if type(data) ~= 'table' or not validPlayer(data.player)
        or types.Actor.isDead(data.player) or not finite(data.attackId)
        or data.attackId <= 0 or data.attackId % 1 ~= 0
        or not finite(data.duration) or data.duration <= 0 or data.duration > 10
        or not finite(data.factor) or data.factor < 0.01 or data.factor >= 1 then
        reject(data, 'invalid-request')
        return
    end
    if lease or seen[data.attackId] then reject(data, lease and 'busy' or 'duplicate'); return end
    local previous = core.getSimulationTimeScale()
    if not finite(previous) or previous <= 0 then reject(data, 'invalid-clock'); return end
    seen[data.attackId] = true
    order[#order+1] = data.attackId
    if #order > 128 then seen[table.remove(order, 1)] = nil end
    lease = {player = data.player, attackId = data.attackId, previous = previous,
        owned = previous * data.factor, expires = core.getRealTime()+data.duration, pending = true}
    accepted, lastReason = accepted+1, 'started'
    world.setSimulationTimeScale(lease.owned)
    data.player:sendEvent('BrutalAdrenalineSlowMotionStarted',
        {attackId = data.attackId, duration = data.duration, factor = data.factor})
end
return {
    interfaceName = 'BrutalAdrenalineTime', interface = {status = status},
    eventHandlers = {
        BrutalAdrenalineSlowMotion = start,
        BrutalAdrenalineSlowMotionExtend = function(data)
            if not settings.get('adrenalineEnabled') or not settings.get('slowMotionEnabled') then return end
            -- A kicked ragdoll during the finisher keeps the world slowed a little longer.
            if not lease or type(data) ~= 'table' or not finite(data.seconds) or data.seconds <= 0
                or not finite(data.maximum) then return end
            local seconds = math.min(data.seconds, data.maximum - (lease.extended or 0))
            if seconds <= 0 then return end
            lease.extended = (lease.extended or 0) + seconds
            lease.expires = lease.expires + seconds
        end,
        BrutalAdrenalineSlowMotionCancel = function(data)
            if lease and type(data) == 'table' and data.player == lease.player
                and (data.attackId == nil or data.attackId == lease.attackId) then
                finish(data.reason or 'cancelled')
            end
        end,
        BrutalAdrenalineSlowMotionQuery = function(data)
            if type(data) == 'table' and validPlayer(data.player) then
                local s = status(); s.queryId = data.queryId
                data.player:sendEvent('BrutalAdrenalineSlowMotionStatus', s)
            end
        end,
    },
    engineHandlers = {
        onUpdate = function()
            if not settings.get('adrenalineEnabled') or not settings.get('slowMotionEnabled') then finish('disabled');return end
            if not lease then return end
            if not validPlayer(lease.player) or types.Actor.isDead(lease.player) then finish('death'); return end
            local current = core.getSimulationTimeScale()
            if same(current, lease.owned) then lease.pending = false
            elseif not lease.pending or not same(current, lease.previous) then finish('external-speed-change'); return end
            if core.getRealTime() >= lease.expires then finish('expired') end
        end,
        onPlayerAdded = function() finish('player-added') end,
        onSave = function() return {} end,
        onLoad = function()
            finish('load')
            seen, order = {}, {}
            accepted, rejected, restored = 0, 0, 0
            lastReason = 'loaded'
        end,
    },
}
