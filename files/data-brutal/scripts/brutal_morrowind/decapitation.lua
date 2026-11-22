-- One draw per confirmed lethal adrenaline hit. No redraws during scene retries.
local types = require('openmw.types')
local settings = require('scripts.brutal_morrowind.settings')
local roll = math.random
-- math.random() is in [0, 1), so every qualifying lethal adrenaline hit cuts.
local CHANCE = 1.0
local M = {}
local MELEE = {}
for _, name in ipairs({'ShortBladeOneHand', 'LongBladeOneHand', 'LongBladeTwoHand', 'BluntOneHand', 'BluntTwoClose',
    'BluntTwoWide', 'SpearTwoWide', 'AxeOneHand', 'AxeTwoHand'}) do MELEE[types.Weapon.TYPE[name]] = true end
local function eligible(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer, hit, kind)
    if not settings.get('finisherEnabled') or not settings.get('dismemberment') or not types.Actor.getBrutalOption('dismemberment') then return false end
    if not aliveBefore or not deadAfter or not victimIsNpc or victimIsPlayer
        or attack.brutalPowered ~= true or not attack.successful or attack.brutalBlocked
        or attack.brutalShieldRush == true
        or attack.brutalAttackType ~= (kind or 'slash') or not hit
        or attack.sourceType ~= 'melee' or ((attack.damage or {}).health or 0) <= 0
        or not attack.attacker or not attack.attacker:isValid()
        or not types.Player.objectIsInstance(attack.attacker)
        or not attack.weapon or not attack.weapon:isValid()
        or not types.Weapon.objectIsInstance(attack.weapon) then return false end
    return MELEE[types.Weapon.record(attack.weapon).type] == true
end
function M.consider(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer)
    if not eligible(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer,
        attack.brutalHeadHit == true and attack.brutalSlideAttack ~= true) then return false, false end
    local value = roll()
    return true, value < CHANCE, value
end
function M.considerBisection(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer)
    local slide = attack.brutalSlideAttack == true
    -- Slide finishers share the broad native leg boundary: lower contacts cut
    -- at the knees; every confirmed upper contact splits at the waist.
    local upperSlide = slide and attack.brutalLegHit == false
    local yes = eligible(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer,
        upperSlide or (not slide and attack.brutalWaistHit == true
            and attack.brutalHeadHit ~= true and attack.brutalLegHit ~= true))
    return yes, yes
end
function M.considerLegs(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer)
    if not eligible(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer,
        attack.brutalLegHit == true and attack.brutalLegsAllowed == true) then return false, 0 end
    -- One draw for this confirmed fatal swing. Retrying geometry or restoring
    -- the corpse never changes the selected left/right/both outcome.
    local value = roll()
    return true, value < 1/3 and 1 or value < 2/3 and 2 or 3, value
end
function M.considerArm(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer)
    local mask = attack.brutalClosestArm
    local yes = eligible(attack, aliveBefore, deadAfter, victimIsNpc, victimIsPlayer,
        mask == 4 or mask == 8, 'chop')
    return yes, yes and mask or 0
end
return M
