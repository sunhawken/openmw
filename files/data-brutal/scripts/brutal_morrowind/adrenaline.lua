-- The configured number of damaging hits fills one charge; a charged strike spends it.
local self=require('openmw.self')
local core=require('openmw.core')
local types=require('openmw.types')
local ui=require('openmw.ui')
local I=require('openmw.interfaces')
local hud=require('scripts.brutal_morrowind.adrenaline_ui')
local fx=require('scripts.brutal_morrowind.adrenaline_fx')
local settings=require('scripts.brutal_morrowind.settings')
local CAPACITY,MULTIPLIER,MIN_STRENGTH=100,1,.8
local function gain() return CAPACITY/settings.get('adrenalineHits') end
-- DM's base charge is20 points: its1-point loss every20 simulation seconds
-- becomes5 points on this percent gauge. An actual increase resets the timer.
local DECAY_INTERVAL,DECAY_AMOUNT=20,5
-- Keep the reference duration/factor, but start on the exact lethal receipt.
-- The user requested removing the late .2-second pause so the cut is readable.
local SLOW_DELAY,SLOW_DURATION,SLOW_FACTOR=0,2,.1
local meter,serial,gains,spent=0,0,0,0
local decayElapsed=0
local slowAttackId
local slowActive,slowStarted,slowEnded=false,0,0
local reservation,reservedWeapon=nil,nil
local pendingKick
local superKicks,rejectedKicks,lastKickId=0,0,nil
local seen,seenOrder={},{}
local seenFinishers,finisherOrder={},{}
local arm,cancel,query
local arrestEffectState
local setEffectActive=types.Actor.setAdrenalineEffectActive
local available=pcall(function()
    arm=types.Actor.armPowerStrike
    cancel=types.Actor.cancelPowerStrike
    query=types.Actor.getPowerStrikeToken
    assert(arm and cancel and query)
end)
local MELEE={}
for _,name in ipairs({'ShortBladeOneHand','LongBladeOneHand','LongBladeTwoHand','BluntOneHand','BluntTwoClose',
    'BluntTwoWide','SpearTwoWide','AxeOneHand','AxeTwoHand'}) do MELEE[types.Weapon.TYPE[name]]=true end
local function melee(weapon)
    return weapon~=nil and weapon:isValid() and types.Weapon.objectIsInstance(weapon)
        and MELEE[types.Weapon.record(weapon).type]==true
end
local function fastShare(weapon)
    if not melee(weapon) then return 1 end
    local kind=types.Weapon.record(weapon).type
    if kind==types.Weapon.TYPE.ShortBladeOneHand then return .5/1.5 end
    if kind==types.Weapon.TYPE.SpearTwoWide or kind==types.Weapon.TYPE.BluntTwoWide then return .5 end
    return .5/3
end
local function visible()
    return settings.get('adrenalineEnabled') and not types.Actor.isDead(self) and not I.UI.getMode() and I.UI.isHudVisible()
        and types.Player.getControlSwitch(self,types.Player.CONTROL_SWITCH.Controls)
        and types.Player.getControlSwitch(self,types.Player.CONTROL_SWITCH.Fighting)
end
local function syncArrestEffect(show)
    local active=show and (meter>=CAPACITY or fx.isVisible() or slowAttackId~=nil)
    if setEffectActive and active~=arrestEffectState then
        setEffectActive(self,active)
        arrestEffectState=active
    end
end
local function clearReservation()
    if available then
        local nativeToken=query(self)
        if nativeToken~='' then cancel(self,nativeToken) end
        if reservation and reservation~=nativeToken then cancel(self,reservation) end
    end
    reservation,reservedWeapon=nil,nil
end
local function cancelSlowMotion(reason)
    if slowAttackId then
        core.sendGlobalEvent('BrutalAdrenalineSlowMotionCancel',
            {player=self,attackId=slowAttackId,reason=reason})
    end
    slowAttackId,slowActive=nil,false
end
local function disable()
    meter,decayElapsed,pendingKick=0,0,nil
    clearReservation()
    cancelSlowMotion('disabled')
    hud.destroy();fx.destroy();syncArrestEffect(false)
end
local function update(dt)
    if not settings.get('adrenalineEnabled') then disable();return end
    if not settings.get('slowMotionEnabled') or not settings.get('finisherEnabled') then cancelSlowMotion('disabled') end
    if types.Actor.isDead(self) then meter,decayElapsed,pendingKick=0,0,nil;cancelSlowMotion('death')
    elseif meter>0 and not pendingKick and settings.get('adrenalineDecay') then
        decayElapsed=decayElapsed+dt
        while decayElapsed>=DECAY_INTERVAL do
            decayElapsed=decayElapsed-DECAY_INTERVAL
            meter=math.max(0,meter-DECAY_AMOUNT)
        end
    end
    local weapon=types.Actor.getEquipment(self)[types.Actor.EQUIPMENT_SLOT.CarriedRight]
    if not settings.get('superKickEnabled') then pendingKick=nil end
    local eligible=settings.get('finisherEnabled') and available and not pendingKick and meter>=CAPACITY and visible() and types.Actor.canMove(self)
        and not (I.BrutalClinch and I.BrutalClinch.status().active)
        and not types.Actor.isSwimming(self) and types.Actor.getStance(self)==types.Actor.STANCE.Weapon
        and melee(weapon)
    if not eligible then clearReservation();return end
    if reservedWeapon and reservedWeapon~=weapon then clearReservation() end
    if not reservation then
        serial=serial+1
        reservation=('brutal:%s:%d'):format(tostring(core.getSimulationTime()),serial)
        reservedWeapon=weapon
    end
    if query(self)~=reservation then arm(self,reservation,MULTIPLIER,MIN_STRENGTH) end
