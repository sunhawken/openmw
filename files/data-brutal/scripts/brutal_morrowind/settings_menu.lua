-- Options > Scripts > Brutal Morrowind, also available from the main menu.
local I=require('openmw.interfaces')
local ui=require('openmw.ui')
local util=require('openmw.util')
local core=require('openmw.core')
local S=require('scripts.brutal_morrowind.settings')
local bindings=require('scripts.brutal_morrowind.input_bindings')
local L='BrutalMorrowind'
local D=S.DEFAULTS
local labels=core.l10n(L)
local renderers={}
require('scripts.omw.settings.renderers')(function(name,render) renderers[name]=render end)
-- Labels and descriptions follow the actual pane width, with the control at the
-- right edge. This keeps compact windows usable without wasting larger ones.
I.Settings.registerRenderer('brutalCompact',function(value,set,argument)
    return {type=ui.TYPE.Flex,props={autoSizeWidth=false},external={stretch=1},content=ui.content{
        {type=ui.TYPE.Flex,external={stretch=1},props={horizontal=true,autoSizeWidth=false,arrange=ui.ALIGNMENT.Center},content=ui.content{
            {template=I.MWUI.templates.textParagraph,external={grow=1},props={text=labels(argument.key),size=util.vector2(185,0)}},
            {template=I.MWUI.templates.interval},
            renderers[argument.renderer](value,set,argument.options),
        }},
        {template=I.MWUI.templates.textParagraph,external={stretch=1},props={text=labels(argument.key..'Description'),size=util.vector2(0,0),relativeSize=util.vector2(1,0)}},
    }}
end)
-- Without the imported Dark Messiah data the page says which components are off.
I.Settings.registerPage{key='BrutalMorrowind',l10n=L,name='PageName',
    description=S.darkMessiahData and 'PageDescription' or 'PageDescriptionNoDarkMessiah'}
local function setting(key,renderer,argument)
    if renderer=='checkbox' then argument={l10n=L,trueLabel='On',falseLabel='Off'} end
    return {key=key,renderer='brutalCompact',argument={key=key,renderer=renderer,options=argument},name='Blank',default=D[key]}
end
local options={setting('enabled','checkbox'),setting('allowThirdPerson','checkbox'),
    {key='kickBinding',renderer='inputBinding',argument={key=bindings.KICK,type='trigger'},name='Blank',default=D.kickBinding},
    {key='sprintBinding',renderer='inputBinding',argument={key=bindings.SPRINT,type='action'},name='Blank',default=D.sprintBinding},
}
for _,key in ipairs({
    'firstPersonBody','firstPersonAnimations','dualWieldEnabled','npcAnimations','guaranteedHits','meleeGeometry','equipmentPhysics',
    'ragdollEnabled','ragdollEnhancements','npcStandupEnabled','ragdollSoftStops','ragdollNeutralPose','ragdollLimbResponse','ragdollSelfCollision','dynamicProps','movementEnabled','staminaEnabled',
    'kickEnabled','kickAlwaysRagdoll','slideEnabled','activeBlockEnabled','shieldBashEnabled','parryEnabled',
    'clinchEnabled','attackCombosEnabled','fastAttacksEnabled','wallBounceEnabled','npcAiTweaks','npcStagger','hitRecovery',
    'adrenalineEnabled','finisherEnabled','superKickEnabled','slowMotionEnabled','dismemberment','bloodEffects','sparkEffects',
    'bloodDecals','combatSounds','adrenalineHudEnabled','adrenalineFxEnabled','slideFxEnabled','clinchFxEnabled',
}) do
    options[#options+1]=setting(key,type(D[key])=='string' and 'select' or 'checkbox',
        type(D[key])=='string' and {l10n=L,items={'darkMessiah','morrowind'}} or nil)
end
options[#options+1]=setting('superKickSpeed','number',{integer=true,min=460,max=1000})
options[#options+1]=setting('adrenalineHits','number',{integer=true,min=1,max=30})
options[#options+1]=setting('adrenalineDecay','checkbox')
I.Settings.registerGroup{key=S.GROUP,page='BrutalMorrowind',l10n=L,name='GroupName',permanentStorage=true,settings=options}
