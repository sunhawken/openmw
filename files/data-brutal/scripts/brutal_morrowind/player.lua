local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local util = require('openmw.util')
local nearby = require('openmw.nearby')
local input = require('openmw.input')
local async = require('openmw.async')
local bindings = require('scripts.brutal_morrowind.input_bindings')
local animation = require('openmw.animation')
local ui = require('openmw.ui')
local camera = require('openmw.camera')
local I = require('openmw.interfaces')
local settings = require('scripts.brutal_morrowind.settings')
local C = require('scripts.brutal_morrowind.config')
local cooldown, windup = 0, nil
local activeKick, activeKickSpeed

-- Bullet time: during the finisher's slow motion the kick keeps real-time speed,
-- so its blow lands within the two real seconds. Returns the world time scale.
local function kickTimeScale()
    local scale = core.getSimulationTimeScale()
    return (scale > 0 and scale < 1) and scale or 1
end
local lastResult, strikes = 'ready', 0
local ragdollHits = 0
local physicsAvailable = false
local superKickAvailable, superKickSerial = false, 0
local hasShieldBlock, setShieldBlock = pcall(function() return types.Actor.setShieldBlock end)
local blockHeld = false
local isBlockRaised = types.Actor.isShieldBlockRaised
local bash, bashWindup, bashes, usePrev, guardPress = nil, nil, 0, false, nil

local function sliding()
    return I.BrutalSlide and I.BrutalSlide.status().phase ~= 0
end
local function clinching()
    if not I.BrutalClinch then return false end
    local s=I.BrutalClinch.status()
    return s.active or s.pending
end

local function chooseTarget()
    local origin = self.position + util.vector3(0, 0, 55)
    local forward = self.rotation * util.vector3(0, 1, 0)
    forward = util.vector3(forward.x, forward.y, 0):normalize()
    local target, closest = nil, C.reach
    for _, actor in ipairs(nearby.actors) do
        if actor ~= self and not types.Actor.isDead(actor) then
            local offset = actor.position - self.position
            local flat = util.vector3(offset.x, offset.y, 0)
            local half = types.Actor.getPathfindingAgentBounds(actor).halfExtents
            -- Reach ends at the body, not at its centre: a kwama queen's middle is out of any boot's range.
            local distance = math.max(1.01, flat:length() - math.max(0, math.max(half.x, half.y) - 40))
            if distance > 1 and distance <= closest and math.abs(offset.z) < 90 + math.max(0, half.z - 64)
                and flat:normalize():dot(forward) >= C.cone then
                local aim = actor.position + util.vector3(0, 0, math.min(55, half.z))
                local hit = nearby.castRay(origin, aim, {ignore = self})
                if not hit.hit or hit.hitObject == actor then
                    target, closest = actor, distance
                end
            end
        end
    end
    return target, forward
end

