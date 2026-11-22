-- Original Dark Messiah fullscreen_adrenaline AnimatedTexture, composited additively.
-- The atlas is imported from the user's own VPKs into the user's data directory.
local postprocessing=require('openmw.postprocessing')
local vfs=require('openmw.vfs')
local shader,shown,checked=nil,false,false
local alpha=0
local warmed,warming,warmFrames=false,false,0
local function setEnabled(enabled)
    -- Native enable reorders the chain and rebuilds its render targets even
    -- when this technique is already enabled. Change membership only once.
    if shader and shader:isEnabled()~=enabled then
        if enabled then shader:enable() else shader:disable() end
    end
end
local function destroy()
    setEnabled(false)
    shown=false
    alpha=0
    warming,warmFrames=false,0
end
local function draw(ready,visible,time,dt)
    if not visible then destroy();return end
    if not checked then
        checked=true
        if vfs.fileExists('textures/brutal_dm/adrenaline_ready_atlas.png') then
            shader=postprocessing.load('brutal_adrenaline')
        end
    end
    if shader and not warmed then
        -- With alpha zero this exact postprocess is the identity, including
        -- scene alpha. Draw it during scene setup so a driver cannot defer its
        -- first pipeline compilation until the tenth combat hit. Three later
        -- onFrame calls cover the renderer's alternating postprocess canvases.
        if not warming then
            shader:setInt('brutalFrame',0)
            shader:setFloat('brutalAlpha',0)
            setEnabled(true)
            warming,warmFrames=true,0
        else
            warmFrames=warmFrames+1
            if warmFrames>=3 then
                warmed,warming=true,false
                setEnabled(false)
            end
        end
        shown=false
        return
    end
    -- As in Dark Messiah: additive intensity clamped to 0..1, approaching
    -- readiness at 5 per second.
    alpha=math.max(0,math.min(1,alpha+(ready and 1 or -1)*5*dt))
    if alpha==0 then
        setEnabled(false)
        shown=false;return
    end
    if not shader then return end
    shader:setInt('brutalFrame',math.floor(time*25)%25)
    shader:setFloat('brutalAlpha',alpha)
    setEnabled(true)
    shown=shader:isEnabled()
end
return {draw=draw,destroy=destroy,isVisible=function() return shown end}
