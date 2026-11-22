local settings=require('scripts.brutal_morrowind.settings')
-- Dark Messiah's sword-versus-sword contest. Native combat owns the pair and
-- counts actual, rebound Attack button edges; Lua supplies the effects/reward.
local self=require('openmw.self')
local core=require('openmw.core')
local types=require('openmw.types')
local animation=require('openmw.animation')
local camera=require('openmw.camera')
local I=require('openmw.interfaces')
local fx=require('scripts.brutal_morrowind.clinch_fx')
local headLoaded,head=pcall(require,'scripts.brutal_assets.clinch_head')
local configured=false
local awarded,lastResult=0,nil
local wins,losses,cancels=0,0,0
local getState=types.Actor.getSwordClinchState
local configure=types.Actor.configureSwordClinch
local cancel=types.Actor.cancelSwordClinch
local available=getState~=nil and configure~=nil and cancel~=nil
-- Original animation and timing; only the hidden click requirement is easier.
-- Entry clicks count too, and the outcome is shown at the original clip end.
local REQUIRED,DURATION,DISTANCE=3,43/30,44*1.84265547703229
local function visible()
    return settings.get('clinchEnabled') and not types.Actor.isDead(self) and not I.UI.getMode()
        and types.Player.getControlSwitch(self,types.Player.CONTROL_SWITCH.Controls)
        and types.Player.getControlSwitch(self,types.Player.CONTROL_SWITCH.Fighting)
end
local function headMotion(state)
    if not headLoaded or camera.getMode()~=camera.MODE.FirstPerson then return end
    local clip=head[state.group]
    if not clip or not clip.frames or #clip.frames<1 then return end
    local position=state.phase=='align' and 0 or math.max(0,math.min(#clip.frames-1,state.elapsed*clip.fps))
    local lo=math.floor(position)+1
    local a,b=clip.frames[lo],clip.frames[math.min(lo+1,#clip.frames)]
    local f=position-math.floor(position)
    -- The ordinary camera update clears extra angles before this onFrame.
    -- Add the source head track after its usual bob/roll, without rotating the
    -- physical body or applying a cumulative offset from the previous frame.
    camera.setExtraPitch(camera.getExtraPitch()+a[1]+(b[1]-a[1])*f)
    camera.setExtraYaw(camera.getExtraYaw()+a[2]+(b[2]-a[2])*f)
    camera.setExtraRoll(camera.getExtraRoll()+a[3]+(b[3]-a[3])*f)
end
return {
    interfaceName='BrutalClinch',
    interface={version=2,status=function()
        local s=available and getState(self) or {active=false,phase='idle'}
        s.available=available;s.configured=configured
        s.wins=wins;s.losses=losses;s.cancels=cancels;s.lastReceipt=lastResult
        s.blur=fx.status().weight
        s.progress=math.min(1,(s.clicks or 0)/math.max(1,s.requiredClicks or REQUIRED))
        return s
    end},
    eventHandlers={BrutalClinchResolved=function(e)
        if not settings.get('clinchEnabled') or not available or not e or e.id==nil then return end
        local s=getState(self)
        -- Reward the native receipt once, including after the player's short
        -- exit while the opponent is still playing its genuine lost recoil.
        if s.lastResultId~=e.id or s.lastResult~=e.result or lastResult==e.id then return end
        lastResult=e.id
        print(('BRUTAL_CLINCH: result=%s reason=%s clicks=%d/%d'):format(e.result,e.reason or '',e.clicks or 0,e.requiredClicks or REQUIRED))
        if e.result=='win' then
            wins=wins+1
            if awarded~=e.id and I.BrutalAdrenaline then
                awarded=e.id;I.BrutalAdrenaline.fill()
            end
        elseif e.result=='lose' then losses=losses+1
        elseif e.result=='cancel' then cancels=cancels+1 end
    end},
    engineHandlers={
        onFrame=function()
            if not available then return end
            if not settings.get('clinchEnabled') then
                if configured then configure(self,{enabled=false});cancel(self);configured=false end
                fx.destroy();return
            end
            if not configured and animation.hasGroup(self,'brutal_clinch') then
                configure(self,{enabled=true,requiredClicks=REQUIRED,duration=DURATION,distance=DISTANCE})
                configured=true
            end
            local s=getState(self)
            local show=visible()
            if s.active and show then headMotion(s) end
            if settings.get('clinchFxEnabled') then fx.draw(s,show) else fx.destroy() end
        end,
        onLoad=function()
            if available then cancel(self) end
            configured=false;awarded,lastResult=0,nil;wins,losses,cancels=0,0,0
            fx.destroy()
        end,
    },
}