local function tryKick()
    if not settings.get('kickEnabled') then
        lastResult = 'disabled'
        return false, lastResult
    end
    if I.UI.getMode() or types.Actor.isDead(self) or not types.Actor.canMove(self)
        or types.Actor.isSwimming(self) then
        lastResult = 'unavailable'
        return false, lastResult
    end
    -- A kick interrupts any sword phase (the engine cancels its pending hit), but
    -- not an unfinished kick: its own animation is the kick's cooldown.
    if bash or sliding() or clinching() or animation.getCompletion(self, 'brutal_kick') ~= nil
        or animation.getCompletion(self, 'brutal_kick1h') ~= nil
        or animation.getCompletion(self, 'brutal_kick1h_low') ~= nil then
        lastResult = 'busy'
        return false, lastResult
    end
    if cooldown > 0 then lastResult = 'cooldown'; return false, lastResult end
    -- Dark Messiah refuses a kick below its stamina reserve; the cost is paid on impact.
    local fatigue = types.Actor.stats.dynamic.fatigue(self)
    if settings.get('staminaEnabled') and fatigue.current < fatigue.base * C.kickStaminaNeeded / C.staminaMax then
        lastResult = 'exhausted'
        ui.showMessage('Too exhausted to kick')
        return false, lastResult
    end
    cooldown, windup, lastResult = C.cooldown, C.windup, 'windup'
    self:sendEvent('BreakInvisibility', {})
    local kickGroup = 'brutal_kick'
    local weapon = types.Actor.getEquipment(self)[types.Actor.EQUIPMENT_SLOT.CarriedRight]
    if types.Actor.getStance(self)==types.Actor.STANCE.Weapon and weapon and types.Weapon.objectIsInstance(weapon) then
        local t, W = types.Weapon.record(weapon).type, types.Weapon.TYPE
        if (t==W.ShortBladeOneHand or t==W.LongBladeOneHand or t==W.BluntOneHand or t==W.AxeOneHand
            or t==W.BluntTwoWide or t==W.SpearTwoWide)
            and animation.hasGroup(self,'brutal_kick1h') then kickGroup='brutal_kick1h' end
        -- Dark Messiah has two kick heights: medium, and low when the view is pitched down.
        if kickGroup=='brutal_kick1h' and camera.getPitch() >= C.lowKickPitch
            and animation.hasGroup(self,'brutal_kick1h_low') then kickGroup='brutal_kick1h_low' end
    end
    if animation.hasGroup(self, kickGroup) then
        activeKick, activeKickSpeed = kickGroup, 1 / kickTimeScale()
        I.AnimationController.playBlendedAnimation(kickGroup, {
            priority = animation.PRIORITY.Scripted, blendMask = animation.BLEND_MASK.All,
            startPoint = 0, speed = activeKickSpeed, autoDisable = true, loops = 0,
        })
    end
    -- The engine plays Dark Messiah's Leg_Swoosh at its own sound-script level.
    if settings.get('combatSounds') then core.sound.playSound3d('Swish', self) end
    return true, lastResult
end

-- Stamina for a kick that landed: Dark Messiah's stamina_kick_npc or stamina_kick_all.
local function payKick(units)
    if not settings.get('staminaEnabled') then return end
    local fatigue = types.Actor.stats.dynamic.fatigue(self)
    fatigue.current = math.max(0, fatigue.current - fatigue.base * units / C.staminaMax)
end

-- Both a foot strike and slide contact use the same hit and native launch receipt.
-- Only the slide requests an unconditional ordinary knockdown on level ground.
local function strikeActor(target, direction, slide)
    payKick(C.kickStaminaNpc)
    local superKickId
    -- A slide only trips; its adrenaline is reserved for the sword finisher.
    if not slide and superKickAvailable and I.BrutalAdrenaline then
        superKickSerial = superKickSerial + 1
        local id = math.floor(core.getRealTime()*1000)*256 + superKickSerial%256
        if I.BrutalAdrenaline.reserveSuperKick(id) then superKickId = id end
    end
    target:sendEvent('Hit', {
        attacker = self, successful = true, sourceType = 'melee', strength = 1,
        brutalKick = true, brutalSlide = slide == true, brutalSuperKick = superKickId ~= nil,
        damage = {health = superKickId and C.superKickHealthDamage or C.healthDamage,
            fatigue = C.fatigueDamage},
        hitPos = target.position + util.vector3(0, 0, slide and 20 or 45),
    })
    if superKickId then
        core.sendGlobalEvent('BrutalNpcKick', {actor=target,attacker=self,superKickId=superKickId,
            velocity=direction*settings.get('superKickSpeed')
                +util.vector3(0,0,settings.get('superKickSpeed')*C.superKickLift)})
    elseif not slide then
        target:sendEvent('BrutalImpulse', {attacker = self, direction = direction, kick = true})
    end
end

-- Dark Messiah's shield sounds come in metal and wood; match the families the engine uses.
local METAL = {'iron', 'steel', 'silver', 'ebony', 'dwemer', 'dwarven', 'daedric', 'adamantium', 'orcish'}
local function shieldSound()
    local shield = types.Actor.getEquipment(self)[types.Actor.EQUIPMENT_SLOT.CarriedLeft]
    local name = shield and (shield.recordId .. ' ' .. (types.Armor.record(shield).model or '')):lower() or ''
    for _, family in ipairs(METAL) do
        if name:find(family, 1, true) then return 'Brutal Shield Metal' end
    end
    return 'Brutal Shield Wood'
end

