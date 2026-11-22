local settings=require('scripts.brutal_morrowind.settings')
local world=require('openmw.world')
local core=require('openmw.core')
local types=require('openmw.types')
local util=require('openmw.util')
local C=require('scripts.brutal_morrowind.config')
local available=type(world.applyNpcKick)=='function'
local ragdollKick=type(world.kickNpcRagdoll)=='function'
local superKick=type(world.applyNpcSuperKick)=='function'
return {eventHandlers={BrutalRagdollKick=function(e)
    if not settings.get('kickEnabled') or not settings.get('ragdollEnabled') then return end
    -- The engine picks the nearest reachable corpse part or severed head and reports
    -- a strike to that corpse as BrutalRagdollKicked.
    if not ragdollKick or not e.attacker or not e.attacker:isValid() then return end
    -- Match the real-time boot once at impact. A floor on the divisor bounds
    -- velocity even if another mod slows the world further; no per-tick force.
    local scale=math.max(.25,math.min(1,core.getSimulationTimeScale()))
    local speed=C.ragdollKickSpeed/scale
    local velocity=e.direction*speed+util.vector3(0,0,speed*C.ragdollKickLift)
    world.kickNpcRagdoll(e.origin,e.direction,C.reach,velocity)
end,
BrutalNpcKick=function(e)
    if not settings.get('enabled') then return end
    if e.superKickId and e.attacker and e.attacker:isValid() then
        if not settings.get('superKickEnabled') or not settings.get('adrenalineEnabled') then
            e.attacker:sendEvent('BrutalSuperKickResolved',{kickId=e.superKickId,accepted=false});return
        end
        if superKick and e.actor and e.actor:isValid() and types.Actor.objectIsInstance(e.actor) then
            world.applyNpcSuperKick(e.actor,e.velocity,e.attacker,e.superKickId)
        else e.attacker:sendEvent('BrutalSuperKickResolved',{kickId=e.superKickId,accepted=false}) end
        return
    end
    if not e.actor or not e.actor:isValid() or not e.attacker or not e.attacker:isValid()
        or not types.Actor.objectIsInstance(e.actor) or types.Actor.isDead(e.actor) then return end
    if e.kick and not settings.get('kickEnabled') then return end
    local knockDown = e.knockDown == true or (e.kick == true and settings.get('kickAlwaysRagdoll'))
    if available then world.applyNpcKick(e.actor,e.velocity,e.attacker,knockDown,e.slideTrip==true)
    else e.actor:sendEvent('BrutalLegacyNpcKick',{velocity=e.velocity}) end
end}}
