-- Sprint + the mapped Sneak action. The native controller owns animation,
-- momentum, the shortened collision hull and the safe return to standing.
local self = require('openmw.self')
local types = require('openmw.types')
local input = require('openmw.input')
local util = require('openmw.util')
local nearby = require('openmw.nearby')
local animation = require('openmw.animation')
local core = require('openmw.core')
local I = require('openmw.interfaces')
local speedFx = require('scripts.brutal_morrowind.slide_speed_fx')
local settings = require('scripts.brutal_morrowind.settings')
local ok, request, query, trip = pcall(function()
    return types.Actor.requestKineticSlide, types.Actor.getKineticSlide, types.Actor.tripKineticSlideNpc
end)
local available = ok and type(request) == 'function' and type(query) == 'function' and type(trip) == 'function'
local previousSneak, previousPhase = false, 0
local previousPosition, direction, seen, contactSpeed
local previousSimulationTime
local starts, hits, tripRequests, result = 0, 0, 0, 'ready'

local function status()
    local state = available and query(self) or {phase=0,moving=false,low=false,blend=0}
    state.available, state.starts, state.hits, state.result = available, starts, hits, result
    state.tripRequests = tripRequests
    state.speedFx = speedFx.status()
    return state
end

local function sprinting()
    if settings.get('movementEnabled') then
        return I.BrutalStamina and I.BrutalStamina.status().sprinting
    end
    -- The stock controls already resolve Run versus AlwaysRun. Use that
    -- actual run state so either original running preference supports slides.
    return self.controls.run and self.controls.movement > .3
end

local function start()
    if not settings.get('slideEnabled') then
        result = 'disabled'
        return false
    end
    if not available or I.UI.getMode() or types.Actor.isDead(self) or not types.Actor.canMove(self)
        or types.Actor.isSwimming(self) or not types.Actor.isOnGround(self)
        or not sprinting()
        or self.controls.movement <= .3 or math.abs(self.controls.sideMovement) > .3
        or query(self).phase ~= 0
        or not (animation.hasGroup(self,'brutal_slide') or types.Actor.getBrutalOption('classicFirstPerson')) then
        result = 'unavailable'
        return false
    end
    request(self)
    result = 'requested'
    return true
end

local function processInput()
    if not available then return end
    local sneak = not I.UI.getMode() and input.getBooleanActionValue('Sneak')
    if sneak and not previousSneak then start() end
    previousSneak = sneak
end

local function hasHandWeapon()
    if types.Actor.getStance(self)~=types.Actor.STANCE.Weapon then return false end
    local weapon=types.Actor.getEquipment(self)[types.Actor.EQUIPMENT_SLOT.CarriedRight]
    if not weapon or not types.Weapon.objectIsInstance(weapon)then return false end
    local kind,W=types.Weapon.record(weapon).type,types.Weapon.TYPE
    return kind==W.ShortBladeOneHand or kind==W.LongBladeOneHand
        or kind==W.AxeOneHand or kind==W.BluntOneHand
        or (not types.Actor.getBrutalOption('classicFirstPerson')
            and (kind==W.BluntTwoWide or kind==W.SpearTwoWide))
end

local function handStrikeRequested()
    return input.getBooleanActionValue('Use') and hasHandWeapon()
end

local function update(dt)
    if not settings.get('slideEnabled') then
        previousPosition,seen,previousPhase=nil,nil,0
        return
    end
    if not available then return end
    local state = query(self)
    if state.phase ~= 0 and previousPhase == 0 then
        starts, result = starts + 1, 'sliding'
        previousPosition, seen, contactSpeed = self.position, {}, 300
        direction = self.rotation * util.vector3(0,1,0)
        direction = util.vector3(direction.x,direction.y,0):normalize()
    end
    previousPhase = state.phase
    if state.phase == 0 then
        previousPosition, seen = nil, nil
        return
    end
    if dt <= 0 or I.UI.getMode() or not previousPosition then return end
    local step = self.position-previousPosition
    step = util.vector3(step.x,step.y,0)
    -- The rising exit still coasts; a new NPC cannot pin its remaining motion.
    if not state.moving and state.phase~=3 then return end
    local measuredSpeed = step:length()/dt
    if measuredSpeed>20 then contactSpeed=measuredSpeed end
    -- Sword ownership suppresses foot damage, never the physical knockdown.
    -- An NPC must yield to the slide while the real blade is still preparing.
    local handOwned = state.attacking==true or handStrikeRequested()
    -- Three low swept rays span the foot's width. They follow actual travelled
    -- distance even at low frame rates. The first physical obstruction wins, so
    -- NPCs behind walls cannot receive this contact.
    local side = util.vector3(-direction.y,direction.x,0)
    local candidates = {}
    for _, lane in ipairs({-14,0,14}) do
        local offset = util.vector3(0,0,20) + side*lane
        local hit = nearby.castRay(previousPosition+offset, self.position+offset+direction*66, {ignore=self})
        local target = hit.hitObject
        if hit.hit and target and types.Actor.objectIsInstance(target) and not types.Actor.isDead(target)
            and not seen[target.id] then
            candidates[target.id] = target
        end
    end
    previousPosition = self.position
    -- An adrenaline slide with a one-handed weapon is a clean trip: the boot
    -- leaves health alone so that a click can still deliver the finisher.
    local finisherReady = handOwned or (hasHandWeapon() and I.BrutalAdrenaline
        and I.BrutalAdrenaline.status().ready)
    for id, target in pairs(candidates) do
        local speed=math.max(150,math.min(500,contactSpeed))
        -- Queue directly in this synchronized input frame, before another
        -- physics batch can run into the standing NPC at a low frame rate.
        -- The engine throws the NPC ahead of the slide with its legs swept
        -- out, so it lands on its back in front of the player. A slide never delivers a super kick, so
        -- full adrenaline stays reserved for a sword finisher.
        -- A ready finisher keeps the thrown body within sword reach: it first
        -- travels at about the slide's own speed instead of outrunning it.
        local throw=finisherReady and .8 or 1
        trip(self,target,direction*(speed*throw)+util.vector3(0,0,speed*.25*throw))
        tripRequests=tripRequests+1
        seen[id]=true
        -- An ordinary slide deals the boot's charge-free foot damage at contact.
        if not finisherReady and I.BrutalMorrowind.slideHit(target,direction) then
            hits, result = hits + 1, 'hit'
        end
    end
end

local function frame()
    processInput()
    local now=core.getSimulationTime()
    local dt=previousSimulationTime and math.max(0,now-previousSimulationTime) or 0
    previousSimulationTime=now
    update(dt)
    if settings.get('slideEnabled') and settings.get('slideFxEnabled') then
        speedFx.draw(available and query(self) or nil)
    else speedFx.destroy() end
end

return {
    interfaceName='BrutalSlide',
    interface={version=2,start=start,status=status},
    -- Builtin controls process crouch in synchronized onFrame. Submit the slide
    -- in that same input frame, before crouching replaces the native sprint.
    engineHandlers={onFrame=frame,onLoad=function()
        previousSneak,previousPhase,previousPosition,seen=false,0,nil,nil
        previousSimulationTime=nil
        starts,hits,tripRequests,result=0,0,0,'ready'
        speedFx.destroy()
    end},
}