-- Shield bash: attack while the shield is raised. The guard stays up throughout.
local function tryBash()
    if not settings.get('shieldBashEnabled') then return false end
    local equipped=types.Actor.getEquipment(self)
    local shield=equipped[types.Actor.EQUIPMENT_SLOT.CarriedLeft]
    if not shield or not types.Armor.objectIsInstance(shield)then return false end
    -- Two-handed weapons keep the shield in its slot, hidden and unavailable.
    local weapon=equipped[types.Actor.EQUIPMENT_SLOT.CarriedRight]
    if weapon and types.Weapon.objectIsInstance(weapon) then
        local kind=types.Weapon.record(weapon).type
        local W=types.Weapon.TYPE
        if kind~=W.ShortBladeOneHand and kind~=W.LongBladeOneHand
            and kind~=W.BluntOneHand and kind~=W.AxeOneHand then return false end
    end
    if bash or windup or sliding() or clinching() or I.UI.getMode() or not isBlockRaised or not isBlockRaised(self)
        or not animation.hasGroup(self, 'brutal_shieldbash')
        or animation.getCompletion(self, 'brutal_kick') ~= nil
        or animation.getCompletion(self, 'brutal_kick1h') ~= nil
        or animation.getCompletion(self, 'brutal_kick1h_low') ~= nil then return false end
    bash, bashWindup = 1 / kickTimeScale(), C.bashWindup
    I.AnimationController.playBlendedAnimation('brutal_shieldbash', {
        priority = animation.PRIORITY.Scripted, blendMask = animation.BLEND_MASK.UpperBody,
        startPoint = 0, speed = bash, autoDisable = true, loops = 0,
    })
    if settings.get('combatSounds') then core.sound.playSound3d('Weapon Swish', self) end
    return true
end

local function updateBash(dt, scale)
    if not bash then return end
    if animation.getCompletion(self, 'brutal_shieldbash') == nil then bash, bashWindup = nil, nil; return end
    if bash ~= 1 / scale then
        bash = 1 / scale
        animation.setSpeed(self, 'brutal_shieldbash', bash)
    end
    if not bashWindup then return end
    bashWindup = bashWindup - dt / scale
    if bashWindup > 0 then return end
    bashWindup = nil
    local target, direction = chooseTarget()
    bashes = bashes + 1
    if not target then return end
    -- A Hit from Lua is not subject to the target's own block: the bash breaks its guard.
    target:sendEvent('Hit', {
        attacker = self, successful = true, sourceType = 'melee', strength = 1,
        brutalBash = shieldSound(),
        damage = {health = C.bashHealthDamage, fatigue = C.bashFatigueDamage},
        hitPos = target.position + util.vector3(0, 0, 70),
    })
    target:sendEvent('BrutalImpulse', {attacker = self, direction = direction, speed = C.bashPushSpeed})
end

local function updateBlock()
    if not hasShieldBlock or not setShieldBlock then return end
    local held = (settings.get('activeBlockEnabled') or settings.get('parryEnabled')) and not I.UI.getMode() and not clinching() and input.isMouseButtonPressed(C.blockButton)
    -- Send changes only; the native controller decides whether the shield can rise.
    if held ~= blockHeld then
        blockHeld = held
        setShieldBlock(self, held)
    end
end

