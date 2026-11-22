-- Preferences survive the master switch. NPC/global scripts read the player's
-- synchronized snapshot because player storage is not exposed in those contexts.
local storage=require('openmw.storage')
local vfs=require('openmw.vfs')
local GROUP='SettingsBrutalMorrowind'
local SYNC='BrutalMorrowindRuntimeSettings'
local DEFAULTS={
    enabled=true,
    allowThirdPerson=false,
    kickBinding='BrutalMorrowindKick',
    sprintBinding='BrutalMorrowindSprint',
    firstPersonBody=true,
    dualWieldEnabled=true,
    firstPersonAnimations='darkMessiah',
    npcAnimations='darkMessiah',
    guaranteedHits=true,
    meleeGeometry=true,
    equipmentPhysics=true,
    ragdollEnabled=true,
    ragdollEnhancements=true,
    npcStandupEnabled=true,
    ragdollSoftStops=true,
    ragdollNeutralPose=true,
    ragdollLimbResponse=true,
    ragdollSelfCollision=true,
    dynamicProps=true,
    movementEnabled=true,
    staminaEnabled=true,
    kickEnabled=true,
    kickAlwaysRagdoll=false,
    slideEnabled=true,
    activeBlockEnabled=true,
    shieldBashEnabled=true,
    parryEnabled=true,
    clinchEnabled=true,
    attackCombosEnabled=true,
    fastAttacksEnabled=true,
    wallBounceEnabled=true,
    npcAiTweaks=true,
    npcStagger=true,
    hitRecovery=true,
    adrenalineEnabled=true,
    finisherEnabled=true,
    superKickEnabled=true,
    slowMotionEnabled=true,
    dismemberment=true,
    bloodEffects=true,
    sparkEffects=true,
    bloodDecals=true,
    combatSounds=true,
    adrenalineHudEnabled=true,
    adrenalineFxEnabled=true,
    slideFxEnabled=true,
    clinchFxEnabled=true,
    superKickSpeed=460,
    adrenalineHits=30,
    adrenalineDecay=true,
}
-- Components that play Dark Messiah animations or use its sounds and textures.
-- Without the imported data they are off and both animation sets are Morrowind's;
-- the stored preferences stay as they are and apply again once the data exists.
local DARK_MESSIAH_KEYS={'firstPersonBody','dualWieldEnabled','meleeGeometry','dismemberment',
    'npcStandupEnabled','kickEnabled','kickAlwaysRagdoll','superKickEnabled','slideEnabled','slideFxEnabled','activeBlockEnabled',
    'shieldBashEnabled','parryEnabled','clinchEnabled','clinchFxEnabled','attackCombosEnabled',
    'fastAttacksEnabled','wallBounceEnabled','adrenalineEnabled','finisherEnabled','bloodEffects',
    'sparkEffects','bloodDecals','combatSounds'}
local darkMessiahData=vfs.fileExists('animations/brutal_npc/brutal_hit.kf')
    and vfs.fileExists('animations/brutal_fp/brutal_blockhit1.kf')
    and vfs.fileExists('sound/brutal_dm/armor_heavy_01.wav')
-- Reads in gameplay are plain table lookups. Refresh only when persistent
-- preferences/the mirrored snapshot changes, or explicitly before native apply.
local async=require('openmw.async')
local playerContext=storage.playerSection~=nil
local section=playerContext and storage.playerSection(GROUP) or storage.globalSection(SYNC)
local rawValues,modeValues,effectiveValues
local function validate(key,value)
    local default=DEFAULTS[key]
    if value==nil or type(value)~=type(default) then return default end
    if type(value)=='number' and (value~=value or math.abs(value)==math.huge) then return default end
    if key=='firstPersonAnimations' or key=='npcAnimations' then
        return value=='morrowind' and 'morrowind' or 'darkMessiah'
    end
    if key=='superKickSpeed' then return math.max(460,math.min(1000,value)) end
    if key=='adrenalineHits' then return math.max(1,math.min(30,math.floor(value))) end
    return value
end
local function refresh()
    local stored
    if playerContext then stored=section:asTable()
    else stored=section:get('values') end
    local values={}
    local changed=rawValues==nil
    for key in pairs(DEFAULTS) do
        local value=validate(key,stored and stored[key])
        -- NPC/global contexts fail closed until the player's first snapshot.
        if key=='enabled' and not playerContext and not stored then value=false end
        values[key]=value
        if rawValues and value~=rawValues[key] then changed=true end
    end
    if not changed then return false end
    local mode={}
    for key,value in pairs(values) do mode[key]=value end
    -- A mode preference, not the current camera position: both views use the
    -- same component set, without rig/ownership changes during an attack.
    -- Permanent preferences are untouched and return when this mode is disabled.
    if values.allowThirdPerson then
        mode.firstPersonAnimations='morrowind'
        for _,key in ipairs({'firstPersonBody','dualWieldEnabled',
            'meleeGeometry','dismemberment','kickEnabled','kickAlwaysRagdoll','superKickEnabled',
            'slideEnabled','slideFxEnabled','shieldBashEnabled','clinchEnabled',
            'clinchFxEnabled','attackCombosEnabled','fastAttacksEnabled','wallBounceEnabled'}) do
            mode[key]=false
        end
    end
    if not darkMessiahData then
        mode.firstPersonAnimations='morrowind'
        mode.npcAnimations='morrowind'
        for _,key in ipairs(DARK_MESSIAH_KEYS) do mode[key]=false end
    end
    local effective={}
    for key,value in pairs(mode) do
        if not values.enabled and type(value)=='boolean' then value=false
        elseif not values.enabled and (key=='firstPersonAnimations' or key=='npcAnimations') then value='morrowind' end
        effective[key]=value
    end
    if not effective.ragdollEnabled then effective.dismemberment=false end
    if not effective.ragdollEnabled or not effective.kickEnabled then effective.kickAlwaysRagdoll=false end
    rawValues,modeValues,effectiveValues=values,mode,effective
    return true
end
refresh()
section:subscribe(async:callback(refresh))
local function raw(key) return rawValues[key] end
local function get(key) return effectiveValues[key] end
local function snapshot()
    local values={}
    for key,value in pairs(rawValues) do values[key]=value end
    return values
end
return {GROUP=GROUP,SYNC=SYNC,DEFAULTS=DEFAULTS,darkMessiahData=darkMessiahData,get=get,raw=raw,mode=function(key)return modeValues[key]end,snapshot=snapshot,
    refresh=refresh,section=function() return section end}
