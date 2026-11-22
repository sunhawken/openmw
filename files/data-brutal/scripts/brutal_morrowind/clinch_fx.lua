local postprocessing=require('openmw.postprocessing')
local core=require('openmw.core')
local ui=require('openmw.ui')
local shader,checked=nil,false
local lossAt,previousId=nil,nil
local size,weight=nil,0
local warmFrames=0
local function reset()
    lossAt,previousId=nil,nil
    weight=0
    if shader then shader:setFloat('brutalClinchHistory',0)end
end
local function draw(state,visible)
    if not visible then reset();return end
    if not checked then
        checked=true;shader=postprocessing.load('brutal_clinch_blur')
    end
    if not shader then return end
    -- Keep the warmed history chain resident. Toggling a technique reallocates
    -- its targets; a parry must not introduce a compilation/rebuild hitch.
    if not shader:isEnabled()then
        shader:setFloat('brutalClinchHistory',0);shader:enable();warmFrames=0
    end
    local screen=ui.screenSize()
    local resized=size and (screen.x~=size.x or screen.y~=size.y)
    size=screen
    if resized then warmFrames=0 end
    warmFrames=warmFrames+1
    if state.phase=='lose'and state.id~=previousId then
        lossAt,previousId=core.getSimulationTime(),state.id
    end
    local remaining=lossAt and 1.4-(core.getSimulationTime()-lossAt)or 0
    -- Source: 2s break-parry cvar * .7 time coefficient. The history's
    -- alpha is min(max((remaining-.6)*1280,0),192)/255; intensity .35.
    weight=warmFrames>=4 and math.min(192,math.max(0,(remaining-.6)*1280))/255 or 0
    shader:setFloat('brutalClinchHistory',weight)
end
local function destroy()
    reset()
    if shader and shader:isEnabled() then shader:disable() end
end
return {draw=draw,destroy=destroy,status=function()return {weight=weight,loaded=shader~=nil}end}