local function update(dt)
    if not settings.get('kickEnabled') then
        windup=nil
        if activeKick then animation.cancel(self,activeKick);activeKick,activeKickSpeed=nil,nil end
    end
    if not settings.get('shieldBashEnabled') then
        if bash then animation.cancel(self,'brutal_shieldbash') end
        bash,bashWindup,guardPress=nil,nil,nil
    end
    if clinching()then
        -- A queued local hit must not leak into the paired action after its
        -- animation was rejected by native ownership.
        windup,bash,bashWindup,activeKick,activeKickSpeed=nil,nil,nil,nil,nil
    end
    updateBlock()
    local use = not I.UI.getMode() and input.getBooleanActionValue('Use')
    -- Dark Messiah bashes on a short click from the raised shield; held past
    -- mintime_to_bigslash the native controller strikes the charged swing instead.
    if use and not usePrev then guardPress = blockHeld and 0 or nil end
    if use and guardPress then guardPress = guardPress + math.max(dt, 0) end
    if not use and usePrev and guardPress and blockHeld and guardPress < C.guardChargeTime then tryBash() end
    if not use then guardPress = nil end
    usePrev = use
    if dt <= 0 then return end
    -- Kick timers and animation run in real time while the world is slowed.
    local scale = kickTimeScale()
    updateBash(dt, scale)
    if activeKick then
        if animation.getCompletion(self, activeKick) == nil then activeKick = nil
        elseif activeKickSpeed ~= 1 / scale then
            activeKickSpeed = 1 / scale
            animation.setSpeed(self, activeKick, activeKickSpeed)
        end
    end
    local kickDt = dt / scale
    cooldown = math.max(0, cooldown - kickDt)
    if windup then
        windup = windup - kickDt
        if windup <= 0 then
            windup = nil
            if types.Actor.isDead(self) or not types.Actor.canMove(self) or types.Actor.isSwimming(self) then return end
            local target, direction = chooseTarget()
            if not target then
                -- No living actor: the same kick can still strike a ragdoll corpse or head.
                core.sendGlobalEvent('BrutalRagdollKick', {attacker = self,
                    origin = self.position + util.vector3(0, 0, 40), direction = direction})
            end
            if not target and physicsAvailable and settings.get('dynamicProps') then
                -- The global engine API lazily creates a native Bullet body.
                local from = self.position + util.vector3(0, 0, 40)
                local best = C.reach
                for _, item in ipairs(nearby.items) do
                    local offset = item.position - self.position
                    local flat = util.vector3(offset.x, offset.y, 0)
                    local distance = flat:length()
                    if not types.Light.objectIsInstance(item) and distance > 1 and distance < best and math.abs(offset.z) < 80
                        and flat:normalize():dot(direction) >= C.cone then
                        local ray = nearby.castRay(from, item:getBoundingBox().center, {ignore = self})
                        if not ray.hit or ray.hitObject == item then target, best = item, distance end
                    end
                end
                if target then
                    payKick(C.kickStaminaOther)
                    core.sendGlobalEvent('BrutalPropImpulse', {item = target, attacker = self, direction = direction})
                    lastResult = 'prop'
                    strikes = strikes + 1
                    return
                end
            end
            lastResult = target and 'hit' or 'miss'
            strikes = strikes + 1
            if target then
                strikeActor(target, direction, false)
            end
        end
    end
end

input.registerTriggerHandler(bindings.KICK,async:callback(tryKick))

return {
    interfaceName = 'BrutalMorrowind',
    interface = {
        version = 1,
        kick = tryKick,
        bash = tryBash,
        slideHit = function(target, direction)
            if not settings.get('slideEnabled') or not sliding() or not target or not target:isValid() or types.Actor.isDead(target)
                or not types.Actor.objectIsInstance(target) or direction:length() < .01 then return false end
            strikeActor(target, direction:normalize(), true)
            return true
        end,
        status = function() return {cooldown = cooldown, result = lastResult, strikes = strikes,
            physicsAvailable = physicsAvailable, superKickAvailable = superKickAvailable,
            bashing = bash ~= nil, bashes = bashes, ragdollHits = ragdollHits} end,
    },
    eventHandlers = {
        BrutalPhysicsAvailable = function(data)
            physicsAvailable, superKickAvailable = data.available, data.superKick == true
        end,
        -- The engine confirmed that this kick struck a ragdoll corpse or severed head.
        BrutalRagdollKickLanded = function(data)
            if not settings.get('kickEnabled') then return end
            payKick(C.kickStaminaOther)
            lastResult = data.head and 'ragdoll-head' or 'ragdoll'
            ragdollHits = ragdollHits + 1
        end,
        -- The engine: this kick sent a living NPC over a ledge into a ragdoll.
        BrutalKickRagdoll = function()
            if not settings.get('kickEnabled') then return end
            -- Reward the confirmed fall without changing simulation speed.
            self:sendEvent('BrutalAdrenalineFill', {})
        end,
    },
    engineHandlers = {
        onUpdate = update,
        -- Save only cooldown. A pending attack is intentionally cancelled on reload.
        onSave = function() return {cooldown = cooldown} end,
        onLoad = function(data)
            cooldown = data and math.min(C.cooldown, math.max(0, data.cooldown or 0)) or 0
            windup, lastResult = nil, 'ready'
            bash, bashWindup = nil, nil
        end,
    },
}
