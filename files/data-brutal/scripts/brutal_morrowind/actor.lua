local settings=require('scripts.brutal_morrowind.settings')
-- Native engine motion only. Lua attributes wall damage to the accepted kick;
-- OpenMW owns actor displacement, collision, gravity, and fall-height tracking.
local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local native = require('scripts.brutal_morrowind.native_impulse')
local C = require('scripts.brutal_morrowind.config')
local reaction, impacts, nativePushes = nil, 0, 0
-- Actor.stagger is optional; reactions work without it.
local hasStagger, stagger = pcall(function() return types.Actor.stagger end)

local function begin(data)
    if not settings.get('enabled') then return end
    if types.Actor.isDead(self) or not data.attacker or not data.attacker:isValid() then return end
    local direction = util.vector3(data.direction.x, data.direction.y, 0)
    if direction:length() < 0.1 then return end
    local speed = data.speed or C.pushSpeed -- a shield bash pushes less than a kick
    -- The native action tests support atomically and chooses recoil or an
    -- immediate living ragdoll. The kick enters exactly one motion channel.
    -- The vertical part lifts the body only if the kick starts a ledge ragdoll.
    -- Creatures react with their own animations; the engine scales the push by their mass.
    core.sendGlobalEvent('BrutalNpcKick', {actor=self, attacker=data.attacker,
        kick=data.kick==true, knockDown=data.knockDown==true,
        velocity=direction:normalize()*speed + util.vector3(0, 0, speed*C.ledgeKickLift)})
    reaction = {age = 0, attacker = data.attacker, cell = self.cell, speed = speed}
    nativePushes = nativePushes + 1
end

local function nativeImpact(data)
    if not settings.get('enabled') then reaction=nil;return end
    if not reaction or types.Actor.isDead(self) or self.cell ~= reaction.cell
        or not reaction.attacker:isValid() then return end
    if data.hitObject and (types.Actor.objectIsInstance(data.hitObject)
        or types.Item.objectIsInstance(data.hitObject)) then return end
    if data.speed < C.wallMinSpeed or math.abs(data.normal.z) > 0.7 then return end
    self:sendEvent('Hit', {attacker = reaction.attacker, successful = true,
        sourceType = 'unspecified', strength = 1,
        damage = {health = C.wallDamage * math.min(data.speed / reaction.speed, 2), fatigue = 20},
        hitPos = data.hitPos,
    })
    impacts = impacts + 1
    reaction = nil
end

local function update(dt)
    if not settings.get('enabled') then reaction=nil;return end
    if not reaction or dt <= 0 then return end
    reaction.age = reaction.age + dt
    if reaction.age > 1 or types.Actor.isDead(self) or self.cell ~= reaction.cell
        or types.Actor.isSwimming(self) then reaction = nil end
end

return {
    interfaceName = 'BrutalReaction',
    interface = {status = function() return {
        -- The attribution window may outlast grounded motion; it is transient.
        moving = reaction ~= nil, moves = 0, impacts = impacts,
        native = true, nativePushes = nativePushes, staggerAvailable = hasStagger and stagger ~= nil,
    } end},
    engineHandlers = {
        onUpdate = update,
        onInactive = function() reaction = nil end,
        onLoad = function() reaction = nil end,
    },
    eventHandlers = {BrutalImpulse = begin, BrutalNativeImpact = nativeImpact,
        BrutalLegacyNpcKick=function(data)
            if not settings.get('enabled') then return end
            if types.Actor.isDead(self) then return end
            native.apply(self,data.velocity)
            if hasStagger and stagger then stagger(self) end
        end},
}
