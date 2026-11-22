local settings=require('scripts.brutal_morrowind.settings')
-- Observe the completed ordinary Combat path; never apply a second damage event.
local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local I = require('openmw.interfaces')
local previous = I.Combat
local interface = require('openmw_aux.util').shallowCopy(previous)
local available, requestAvailability = false, true
local currentAttack, selectedArmor
local bloodRequests, sparkRequests = 0, 0
local bloodDecalData, restoreBloodDecals = '', false
local bloodRestoreWait = 0
local decapitation = require('scripts.brutal_morrowind.decapitation')
local decapSelected, restoreDecapitation = false, false
local torsoSelected, restoreBisection = false, false
local torsoPoseData = ''
local torsoPoseWait = 0
local limbMask, limbPose, limbPoseWait, restoreLimbs = 0, '', 0, false
local limbSelections, legRoll = 0, nil
local bisectionEligible, bisectionSelections = 0, 0
local decapEligible, decapSelections, decapRoll, decapAttackId = 0, 0, nil, nil
local pendingDecapitation = {}
-- Dark Messiah life_tohurt: a blow removing this share of maximum health staggers
-- the whole body. Every smaller damaging hit keeps the native small flinch.
local HEAVY_HIT_FRACTION = 0.1
local hasStagger, stagger = pcall(function() return types.Actor.stagger end)
local heavyStaggers = 0

local function requestDecapitation(velocity, cutDirection)
    if not settings.get('dismemberment') then return end
    core.sendGlobalEvent('BrutalDecapitationDeath', {actor = self, velocity = velocity,
        cutDirection = cutDirection or util.vector3(0, 0, 0)})
end
local function requestBisection(velocity, cutDirection, savedPose)
    if not settings.get('dismemberment') then return end
    core.sendGlobalEvent('BrutalBisectionDeath', {actor=self, velocity=velocity,
        cutDirection=cutDirection or util.vector3(0, 0, 0), savedPose=savedPose or ''})
end
local function requestLimbs(mask, cutDirection, savedPose)
    if not settings.get('dismemberment') then return end
    core.sendGlobalEvent('BrutalLimbSeveringDeath', {actor=self, mask=mask,
        velocity=util.vector3(0,0,0), cutDirection=cutDirection or util.vector3(0,0,0),
        savedPose=savedPose or ''})
end
local function validLimbMask(mask)
    return type(mask)=='number' and mask>=1 and mask<=15 and mask==math.floor(mask)
end

local function direction(attack)
    if attack and attack.attacker and attack.attacker:isValid() then
        local delta = self.position - attack.attacker.position
        if delta:length() > 0.01 then return delta:normalize() end
    end
    return util.vector3(0, 1, 0)
end

local function metal(item)
    if not item or not types.Armor.objectIsInstance(item) then return false end
    local record = types.Armor.record(item)
    -- Morrowind has no per-armor physics material. Match known metal families;
    -- do not treat leather, bone and chitin as metal merely by armor skill.
    local name = string.lower((record.id or '') .. ' ' .. (record.model or ''))
    for _, material in ipairs({'iron', 'steel', 'silver', 'ebony', 'dwemer', 'dwarven', 'daedric', 'adamantium', 'orcish'}) do
        if string.find(name, material, 1, true) then return true end
    end
    return false
end

local function burst(kind, position, attack)
    if kind=='blood' and not (settings.get('bloodEffects') or settings.get('bloodDecals'))
        or kind=='sparks' and not settings.get('sparkEffects') then return end
    if not available or not position then return end
    local outward = direction(attack)
    if kind == 'sparks' then
        -- Reflect metal fragments toward the incoming blade, clear of the shield.
        outward = -outward
        if not (attack and attack.brutalContactExact) then position = position + outward * 8 end
    else
        -- Spray back out of the wound toward the attacker, where the view can see
        -- it; drops thrown away from the camera are hidden behind the body.
        outward = (-outward + util.vector3(0, 0, 0.6)):normalize()
    end
    core.sendGlobalEvent('BrutalImpact', {
        actor = self, kind = kind, position = position, direction = outward,
        woundKind = attack and attack.brutalAttackType or '',
        woundPart = attack and attack.brutalContactPart or nil,
        attachment = kind == 'sparks' and attack and attack.brutalBlocked and 'shield' or nil,
        contactExact = attack and attack.brutalContactExact == true or false,
        power = attack and attack.brutalPowered or false,
        intensity = types.Player.objectIsInstance(self) and 0.65 or (kind == 'blood' and 2 or 1),
    })
    if kind == 'blood' then bloodRequests = bloodRequests + 1 else sparkRequests = sparkRequests + 1 end
