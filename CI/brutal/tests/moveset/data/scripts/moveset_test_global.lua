-- Places a target NPC in front of the player for the move set test.
local world = require('openmw.world')
local util = require('openmw.util')
local done = false
return {
    eventHandlers = {
        MovesetTestSpawnTarget = function(e)
            local player = world.players[1]
            local forward = player.rotation:apply(util.vector3(0, 1, 0))
            local target = world.createObject('fargoth')
            target:teleport(player.cell, player.position + forward * 90,
                { rotation = player.rotation * util.transform.rotateZ(math.pi) })
            print('[moveset-test] target placed')
            player:sendEvent('MovesetTestTarget', { target = target })
        end,
    },
    engineHandlers = {
        onUpdate = function()
            if done or not world.players[1] then return end
            done = true
            world.players[1]:sendEvent('MovesetTestBegin', {})
        end,
    },
}