end
-- Reserve at the actual foot contact, cancelling the charged sword channel.
-- A native receipt spends only after a real ragdoll handoff. Rejected targets
-- retain the charge; a miss never reserves it in the first place.
local function reserveSuperKick(id)
    if not settings.get('superKickEnabled') or not settings.get('adrenalineEnabled') or not settings.get('ragdollEnabled')
        or pendingKick or meter<CAPACITY or not visible() or not types.Actor.canMove(self)
        or types.Actor.isSwimming(self) or type(id)~='number' or id<1 or id%1~=0
        or id>9007199254740991 then return false end
    clearReservation()
    pendingKick=id
    return true
end
local function superKickResolved(e)
    if not settings.get('adrenalineEnabled') or not settings.get('superKickEnabled') then pendingKick=nil;return end
    if not pendingKick or e.kickId~=pendingKick then return end
    lastKickId,pendingKick=e.kickId,nil
    if e.accepted~=true then rejectedKicks=rejectedKicks+1;return end
    meter,decayElapsed=0,0
    spent,superKicks=spent+1,superKicks+1
    clearReservation()
    self:sendEvent('BrutalAdrenalineSpent',{kickId=e.kickId,superKick=true})
end
local function damaged(e)
    if not settings.get('adrenalineEnabled') then return end
    if types.Actor.isDead(self) or not e.successful or not e.targetWasAlive
        or e.sourceType~='melee' or not e.target or not e.target:isValid()
        or not types.Actor.objectIsInstance(e.target) or not melee(e.weapon)
        or type(e.healthDamage)~='number' or e.healthDamage<=0 then return end
    if e.powered then return end
    -- Native attack ids distinguish real swings and prevent duplicate delivery.
    if not e.attackId or e.attackId==0 or seen[e.attackId] then return end
    seen[e.attackId]=true;seenOrder[#seenOrder+1]=e.attackId
    if #seenOrder>128 then seen[table.remove(seenOrder,1)]=nil end
    local before=meter
    -- first_basic.qct adrenalineadd: a click gives .5 where a charged swing gives 3 (sword), 1.5 (daggers), 1 (staff).
    meter=meter+gain()*(e.fast and fastShare(e.weapon) or 1)
    if meter>=CAPACITY-.001 then meter=CAPACITY end
    if meter>before then decayElapsed=0 end
    gains=gains+1
    if before<CAPACITY and meter>=CAPACITY then self:sendEvent('BrutalAdrenalineReady',{}) end
end
local function confirmedFinisher(e)
    if not settings.get('finisherEnabled') or not settings.get('adrenalineEnabled') or not settings.get('slowMotionEnabled') then return end
    if not e.powered or not e.deadAfter or not e.successful or not e.targetWasAlive
        or e.sourceType~='melee' or not e.target or not e.target:isValid()
        or not types.Actor.objectIsInstance(e.target) or not melee(e.weapon)
        or type(e.healthDamage)~='number' or e.healthDamage<=0
        or not e.attackId or e.attackId==0 or seenFinishers[e.attackId] then return end
    seenFinishers[e.attackId]=true;finisherOrder[#finisherOrder+1]=e.attackId
    if #finisherOrder>128 then seenFinishers[table.remove(finisherOrder,1)]=nil end
    if slowAttackId or not visible() then return end
    slowAttackId=e.attackId
    core.sendGlobalEvent('BrutalAdrenalineSlowMotion',
        {player=self,attackId=slowAttackId,duration=SLOW_DURATION,factor=SLOW_FACTOR})
end
-- Fill the gauge at once (a confirmed ledge kick, or the console), or set it to an
-- amount. Readiness plays the usual cue and the fullscreen effect follows the ready state.
local function fill(amount)
    if not settings.get('adrenalineEnabled') or types.Actor.isDead(self) then return false end
    local before=meter
    meter,decayElapsed=math.max(0,math.min(CAPACITY,tonumber(amount) or CAPACITY)),0
    if meter<CAPACITY then clearReservation() end
    if before<CAPACITY and meter>=CAPACITY then self:sendEvent('BrutalAdrenalineReady',{}) end
    syncArrestEffect(visible())
    return true
end
local function resolved(e)
    if not settings.get('adrenalineEnabled') or not settings.get('finisherEnabled') then clearReservation();return end
    if not e.powered or not e.attackId or e.attackId==0 or seen[e.attackId] then return end
    seen[e.attackId]=true;seenOrder[#seenOrder+1]=e.attackId
    if #seenOrder>128 then seen[table.remove(seenOrder,1)]=nil end
    -- Native receipts authenticate the swing. DM spends on a living opponent's
    -- contact (including a parry), while an empty swing retains the charge.
    if not e.successful and not e.blocked then clearReservation();return end
    -- Do not require a current reservation: a menu/cancel event can arrive after
    -- an already-resolved native strike but before its notification reaches Lua.
    meter,decayElapsed=0,0;spent=spent+1
    clearReservation()
    self:sendEvent('BrutalAdrenalineSpent',{attackId=e.attackId,token=e.token})
end
return {
    interfaceName='BrutalAdrenaline',
    interface={version=4,fill=fill,reserveSuperKick=reserveSuperKick,status=function()
        return {amount=meter,capacity=CAPACITY,ready=meter>=CAPACITY and not pendingKick,apiAvailable=available,
            superKickPending=pendingKick,superKicks=superKicks,rejectedKicks=rejectedKicks,lastKickId=lastKickId,
            token=available and query(self) or '',gains=gains,spent=spent,hudVisible=hud.isVisible(),
            fxVisible=fx.isVisible(),decayElapsed=decayElapsed,decayInterval=DECAY_INTERVAL,
            arrestDeferred=arrestEffectState==true,
            decayAmount=DECAY_AMOUNT,hitsToReady=math.ceil((CAPACITY-meter)/gain()-.001),
            slowPending=slowAttackId~=nil and not slowActive,slowActive=slowActive,slowStarted=slowStarted,slowEnded=slowEnded,
            slowDelay=SLOW_DELAY,slowDuration=SLOW_DURATION,slowFactor=SLOW_FACTOR}
    end},
    eventHandlers={BrutalDamagingHit=damaged,BrutalMeleeResolved=resolved,
        BrutalSuperKickResolved=superKickResolved,
        BrutalAdrenalineFill=function() fill() end,
        BrutalFinisherConfirmed=confirmedFinisher,
        BrutalAdrenalineSlowMotionStarted=function(e)
            if not settings.get('adrenalineEnabled') or not settings.get('slowMotionEnabled') then cancelSlowMotion('disabled');return end
            if e.attackId==slowAttackId then slowActive=true;slowStarted=slowStarted+1 end
        end,
        BrutalAdrenalineSlowMotionEnded=function(e)
            if e.attackId==slowAttackId then slowAttackId,slowActive=nil,false;slowEnded=slowEnded+1 end
        end,
        BrutalAdrenalineSlowMotionRejected=function(e)
            if e.attackId==slowAttackId then slowAttackId,slowActive=nil,false end
        end,
    },
    engineHandlers={
        onUpdate=update,
        -- Console: "adrenaline" fills the gauge, "adrenaline 0".."adrenaline 100" sets it (the
        -- engine forwards these to Lua); luap: I.BrutalAdrenaline.fill(amount).
        onConsoleCommand=function(mode,command)
            local amount=mode=='' and command:lower():match('^%s*adrenaline%s*(%d*)%s*$')
            if not amount then return end
            if fill(amount~='' and tonumber(amount) or nil) then
                ui.printToConsole(('Adrenaline %d/%d'):format(meter,CAPACITY),ui.CONSOLE_COLOR.Success)
            else ui.printToConsole('Adrenaline: the player is dead',ui.CONSOLE_COLOR.Error) end
        end,
        onFrame=function(dt)
            if not settings.get('adrenalineEnabled') then disable();return end
            if not settings.get('finisherEnabled') then clearReservation() end
            if not settings.get('superKickEnabled') then pendingKick=nil end
            if not settings.get('slowMotionEnabled') or not settings.get('finisherEnabled') then cancelSlowMotion('disabled') end
            -- Menus may pause simulation updates, so cancel and hide here too.
            local show=visible()
            if not show then clearReservation();cancelSlowMotion('hidden') end
            hud.draw(meter,CAPACITY,show and settings.get('adrenalineHudEnabled'),core.getRealTime())
            if settings.get('adrenalineFxEnabled') then fx.draw(meter>=CAPACITY,show,core.getSimulationTime(),dt)
            else fx.destroy() end
            syncArrestEffect(show)
        end,
        -- A save between contact and its queued receipt must not duplicate a
        -- reserved charge. Pending attacks themselves are transient on reload.
        onSave=function() return {amount=pendingKick and 0 or meter,decayElapsed=decayElapsed} end,
        onLoad=function(data)
            pendingKick,lastKickId=nil,nil
            superKicks,rejectedKicks=0,0
            clearReservation()
            cancelSlowMotion('load')
            local value=data and data.amount or 0
            meter=type(value)=='number' and value==value and math.max(0,math.min(CAPACITY,value)) or 0
            local decay=data and data.decayElapsed or 0
            decayElapsed=type(decay)=='number' and decay==decay and math.max(0,math.min(DECAY_INTERVAL,decay)) or 0
            gains,spent,serial=0,0,0
            slowStarted,slowEnded=0,0
            seen,seenOrder={},{}
            seenFinishers,finisherOrder={},{}
            hud.destroy()
            fx.destroy()
            arrestEffectState=nil
            syncArrestEffect(false)
        end,
    },
}
