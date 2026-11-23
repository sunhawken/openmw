-- Places a target NPC in front of the player for the move set test.
local world = require('openmw.world')
local util = require('openmw.util')
local types = require('openmw.types')
local done = false
return {
    eventHandlers = {
        MovesetTestWall = function(e)
            -- A stock wall between the player and the target, facing the player.
            local player = world.players[1]
            local mid = (player.position + e.target.position) / 2
            local wall = world.createObject('ex_imp_wall_01')
            wall:teleport(player.cell, mid + util.vector3(0, 0, -50), { rotation = player.rotation })
            print('[moveset-test] wall placed')
            player:sendEvent('MovesetTestWallPlaced', { wall = wall })
        end,
        MovesetTestMoveTarget = function(e)
            local player = world.players[1]
            local forward = player.rotation:apply(util.vector3(0, 1, 0))
            e.target:teleport(player.cell, player.position + forward * e.distance,
                { rotation = player.rotation * util.transform.rotateZ(math.pi) })
            player:sendEvent('MovesetTestTargetMoved', {})
        end,
        MovesetTestSpawnTarget = function(e)
            local player = world.players[1]
            local forward = player.rotation:apply(util.vector3(0, 1, 0))
            -- A creature, not an NPC: striking it is no crime, so no guard
            -- dialogue pauses the game mid-test. Fight 0 keeps it passive.
            local target = world.createObject('mudcrab')
            target:teleport(player.cell, player.position + forward * 90,
                { rotation = player.rotation * util.transform.rotateZ(math.pi) })
            target:addScript('scripts/moveset_test_target.lua')
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
