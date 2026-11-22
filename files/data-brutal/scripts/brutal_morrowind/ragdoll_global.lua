local settings=require('scripts.brutal_morrowind.settings')
-- Activate from the lethal pose; retry briefly if the scene is not ready yet.
-- Ordinary native death notifications and inventory ownership remain intact.
local world = require('openmw.world')
local types = require('openmw.types')
local decapitate = world.startNpcDecapitation
local bisect = world.startNpcBisection
local severLimbs = world.startNpcLimbSevering
local pending = {}
return {
    eventHandlers = {BrutalRagdollDeath = function(data)
        if not settings.get('ragdollEnabled') then return end
        if data.actor and data.actor:isValid() and types.Actor.objectIsInstance(data.actor) then
            if not pending[data.actor] then
                pending[data.actor] = {velocity=data.velocity,age=0,retry=0}
            end
        end
    end,
    BrutalDecapitationDeath = function(data)
        if not settings.get('dismemberment') then return end
        if data.actor and data.actor:isValid() and types.NPC.objectIsInstance(data.actor)
            and not types.Player.objectIsInstance(data.actor) then
            local entry = pending[data.actor] or {velocity=data.velocity,age=0,retry=0}
            entry.decapitate = true
            entry.age, entry.retry = 0, 0
            entry.cutDirection = data.cutDirection
            pending[data.actor] = entry
        end
    end,
    BrutalBisectionDeath = function(data)
        if not settings.get('dismemberment') then return end
        if data.actor and data.actor:isValid() and types.NPC.objectIsInstance(data.actor)
            and not types.Player.objectIsInstance(data.actor) then
            local entry = pending[data.actor] or {velocity=data.velocity,age=0,retry=0}
            entry.bisect = true
            entry.age, entry.retry = 0, 0
            entry.cutDirection = data.cutDirection
            entry.savedPose = data.savedPose
            pending[data.actor] = entry
        end
    end,
    BrutalLimbSeveringDeath = function(data)
        if not settings.get('dismemberment') then return end
        if data.actor and data.actor:isValid() and types.NPC.objectIsInstance(data.actor)
            and not types.Player.objectIsInstance(data.actor) and type(data.mask)=='number'
            and data.mask>=1 and data.mask<=15 and data.mask==math.floor(data.mask) then
            local entry = pending[data.actor] or {velocity=data.velocity,age=0,retry=0}
            entry.limbs, entry.savedPose = data.mask, data.savedPose
            entry.age, entry.retry, entry.cutDirection = 0, 0, data.cutDirection
            pending[data.actor] = entry
        end
    end},
    engineHandlers = {
        onLoad = function() pending={} end,
        onUpdate = function(dt)
            for actor, entry in pairs(pending) do
                if not settings.get('dismemberment') then
                    entry.decapitate,entry.bisect,entry.limbs=nil,nil,nil
                end
                if not settings.get('ragdollEnabled') then pending[actor]=nil
                else
                entry.age, entry.retry = entry.age+dt, entry.retry-dt
                if not actor:isValid() or not actor.cell or not actor.enabled
                    or not types.Actor.isDead(actor) or entry.age>10 then
                    pending[actor]=nil
                elseif entry.retry<=0 then
                    -- A moving lethal pose can briefly fail anatomical cut
                    -- clearance. Recheck early without changing any geometry guard.
                    entry.retry=(entry.decapitate or entry.bisect or entry.limbs) and entry.age<0.2 and 0.025 or 0.2
                    local state = world.getNpcRagdollState(actor)
                    if entry.limbs and severLimbs then
                        if state and state.limbsMask==entry.limbs then pending[actor]=nil
                        else severLimbs(actor,entry.limbs,entry.velocity,entry.cutDirection,entry.savedPose) end
                    elseif entry.bisect and bisect then
                        if state and state.torsoDetached then pending[actor]=nil
                        else bisect(actor,entry.velocity,entry.cutDirection,entry.savedPose) end
                    elseif entry.decapitate and decapitate then
                        if state and state.headDetached then
                            pending[actor]=nil
                        else
                            -- Queueing is not proof of success: only the next native state
                            -- confirms both prepared render geometry and physical detachment.
                            decapitate(actor,entry.velocity,entry.cutDirection)
                        end
                    elseif state or world.startNpcRagdoll(actor,entry.velocity) then
                        pending[actor]=nil
                    end
                end
                end
            end
        end,
    },
}