end

interface.pickRandomArmor = function(actor)
    local item = previous.pickRandomArmor(actor)
    if currentAttack then selectedArmor = item end
    return item
end

interface.spawnBloodEffect = function(position)
    -- Creatures have no wound surfaces here: they keep their own stock blood.
    if not available or not types.NPC.objectIsInstance(self) then return previous.spawnBloodEffect(position) end
    -- Marks have their own switch. Native placement can record a wound without
    -- emitting our blood particles; keep stock blood in that combination.
    if not types.Player.objectIsInstance(self) then burst('blood', position, currentAttack) end
    if not settings.get('bloodEffects') then return previous.spawnBloodEffect(position) end
    -- Keep the stock first-person choice: world blood belongs on the opponent,
    -- while the normal damage vignette communicates wounds to the player.
end

interface.adjustDamageForDifficulty = function(attack)
    previous.adjustDamageForDifficulty(attack)
    -- DM's manual describes a full-adrenaline Power Strike as a finishingmove.
    -- Apply this after ordinary armor/difficulty in the same Combat damage path,
    -- so death, killer attribution and ragdoll still receive one ordinary hit.
    if settings.get('finisherEnabled') and settings.get('adrenalineEnabled') and attack.brutalPowered and attack.successful and not attack.brutalBlocked
        and attack.sourceType=='melee' and attack.attacker and attack.attacker:isValid()
        and types.Player.objectIsInstance(attack.attacker)
        and not types.Player.objectIsInstance(self)
        and not types.Actor.isDead(self) and attack.damage and (attack.damage.health or 0)>0 then
        attack.damage.health=math.max(attack.damage.health,types.Actor.stats.dynamic.health(self).current)
    end
end

interface.onHit = function(attack)
    local aliveBefore = not types.Actor.isDead(self)
    local outerAttack, outerArmor = currentAttack, selectedArmor
    currentAttack, selectedArmor = attack, nil
    previous.onHit(attack)
    local hitArmor = selectedArmor
    currentAttack, selectedArmor = outerAttack, outerArmor
    local healthDamage = (attack.damage or {}).health or 0
    if settings.get('finisherEnabled') and settings.get('adrenalineEnabled') and attack.brutalAttackId and attack.brutalPowered then
        -- Combat queues native damage. Confirm the outcome of THIS damage action,
        -- rather than sampling death before it runs or attributing a later death.
        pendingDecapitation[attack.brutalAttackId] = {attack = attack, age = 0}
    end
    if aliveBefore and attack.successful and attack.sourceType == 'melee' and (attack.weapon or attack.brutalBlocked) then
        if attack.brutalBlocked then
            hitArmor = types.Actor.getEquipment(self)[types.Actor.EQUIPMENT_SLOT.CarriedLeft]
        end
        if (healthDamage > 0 or attack.brutalBlocked) and metal(hitArmor) then
            burst('sparks', attack.hitPos, attack)
        end
        if settings.get('finisherEnabled') and settings.get('adrenalineEnabled') and attack.brutalPowered and healthDamage > 0 and not types.Actor.isDead(self) then
            -- This is a physical recoil on the existing actor, not extra damage.
            types.Actor.addExternalVelocity(self, direction(attack) * 420 + util.vector3(0, 0, 70))
        end
    end
    if attack.attacker and attack.attacker:isValid() then
        attack.attacker:sendEvent('BrutalDamagingHit', {
            target = self, weapon = attack.weapon, sourceType = attack.sourceType,
            successful = attack.successful, healthDamage = healthDamage,
            targetWasAlive = aliveBefore, powered = attack.brutalPowered or false,
            attackId = attack.brutalAttackId, fast = attack.brutalFast or false,
        })
    end
end
interface.brutalFeedbackStats = function()
    return {available = available, bloodRequests = bloodRequests, sparkRequests = sparkRequests,
        decapEligible = decapEligible, decapSelected = decapSelections, decapRoll = decapRoll,
        decapAttackId = decapAttackId, headSevered = decapSelected, heavyStaggers = heavyStaggers,
        bisectionEligible = bisectionEligible, bisectionSelected = bisectionSelections, torsoSevered = torsoSelected,
        limbsMask=limbMask, limbSelections=limbSelections, legRoll=legRoll}
