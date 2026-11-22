-- Vertical gauge right of the native health/magicka/fatigue bars, exactly their height.
-- The engine's HUD layout moves the weapon/spell/sneak boxes 18 units right to make room,
-- leaving the native 4-unit gap between boxes on both sides of the gauge.
local ui=require('openmw.ui')
local util=require('openmw.util')
local I=require('openmw.interfaces')
local v2=util.vector2
local track=ui.texture{path='textures/menu_bar_gray.dds'}
-- openmw_hud.layout: the bars span x 13..78 and y 146..188 of a 200-unit bottom-anchored HUD.
local LEFT,TOP,WIDTH,HEIGHT=82,-54,14,42
local INNER=HEIGHT-2
local GOLD,BRIGHT=util.color.rgb(1,.8,.25),util.color.rgb(1,.97,.75)
local element,lastAmount,lastReady,lastPulse,shown=nil,nil,nil,nil,false
local function destroy()
    if element then element:destroy();element=nil end
    shown=false
end
local function draw(amount,capacity,visible,time)
    if not visible then destroy();return end
    local ready=amount>=capacity
    local pulse=ready and math.floor((0.5+0.5*math.sin(time*4))*12)/12 or 0
    if not element then
        element=ui.create{
            type=ui.TYPE.Widget,layer='HUD',name='BrutalAdrenaline',
            -- HUD-layer coordinates inherit native GUI scaling, like the layout itself.
            props={position=v2(LEFT,TOP),relativePosition=v2(0,1),size=v2(WIDTH,HEIGHT)},
            content=ui.content{
                {type=ui.TYPE.Image,props={resource=track,position=v2(1,1),size=v2(WIDTH-2,INNER),color=util.color.rgb(.11,.09,.06)}},
                {name='fill',type=ui.TYPE.Image,props={resource=track,position=v2(1,1+INNER),size=v2(WIDTH-2,0),color=GOLD}},
                {type=ui.TYPE.Widget,template=I.MWUI.templates.borders,props={position=v2(0,0),size=v2(WIDTH,HEIGHT)}},
            },
        }
        lastAmount,lastReady,lastPulse=nil,nil,nil
    end
    shown=true
    if amount==lastAmount and ready==lastReady and pulse==lastPulse then return end
    local fill=element.layout.content.fill.props
    local height=math.floor(INNER*math.max(0,math.min(1,amount/capacity))+.5)
    fill.position=v2(1,1+INNER-height)
    fill.size=v2(WIDTH-2,height)
    fill.color=ready and util.color.rgb(GOLD.r+(BRIGHT.r-GOLD.r)*pulse,GOLD.g+(BRIGHT.g-GOLD.g)*pulse,
        GOLD.b+(BRIGHT.b-GOLD.b)*pulse) or GOLD
    element:update()
    lastAmount,lastReady,lastPulse=amount,ready,pulse
end
return {draw=draw,destroy=destroy,isVisible=function() return shown end}
