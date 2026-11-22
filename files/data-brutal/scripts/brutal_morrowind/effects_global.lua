local settings=require('scripts.brutal_morrowind.settings')
local world = require('openmw.world')
local types = require('openmw.types')
local assetsLoaded = pcall(require, 'scripts.brutal_assets.effects')
local function available()
    if not assetsLoaded or type(world.spawnBrutalImpact) ~= 'function'
        or type(world.getBrutalImpactStats) ~= 'function' then return false end
    local state = world.getBrutalImpactStats()
    -- Availability describes resources, not the current switches. Local
    -- Combat scripts cache this capability and must resume after re-enabling.
    return state.bloodAvailable == true and state.sparksAvailable == true
end
return {eventHandlers = {
    BrutalRestoreBloodDecals = function(data)
        if not settings.get('bloodDecals') then return end
        if type(world.restoreNpcBloodDecals) == 'function' and data.actor and data.actor:isValid()
            and type(data.data) == 'string' then world.restoreNpcBloodDecals(data.actor, data.data) end
    end,
    BrutalFeedbackQuery = function(data)
        if data.actor and data.actor:isValid() then
            data.actor:sendEvent('BrutalFeedbackAvailable', {available = available()})
        end
    end,
    BrutalImpact = function(data)
        if data.kind=='blood' and not (settings.get('bloodEffects') or settings.get('bloodDecals'))
            or data.kind=='sparks' and not settings.get('sparkEffects') then return end
        if not available() or (data.kind ~= 'blood' and data.kind ~= 'sparks') then return end
        local actor, player = data.actor, world.players[1]
        if not actor or not actor:isValid() or not actor.enabled or not actor.cell
            or not player or not player.cell or not types.NPC.objectIsInstance(actor) then return end
        local connected = actor.cell == player.cell or (actor.cell.isExterior and player.cell.isExterior)
        if not connected or (actor.position - player.position):length() > 2400 then return end
        if not data.position or (data.position - actor.position):length() > 350 then return end
        world.spawnBrutalImpact(data.kind, data.position, data.direction, {
            intensity = math.max(0.25, math.min(2, data.intensity or 1)), power = data.power == true,
            contactExact = data.contactExact == true,
            actor = data.kind == 'blood' and actor
                or not data.contactExact and data.kind == 'sparks' and data.attachment == 'shield' and actor or nil,
            woundKind = data.woundKind,
            woundPart = data.woundPart,
            attachment = not data.contactExact and data.kind == 'sparks' and data.attachment == 'shield' and 'shield' or nil,
        })
    end,
}}