end
return {
    interfaceName = 'Combat', interface = interface,
    eventHandlers = {
        BrutalBloodDecalsStored = function(data)
            if data.restored and type(data.data) == 'string' then
                bloodDecalData, restoreBloodDecals = data.data, false
            else bloodRestoreWait = 1 end
        end,
        BrutalFeedbackAvailable = function(data) available = data.available == true end,
        BrutalHitApplied = function(data)
            local damage = data.actualHealthDamage or 0
            if settings.get('npcStagger') and hasStagger and stagger and damage > 0 and not data.deadAfter
                and not types.Player.objectIsInstance(self)
                and damage >= HEAVY_HIT_FRACTION * types.Actor.stats.dynamic.health(self).base then
                if stagger(self) then heavyStaggers = heavyStaggers + 1 end
            end
            local entry = pendingDecapitation[data.attackId]
            if not entry then return end
            pendingDecapitation[data.attackId] = nil
            if (data.actualHealthDamage or 0) <= 0 then return end
            local attack = entry.attack
            -- Notify from this native damage action, never from a delayed sample
            -- that could accidentally attribute a later death to this attack.
            if data.aliveBefore and data.deadAfter and attack.brutalPowered
                and attack.successful and not attack.brutalBlocked
                and attack.attacker and attack.attacker:isValid() then
                attack.attacker:sendEvent('BrutalFinisherConfirmed', {
                    target=self, weapon=attack.weapon, sourceType=attack.sourceType,
                    successful=true, targetWasAlive=true, deadAfter=true, powered=true,
                    healthDamage=data.actualHealthDamage, attackId=data.attackId,
                })
            end
            local armEligible, selectedLimb = decapitation.considerArm(attack,
                data.aliveBefore == true, data.deadAfter == true,
                types.NPC.objectIsInstance(self), types.Player.objectIsInstance(self))
            local legEligible, selectedLeg, draw = decapitation.considerLegs(attack,
                data.aliveBefore == true, data.deadAfter == true,
                types.NPC.objectIsInstance(self), types.Player.objectIsInstance(self))
            if armEligible or legEligible then
                if limbMask==0 and not decapSelected and not torsoSelected then
                    limbMask = armEligible and selectedLimb or selectedLeg
                    limbSelections, legRoll = limbSelections+1, draw
                    requestLimbs(limbMask, attack.brutalImpactDirection or attack.brutalAimDirection or direction(attack))
                end
                return
            end
            local waistEligible, waistSelected = decapitation.considerBisection(attack,
                data.aliveBefore == true, data.deadAfter == true,
                types.NPC.objectIsInstance(self), types.Player.objectIsInstance(self))
            if waistEligible then
                bisectionEligible = bisectionEligible + 1
                if waistSelected and not decapSelected and not torsoSelected and limbMask==0 then
                    torsoSelected, bisectionSelections = true, bisectionSelections + 1
                    local forceDirection = attack.brutalImpactDirection or attack.brutalAimDirection or direction(attack)
                    requestBisection(forceDirection * (250 * 1.84265547703229), forceDirection)
                end
                return
            end
            local eligible, selected, draw = decapitation.consider(attack,
                data.aliveBefore == true, data.deadAfter == true,
                types.NPC.objectIsInstance(self), types.Player.objectIsInstance(self))
            if not eligible then return end
            decapEligible, decapRoll, decapAttackId = decapEligible + 1, draw, data.attackId
            if selected and limbMask==0 and not torsoSelected then
                decapSelected, decapSelections = true, decapSelections + 1
                local outward = direction(attack)
                local planar = util.vector3(outward.x, outward.y, 0)
                if planar:length() > 0.01 then planar = planar:normalize() end
                local forceDirection = attack.brutalImpactDirection or attack.brutalAimDirection or outward
                local initialVelocity = attack.brutalImpactDirection
                    and forceDirection * (250 * 1.84265547703229)
                    or planar * 600 + util.vector3(0, 0, 120)
                requestDecapitation(initialVelocity, forceDirection)
            end
        end,
    },
    engineHandlers = {
        onUpdate = function(dt)
            bloodRestoreWait = math.max(0, bloodRestoreWait - dt)
            if settings.get('bloodDecals') and restoreBloodDecals and bloodRestoreWait == 0 then
                core.sendGlobalEvent('BrutalRestoreBloodDecals', {actor=self, data=bloodDecalData})
                bloodRestoreWait = 1
            end
            for id, entry in pairs(pendingDecapitation) do
                entry.age = entry.age + dt
                if entry.age > 2 then pendingDecapitation[id] = nil end
            end
            if torsoSelected and not types.Actor.isDead(self) then
                torsoSelected, restoreBisection = false, false
                torsoPoseData = ''
            end
            if limbMask~=0 and not types.Actor.isDead(self) then
                limbMask, limbPose, restoreLimbs = 0, '', false
            end
            limbPoseWait = math.max(0, limbPoseWait-dt)
            if limbMask~=0 and limbPoseWait==0 and type(types.Actor.getLimbSeveringPoseData)=='function' then
                local pose=types.Actor.getLimbSeveringPoseData(self)
                if #pose>0 and #pose<=2048 then limbPose=pose end
                limbPoseWait=.25
            end
            if restoreLimbs then
                restoreLimbs=false
                if limbMask~=0 then requestLimbs(limbMask,nil,limbPose) end
            end
            torsoPoseWait = math.max(0, torsoPoseWait-dt)
            if torsoSelected and torsoPoseWait==0 and type(types.Actor.getTorsoSeveredPoseData)=='function' then
                -- Keep the last active snapshot when the scene/physics unloads
                -- before a later save. Settled fragments need no active bodies.
                local pose=types.Actor.getTorsoSeveredPoseData(self)
                if #pose==60 then torsoPoseData=pose end
                torsoPoseWait=.25
            end
            if restoreBisection then
                restoreBisection = false
                if torsoSelected then requestBisection(util.vector3(0, 0, 0), nil, torsoPoseData) end
            end
            if decapSelected and not types.Actor.isDead(self) then
                decapSelected, restoreDecapitation = false, false
            end
            if restoreDecapitation then
                restoreDecapitation = false
                if decapSelected then requestDecapitation(util.vector3(0, 0, 0)) end
            end
            if requestAvailability then
                requestAvailability = false
                core.sendGlobalEvent('BrutalFeedbackQuery', {actor = self})
            end
        end,
        onSave = function()
            -- Native/scripted cuts share the same persisted corpse state.
            -- The chosen policy flag also survives while an actor is inactive.
            local nativeCut = type(types.Actor.isHeadSevered) == 'function' and types.Actor.isHeadSevered(self)
            local nativeTorso = type(types.Actor.isTorsoSevered) == 'function' and types.Actor.isTorsoSevered(self)
            local nativeLimbs = type(types.Actor.getSeveredLimbsMask)=='function'
                and types.Actor.getSeveredLimbsMask(self) or 0
            if validLimbMask(nativeLimbs) then limbMask=nativeLimbs end
            if limbMask~=0 and type(types.Actor.getLimbSeveringPoseData)=='function' then
                local pose=types.Actor.getLimbSeveringPoseData(self)
                if #pose>0 and #pose<=2048 then limbPose=pose end
            end
            if nativeTorso and type(types.Actor.getTorsoSeveredPoseData) == 'function' then
                local pose = types.Actor.getTorsoSeveredPoseData(self)
                if #pose == 60 then torsoPoseData = pose end
            end
            return {headSevered = decapSelected or nativeCut, torsoSevered = torsoSelected or nativeTorso,
                torsoPoseData = torsoPoseData, bloodDecals = bloodDecalData,
                limbsMask=limbMask, limbPoseData=limbPose}
        end,
        onLoad = function(data)
            pendingDecapitation = {}
            bloodDecalData = data and type(data.bloodDecals) == 'string' and data.bloodDecals or ''
            restoreBloodDecals, bloodRestoreWait = bloodDecalData ~= '', 0
            available, requestAvailability = false, true
            decapSelected = data and data.headSevered == true or false
            torsoSelected = data and data.torsoSevered == true or false
            torsoPoseData = data and type(data.torsoPoseData) == 'string' and data.torsoPoseData or ''
            if #torsoPoseData ~= 60 then torsoPoseData = '' end
            torsoPoseWait = 0
            limbMask = data and validLimbMask(data.limbsMask) and data.limbsMask or 0
            limbPose = data and type(data.limbPoseData)=='string' and data.limbPoseData or ''
            if #limbPose>2048 then limbPose='!' end
            limbPoseWait, limbSelections, legRoll = 0, 0, nil
            if limbMask~=0 then torsoSelected, decapSelected=false,false end
            restoreLimbs = limbMask~=0
            if torsoSelected then decapSelected = false end
            restoreDecapitation = decapSelected
            restoreBisection = torsoSelected
        end,
        onActive = function()
            restoreLimbs = limbMask~=0
            restoreDecapitation = decapSelected
            restoreBisection = torsoSelected
            restoreBloodDecals, bloodRestoreWait = bloodDecalData ~= '', 0
        end,
    },
}
