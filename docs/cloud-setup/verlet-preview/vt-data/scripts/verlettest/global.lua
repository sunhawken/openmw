local world = require('openmw.world')
local util = require('openmw.util')
local t, phase = 0, ''
-- name, start, speed(units/s), anim, yaw rate rad/s
local plan = {
  {'idle', 0, 0, 'idle', 0}, {'walk', 3, 110, 'walkforward', 0}, {'stop', 7, 0, 'idle', 0},
  {'run', 10, 300, 'runforward', 0}, {'stop2', 14, 0, 'idle', 0},
}
local total = 18
return { engineHandlers = { onUpdate = function(dt)
  local p = world.players[1]
  if not p then return end
  t = (t + dt) % total
  local cur = plan[1]
  for _, e in ipairs(plan) do if t >= e[2] then cur = e end end
  if cur[1] ~= phase then
    phase = cur[1]
    print('VT_PHASE ' .. phase .. ' t=' .. string.format('%.1f', t))
    p:sendEvent('VT_Anim', cur[4])
  end
  if cur[3] > 0 then
    local y = p.rotation:getYaw()
    local d = util.vector3(math.sin(y), math.cos(y), 0) * cur[3] * math.min(dt, 0.1)
    p:teleport(p.cell, p.position + d, { rotation = p.rotation })
  end
end } }
