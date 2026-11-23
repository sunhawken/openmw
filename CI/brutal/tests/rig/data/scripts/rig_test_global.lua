-- Spawns an NPC whose model is the generated rig (meshes/test/<rig>.dae)
-- and attaches the checking script to it. RIG_TEST_MODEL selects the rig.
local world = require('openmw.world')
local types = require('openmw.types')
local util = require('openmw.util')
local done = false
return {
    engineHandlers = {
        onUpdate = function()
            if done or not world.players[1] then return end
            done = true
            local player = world.players[1]
            local models = require('scripts.rig_models')
            for index, model in ipairs(models) do
                local draft = types.NPC.createRecordDraft({ template = types.NPC.record('fargoth'), model = model,
                    name = 'rig test' })
                local record = world.createRecord(draft)
                local npc = world.createObject(record.id)
                local forward = player.rotation:apply(util.vector3(0, 1, 0))
                local side = (index - (#models + 1) / 2) * 90
                npc:teleport(player.cell, player.position + forward * 350 + util.vector3(side, 0, 0))
                npc:addScript('scripts/rig_test_actor.lua', { model = model })
                print('[rig-test] spawned NPC with ' .. model)
            end
        end,
    },
}
