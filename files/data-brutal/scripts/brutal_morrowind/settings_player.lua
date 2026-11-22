-- Apply preferences at load and on edits. Effective values honour the master
-- without overwriting the user's individual choices.
local self=require('openmw.self')
local core=require('openmw.core')
local types=require('openmw.types')
local async=require('openmw.async')
local S=require('scripts.brutal_morrowind.settings')
local native={
    firstPersonBody='firstPersonBody',guaranteedHits='guaranteedHits',
    dualWieldEnabled='dualWield',meleeGeometry='meleeGeometry',equipmentPhysics='equipmentPhysics',
    npcStandupEnabled='npcStandup',ragdollSoftStops='ragdollSoftStops',ragdollNeutralPose='ragdollNeutralPose',
    ragdollLimbResponse='ragdollLimbResponse',ragdollSelfCollision='ragdollSelfCollision',
    ragdollEnabled='ragdoll',ragdollEnhancements='ragdollEnhancements',dynamicProps='dynamicProps',
    bloodDecals='bloodDecals',movementEnabled='kineticMovement',activeBlockEnabled='activeBlocking',
    parryEnabled='swordParry',attackCombosEnabled='comboAttacks',npcAiTweaks='npcCombatTweaks',
    npcStagger='npcStagger',hitRecovery='hitRecovery',staminaEnabled='stamina',
    wallBounceEnabled='wallBounce',clinchEnabled='clinch',dismemberment='dismemberment',
    slideEnabled='slide',combatSounds='combatSounds',fastAttacksEnabled='fastAttacks',
    bloodEffects='bloodEffects',sparkEffects='sparkEffects',kickAlwaysRagdoll='kickAlwaysRagdoll',
}
local function apply()
    -- Storage callbacks are asynchronous; explicit apply must see this edit now.
    S.refresh()
    local set=types.Actor.setBrutalOption
    if not S.raw('enabled') then set(self,'enabled',false) end
    -- The native bridge owns its master/dependency gates. Only the camera-mode
    -- mask changes requested values here; keep its existing independent options.
    for key,option in pairs(native) do set(self,option,S.mode(key)) end
    set(self,'classicFirstPerson',S.mode('firstPersonAnimations')=='morrowind')
    set(self,'classicNpcAnimations',S.mode('npcAnimations')=='morrowind')
    set(self,'impactEffects',S.mode('bloodEffects') or S.mode('sparkEffects'))
    set(self,'adrenalineHud',S.mode('adrenalineEnabled') and S.mode('adrenalineHudEnabled'))
    if S.raw('enabled') then set(self,'enabled',true) end
    core.sendGlobalEvent('BrutalSettingsChanged',{player=self,values=S.snapshot()})
end
S.section():subscribe(async:callback(apply))
return {
    interfaceName='BrutalSettings',
    interface={version=2,get=S.get,raw=S.raw,apply=apply},
    engineHandlers={onActive=apply,onLoad=apply,onInit=apply},
}
