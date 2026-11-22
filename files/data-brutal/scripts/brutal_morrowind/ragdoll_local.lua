local settings=require('scripts.brutal_morrowind.settings')
-- Requires the native ragdoll backport in this project's engine. Creatures with no rig ignore it.
-- Observe ordinary combat without replacing damage, death, inventory or crime.
local self = require('openmw.self')
local types = require('openmw.types')
local core = require('openmw.core')
local nearby = require('openmw.nearby')
local util = require('openmw.util')
local I = require('openmw.interfaces')
local C = require('scripts.brutal_morrowind.config')
local wasAlive, lastDirection, hitAge, hitStrength = nil, nil, 10, 0
local poweredHit = false
local authoredVelocity
local function deathVelocity()
    if authoredVelocity and hitAge < 3 then return authoredVelocity end
    if lastDirection and hitAge < 3 then
        return lastDirection * (poweredHit and 600 or (120 + 160 * hitStrength))
            + util.vector3(0, 0, poweredHit and 120 or 60)
    end
    return util.vector3(0, 0, 0)
end
I.Combat.addOnHitHandler(function(attack)
    if not settings.get('ragdollEnabled') then return end
    if attack.successful and not attack.brutalBlocked and ((attack.damage or {}).health or 0) > 0
        and attack.attacker and attack.attacker:isValid() then
        authoredVelocity = nil
        if attack.brutalPowered and attack.brutalImpactDirection then
            -- The event snapshots the imported player's one-handed slash direction.
            -- Start the lethal pose in that direction, without an earlier forward/up shove.
            authoredVelocity = attack.brutalImpactDirection * (250 * 1.84265547703229)
            hitAge = 0
        end
        local direction = self.position-attack.attacker.position
        direction = util.vector3(direction.x,direction.y,0)
        if direction:length()>0.1 then
            lastDirection, hitAge = direction:normalize(), 0
            hitStrength = math.max(0,math.min(1,attack.strength or 0))
            poweredHit = attack.brutalPowered == true
        end
        if not types.Player.objectIsInstance(self) then
            -- Handlers run in reverse registration order. Native Combat reads this
            -- before queueing damage; it starts physics only if that damage is lethal.
            attack.brutalRagdollVelocity = deathVelocity()
        end
    end
end)
return {eventHandlers = {
    -- A kick struck this corpse or its severed head: during the finisher the
    -- slow motion lasts a little longer so the flight can be seen.
    BrutalRagdollKicked = function(data)
        if not settings.get('ragdollEnabled') or not settings.get('kickEnabled') then return end
        -- Physics confirms the surface actually struck. Corpse kicks bypass the
        -- living Combat path, so their impact feedback belongs to this receipt.
        if (settings.get('bloodEffects') or settings.get('bloodDecals')) and data.position and data.direction then
            core.sendGlobalEvent('BrutalImpact', {actor=self, kind='blood',
                position=data.position, direction=(-data.direction+util.vector3(0,0,.5)):normalize(),
                -- Physics part indices are not ESM armor/skin part tags. Let
                -- decal projection choose the current skin nearest this point.
                contactExact=true, woundKind='kick', intensity=1})
        end
        core.sendGlobalEvent('BrutalAdrenalineSlowMotionExtend',
            {seconds = C.slowMotionKickExtension, maximum = C.slowMotionKickExtensionMax})
        -- Only the player kicks ragdolls; it pays the kick's stamina.
        for _, player in ipairs(nearby.players) do player:sendEvent('BrutalRagdollKickLanded', data) end
    end,
}, engineHandlers = {
    onInit = function() wasAlive = not types.Actor.isDead(self) end,
    onLoad = function()
        -- Existing corpses retain the engine's normal loaded death pose.
        wasAlive, lastDirection, hitAge = not types.Actor.isDead(self), nil, 10
        poweredHit = false
        authoredVelocity = nil
    end,
    onUpdate = function(dt)
        hitAge=hitAge+dt
        local alive=not types.Actor.isDead(self)
        if settings.get('ragdollEnabled') and wasAlive and not alive then
            local velocity = deathVelocity()
            core.sendGlobalEvent('BrutalRagdollDeath',{actor=self,velocity=velocity})
        end
        wasAlive=alive
    end,
}}
