local self = require('openmw.self')
local camera = require('openmw.camera')
local util = require('openmw.util')
local input = require('openmw.input')
local anim = require('openmw.animation')
local inited, cur = false, nil
return {
  eventHandlers = { VT_Anim = function(name)
    if cur then anim.cancel(self, cur); cur = nil end
    if name ~= 'idle' then
      cur = name
      anim.playBlended(self, name, { loops = 100000, priority = anim.PRIORITY.Scripted, autoDisable = false })
    end
  end },
  engineHandlers = { onUpdate = function(dt)
    if not inited then inited = true; input.setControlSwitch(input.CONTROL_SWITCH.Controls, false); end
    if camera.getMode() ~= camera.MODE.Static then camera.setMode(camera.MODE.Static, true) return end
    local pos = self.position
    local y = self.rotation:getYaw()
    camera.setStaticPosition(util.vector3(pos.x + math.cos(y) * 105, pos.y - math.sin(y) * 105, pos.z + 125))
    camera.setYaw(y - math.pi / 2)
    camera.setPitch(0.05)
  end },
}
