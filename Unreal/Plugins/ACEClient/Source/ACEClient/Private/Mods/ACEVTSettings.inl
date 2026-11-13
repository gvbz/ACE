// Character settings use typed table data, never executable Decal code.
namespace
{
bool VTMonsterExpression(const J& E,TArray<FString>& Issues,bool HasCatalog)
{
    const FString Op=S(E,TEXT("op"));
    if(Op==TEXT("call")||Op==TEXT(";")||Op==TEXT("^")){Issues.Add(TEXT("Unsupported monster expression operator: ")+Op);return false;}
    if(Op==TEXT("literal"))
    {
        const FString Word=S(E,TEXT("value")).ToLower();
        if((Word==TEXT("species")||Word==TEXT("maxhp"))&&!HasCatalog){Issues.Add(TEXT("Monster expression requires gameinfodb.ugd beside the source profile: ")+Word);return false;}
        if(Word==TEXT("name")||Word==TEXT("typeid")||Word==TEXT("range")||Word==TEXT("metastate")||Word==TEXT("hasshield")||Word==TEXT("species")||Word==TEXT("maxhp")){E->SetStringField(TEXT("op"),TEXT("monster_var"));E->SetStringField(TEXT("name"),Word);}
        return true;
    }
    bool Valid=true;for(const TCHAR* Key:{TEXT("left"),TEXT("right")}){const J* Child=nullptr;if(E->TryGetObjectField(Key,Child))Valid=VTMonsterExpression(*Child,Issues,HasCatalog)&&Valid;}
    if(Op==TEXT("#"))
    {
        const auto Right=E->GetObjectField(TEXT("right"));
        if(S(Right,TEXT("op"))!=TEXT("literal")||!Right->HasTypedField<EJson::String>(TEXT("value"))||!ACEVTProfile::IsSupportedPattern(S(Right,TEXT("value"))))
        {Issues.Add(TEXT("Monster regex must be a supported literal pattern"));Valid=false;}
    }
    return Valid;
}
TArray<J> VTSettingsRows(const J& Table,TArray<FString>& Issues)
{
    TArray<J> Result;const A *Columns=nullptr,*Rows=nullptr;
    if(!Table||!Table->TryGetArrayField(TEXT("columns"),Columns)||!Table->TryGetArrayField(TEXT("rows"),Rows))
    {Issues.Add(TEXT("Invalid character settings table"));return Result;}
    for(const auto& Row:*Rows)
    {
        const A* Cells=nullptr;if(!Row->TryGetArray(Cells)||Cells->Num()!=Columns->Num())
        {Issues.Add(TEXT("Invalid character settings row"));continue;}
        auto O=MakeShared<FJsonObject>();
        for(int I=0;I<Cells->Num();++I)O->SetField((*Columns)[I]->AsString(),(*Cells)[I]);
        Result.Add(O);
    }
    return Result;
}
J VTSettings(const J& D,TArray<FString>& Issues)
{
    auto Out=MakeShared<FJsonObject>();A Buffs,Excluded,Consumables,BuffItems,Equipment,ItemTargets,Monsters,Handlers;
    // USD stores overrides of VT defaults, not overrides of whichever UCM
    // profile happened to be loaded previously.
    Out->SetBoolField(TEXT("auto_craft"),true);Out->SetBoolField(TEXT("auto_fellowship"),true);
    Out->SetBoolField(TEXT("random_helper_buffs"),false);Out->SetNumberField(TEXT("random_helper_interval"),5);
    Out->SetBoolField(TEXT("cast_jiggle"),false);Out->SetBoolField(TEXT("jump_out_wand"),false);
    for(const TCHAR* Key:{TEXT("craft_health_kits"),TEXT("craft_stamina_kits"),TEXT("craft_mana_kits")})Out->SetNumberField(Key,2);
    for(const TCHAR* Key:{TEXT("craft_health_food"),TEXT("craft_stamina_food"),TEXT("craft_mana_food")})Out->SetNumberField(Key,15);
    Out->SetBoolField(TEXT("buffing"),true);Out->SetStringField(TEXT("combat"),TEXT("auto"));
    Out->SetBoolField(TEXT("navigation"),false);Out->SetBoolField(TEXT("looting"),false);Out->SetBoolField(TEXT("meta_enabled"),false);
    Out->SetBoolField(TEXT("recovery"),true);Out->SetBoolField(TEXT("target_lock"),false);
    Out->SetNumberField(TEXT("target_select"),3);Out->SetNumberField(TEXT("target_angle_range"),5);Out->SetNumberField(TEXT("minimum_range"),0);
    // UCM defaults to fast buffs; an explicit imported FastCastBuffs still wins.
    Out->SetBoolField(TEXT("fast_cast_buffs"),true);
    Out->SetBoolField(TEXT("dispel_self"),false);Out->SetBoolField(TEXT("dispel_items"),false);
    Out->SetBoolField(TEXT("summon_pets"),true);Out->SetNumberField(TEXT("pet_range_mode"),0);Out->SetNumberField(TEXT("pet_range"),5);Out->SetNumberField(TEXT("pet_min_targets"),1);
    Out->SetBoolField(TEXT("split_peas"),true);Out->SetNumberField(TEXT("component_critical"),4);Out->SetNumberField(TEXT("component_normal"),20);Out->SetNumberField(TEXT("component_idle"),20);
    Out->SetBoolField(TEXT("item_mana"),true);Out->SetNumberField(TEXT("item_mana_threshold"),20);
    Out->SetBoolField(TEXT("read_unknown_scrolls"),false);
    Out->SetNumberField(TEXT("kit_min_success"),95);Out->SetBoolField(TEXT("kits_in_magic"),true);Out->SetBoolField(TEXT("kit_peace"),false);
    Out->SetNumberField(TEXT("helper_health_threshold"),20);Out->SetNumberField(TEXT("helper_stamina_threshold"),1);Out->SetNumberField(TEXT("helper_mana_threshold"),1);
    Out->SetNumberField(TEXT("helper_health_range"),74.5);Out->SetNumberField(TEXT("helper_stamina_range"),74.5);Out->SetNumberField(TEXT("helper_mana_range"),40);
    Out->SetBoolField(TEXT("auto_attack_power"),true);Out->SetBoolField(TEXT("use_recklessness"),true);
    Out->SetBoolField(TEXT("ring"),false);Out->SetNumberField(TEXT("ring_range"),5);Out->SetNumberField(TEXT("ring_min_targets"),4);
    Out->SetNumberField(TEXT("debuff_refresh"),5);Out->SetBoolField(TEXT("debuff_fallback"),false);
    Out->SetNumberField(TEXT("debuff_selection"),2);
    Out->SetObjectField(TEXT("grenade_catalog"),MakeShared<FJsonObject>());
    const J* Grenades=nullptr;if(D->TryGetObjectField(TEXT("grenade_catalog"),Grenades))Out->SetObjectField(TEXT("grenade_catalog"),*Grenades);
    Out->SetNumberField(TEXT("use_arcs"),1);Out->SetNumberField(TEXT("arc_range"),5);Out->SetNumberField(TEXT("debuff_each_first"),1);
    Out->SetBoolField(TEXT("switch_debuff_wand"),true);
    for(const TCHAR* Key:{TEXT("debuff_yield"),TEXT("debuff_imperil"),TEXT("debuff_vuln")})Out->SetBoolField(Key,false);
    Out->SetBoolField(TEXT("idle_buff_topoff"),false);Out->SetNumberField(TEXT("idle_buff_seconds"),1200);
    Out->SetBoolField(TEXT("loot_fellow_corpses"),false);Out->SetBoolField(TEXT("loot_all_corpses"),false);
    Out->SetNumberField(TEXT("monster_attempts"),4);Out->SetNumberField(TEXT("monster_blacklist_seconds"),120);
    Out->SetNumberField(TEXT("corpse_attempts"),30);Out->SetNumberField(TEXT("corpse_blacklist_seconds"),200);
    Out->SetBoolField(TEXT("auto_buffs"),true);Out->SetNumberField(TEXT("attack_spell"),0);Out->SetNumberField(TEXT("damage_type"),0);
    Out->SetNumberField(TEXT("character_damage_version"),2);Out->SetNumberField(TEXT("monster_damage_version"),2);
    Out->SetNumberField(TEXT("protection_profile"),2);Out->SetNumberField(TEXT("bane_profile"),2);
    Out->SetStringField(TEXT("protection_custom"),TEXT("ALL"));Out->SetStringField(TEXT("bane_custom"),TEXT("ALL"));
    Out->SetArrayField(TEXT("weapons"),{});Out->SetArrayField(TEXT("consumables"),{});Out->SetArrayField(TEXT("excluded_buffs"),{});
    Out->SetNumberField(TEXT("radius"),5);Out->SetNumberField(TEXT("approach_range"),0);
    Out->SetNumberField(TEXT("health_threshold"),75);Out->SetNumberField(TEXT("stamina_threshold"),50);Out->SetNumberField(TEXT("mana_threshold"),50);
    Out->SetNumberField(TEXT("idle_health_threshold"),1);Out->SetNumberField(TEXT("idle_stamina_threshold"),1);Out->SetNumberField(TEXT("idle_mana_threshold"),1);
    Out->SetNumberField(TEXT("refresh_seconds"),300);Out->SetNumberField(TEXT("buff_skill_margin"),5);
    auto AddDefaultHandler=[&](int Vital,int Stance,const TCHAR* Name,int Max=100)
    {auto H=MakeShared<FJsonObject>();H->SetNumberField(TEXT("vital"),Vital);H->SetNumberField(TEXT("stance"),Stance);H->SetStringField(TEXT("handler"),Name);H->SetNumberField(TEXT("min"),0);H->SetNumberField(TEXT("max"),Max);Handlers.Add(Obj(H));};
    for(int Stance:{1,2})
    {
        if(Stance==1)
        {
            for(const TCHAR* Name:{TEXT("Stamina to Health"),TEXT("Mana to Health"),TEXT("Regular Spell"),TEXT("Recharge With Food")})AddDefaultHandler(2,Stance,Name,15);
            for(const TCHAR* Name:{TEXT("Kit Recharge"),TEXT("Stamina to Health"),TEXT("Mana to Health"),TEXT("Regular Spell"),TEXT("Recharge With Food")})AddDefaultHandler(2,Stance,Name);
        }
        else {AddDefaultHandler(2,Stance,TEXT("Recharge With Food"),15);AddDefaultHandler(2,Stance,TEXT("Kit Recharge"));AddDefaultHandler(2,Stance,TEXT("Recharge With Food"));AddDefaultHandler(2,Stance,TEXT("Stamina to Health"),10);AddDefaultHandler(2,Stance,TEXT("Regular Spell"));}
        AddDefaultHandler(4,Stance,TEXT("Kit Recharge"));AddDefaultHandler(4,Stance,Stance==1?TEXT("Regular Spell"):TEXT("Recharge With Food"));AddDefaultHandler(4,Stance,Stance==1?TEXT("Recharge With Food"):TEXT("Regular Spell"));
        AddDefaultHandler(6,Stance,TEXT("Kit Recharge"));AddDefaultHandler(6,Stance,TEXT("Recharge With Food"));AddDefaultHandler(6,Stance,TEXT("Regular Spell"));
    }
    // Always replace collections on load: an empty table clears the previous setup.
    for(const auto& Entry:D->GetArrayField(TEXT("tables")))
    {
        const auto Table=Entry->AsObject();const FString Name=S(Table,TEXT("name"));
        for(const auto& Row:VTSettingsRows(Table,Issues))
        {
            if(Name==TEXT("Settings"))
            {
                const FString Key=S(Row,TEXT("Setting"));const auto Value=Row->TryGetField(TEXT("Value"));
                if(Key.Equals(TEXT("RechargeHandlerSet"),ESearchCase::IgnoreCase))
                {
                    if(!Value||Value->Type!=EJson::Object){Issues.Add(TEXT("Invalid recharge handler table"));continue;}
                    Handlers.Reset();
                    for(const auto& H:VTSettingsRows(Value->AsObject(),Issues))
                    {
                        const int Vital=int(N(H,TEXT("Vital"))),Stance=int(N(H,TEXT("Stance")));
                        const FString Handler=S(H,TEXT("HandlerString"));
                        const TMap<FString,int> Conversions={{TEXT("Stamina to Health"),2},{TEXT("Mana to Health"),2},{TEXT("Health to Stamina"),4},{TEXT("Health to Mana"),6}};
                        const bool Regular=Handler==TEXT("Regular Spell")||Handler==TEXT("Recharge With Food")||Handler==TEXT("Kit Recharge");
                        if(Vital<1||Vital>3||(Stance!=1&&Stance!=2)||N(H,TEXT("MinPercent"))<0||N(H,TEXT("MaxPercent"))>100||N(H,TEXT("MinPercent"))>N(H,TEXT("MaxPercent"))||(!Regular&&Conversions.FindRef(Handler)!=Vital*2))
                        {Issues.Add(TEXT("Unsupported recharge handler: ")+Handler);continue;}
                        auto R=MakeShared<FJsonObject>();R->SetNumberField(TEXT("vital"),Vital*2);R->SetNumberField(TEXT("stance"),Stance);
                        R->SetStringField(TEXT("handler"),Handler);R->SetNumberField(TEXT("min"),N(H,TEXT("MinPercent")));R->SetNumberField(TEXT("max"),N(H,TEXT("MaxPercent")));Handlers.Add(Obj(R));
                    }
                    continue;
                }
                FString Text;if(Value&&Value->Type==EJson::Boolean)Text=Value->AsBool()?TEXT("true"):TEXT("false");
                else if(Value&&Value->Type==EJson::Number)Text=FString::Printf(TEXT("%.15g"),Value->AsNumber());
                else if(Value&&Value->Type==EJson::String)Text=Value->AsString();
                TArray<FString> Problems;const auto Command=VTCommand(TEXT("/vt opt set ")+Key+TEXT(" ")+Text,Problems);
                if(Problems.IsEmpty())Out->SetField(S(Command,TEXT("key")),Command->TryGetField(TEXT("value")));
                else Issues.Add(TEXT("Character setting needs an adapter: ")+Key);
            }
            else if(Name==TEXT("ExtraBuffSpells")||Name==TEXT("AntiExtraBuffSpells"))
            {
                const double Id=N(Row,TEXT("ExemplarId"));if(Id<=0||Id>MAX_int32||FMath::FloorToDouble(Id)!=Id){Issues.Add(TEXT("Invalid buff exemplar"));continue;}
                (Name==TEXT("ExtraBuffSpells")?Buffs:Excluded).Add(MakeShared<FJsonValueNumber>(Id));
            }
            else if(Name==TEXT("AssistItems"))
            {
                const double Type=N(Row,TEXT("Type"),-1);const FString Item=S(Row,TEXT("Object"));
                if(Type<0||Type>11||FMath::FloorToDouble(Type)!=Type||Item.IsEmpty()||(Type==11&&Item!=TEXT("[All Peas]"))){Issues.Add(TEXT("Unsupported assistance item: ")+Item);continue;}
                if(Type==7&&(!Grenades||!(*Grenades)->HasTypedField<EJson::Object>(Item)))
                {Issues.Add(TEXT("Grenade requires a matching GrenadeOptions entry in gameinfodb.ugd: ")+Item);continue;}
                auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),Item);R->SetNumberField(TEXT("type"),Type);Consumables.Add(Obj(R));
            }
            else if(Name==TEXT("GemFoodItems"))
            {
                if(S(Row,TEXT("Name")).IsEmpty()||N(Row,TEXT("Spell"))<=0){Issues.Add(TEXT("Invalid consumable buff"));continue;}
                auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),S(Row,TEXT("Name")));R->SetNumberField(TEXT("spell"),N(Row,TEXT("Spell")));BuffItems.Add(Obj(R));
            }
            else if(Name==TEXT("BuffedItems"))
            {
                const double Id=N(Row,TEXT("Object")),Spell=N(Row,TEXT("Spell"));
                if(Id<MIN_int32||Id>MAX_uint32||Id==0||FMath::FloorToDouble(Id)!=Id||Spell<-1||Spell==0||Spell>MAX_int32||FMath::FloorToDouble(Spell)!=Spell)
                {Issues.Add(TEXT("Invalid item buff binding"));continue;}
                const double Guid=Id<0?Id+4294967296.:Id;
                if(Id!=-1)Equipment.Add(MakeShared<FJsonValueNumber>(Guid));
                if(Spell>0){auto R=MakeShared<FJsonObject>();R->SetNumberField(TEXT("item"),Id==-1?0:Guid);R->SetNumberField(TEXT("spell"),Spell);ItemTargets.Add(Obj(R));}
            }
            else if(Name==TEXT("MyMonsters"))
            {
                const FString Monster=S(Row,TEXT("MonsterName"));auto R=MakeShared<FJsonObject>();
                // cl.cs evaluates a string result as an exact name, or a numeric
                // result as a predicate. Share the bounded arithmetic parser.
                if(Monster!=TEXT("<DEFAULT>"))
                {
                    VTExpression Parser{Monster};auto Expression=Parser.Expr(1,0);Parser.Space();
                    if(!Parser.Error.IsEmpty()||Parser.At!=Monster.Len())Issues.Add(TEXT("Invalid monster expression: ")+Monster);
                    else if(VTMonsterExpression(Expression,Issues,D->HasTypedField<EJson::Object>(TEXT("monster_catalog"))))R->SetObjectField(TEXT("expression"),Expression);
                }
                R->SetStringField(TEXT("name"),Monster);R->SetBoolField(TEXT("exact"),true);R->SetBoolField(TEXT("default"),Monster==TEXT("<DEFAULT>"));
                R->SetNumberField(TEXT("priority"),N(Row,TEXT("AttackPriority")));
                bool Attack=true;Row->TryGetBoolField(TEXT("Attack"),Attack);R->SetBoolField(TEXT("bolt"),Attack);
                bool Streak=true;Row->TryGetBoolField(TEXT("Streak"),Streak);R->SetBoolField(TEXT("streak"),Streak);
                bool Ring=false;Row->TryGetBoolField(TEXT("Ring"),Ring);R->SetBoolField(TEXT("ring"),Ring);
                // Attack-family switches do not disable essence summoning.
                R->SetBoolField(TEXT("ignore"),false);
                const int Damage=int(N(Row,TEXT("DamageType"),8));const int Elements[]={2,4,1,32,64,8,16};
                if(Damage>=0&&Damage<7)R->SetNumberField(TEXT("damage_type"),Elements[Damage]);
                else if(Damage==9)R->SetNumberField(TEXT("damage_type"),1024);
                else if(Damage!=8)Issues.Add(TEXT("Unsupported monster damage mode: ")+Monster);
                const double Weapon=N(Row,TEXT("WeaponToUse"),-1);
                if(Weapon<MIN_int32||Weapon>MAX_uint32||FMath::FloorToDouble(Weapon)!=Weapon)Issues.Add(TEXT("Invalid monster weapon: ")+Monster);
                else R->SetNumberField(TEXT("weapon"),Weapon==-1?-1:Weapon<0?Weapon+4294967296.:Weapon);
                for(const auto& Pair:TMap<FString,FString>{{TEXT("Imperil"),TEXT("debuff_imperil")},{TEXT("Vuln"),TEXT("debuff_vuln")},{TEXT("Yield"),TEXT("debuff_yield")},{TEXT("GravityW"),TEXT("debuff_gravity")},{TEXT("Broadside"),TEXT("debuff_broadside")},{TEXT("Fester"),TEXT("debuff_fester")},{TEXT("WeakeningCurse"),TEXT("debuff_weakening")},{TEXT("FesteringCurse"),TEXT("debuff_festering")},{TEXT("Corruption"),TEXT("debuff_corruption")},{TEXT("DestructiveCurse"),TEXT("debuff_destructive")},{TEXT("Corrosion"),TEXT("debuff_corrosion")}})
                {bool Enabled=false;Row->TryGetBoolField(Pair.Key,Enabled);R->SetBoolField(Pair.Value,Enabled);}
                const int Pet=int(N(Row,TEXT("PetDamageType"),101));
                if((Pet>=0&&Pet<7)||Pet==8||Pet==98||Pet==101)R->SetNumberField(TEXT("pet_damage_type"),Pet);
                else Issues.Add(TEXT("Unsupported pet damage mode: ")+Monster);
                const double Secondary=N(Row,TEXT("SecondaryVuln"),98);
                if(Secondary>=0&&Secondary<7&&FMath::FloorToDouble(Secondary)==Secondary)R->SetNumberField(TEXT("secondary_vuln"),Elements[int(Secondary)]);
                else if(Secondary!=98)Issues.Add(TEXT("Unsupported secondary vulnerability: ")+Monster);
                const double Offhand=N(Row,TEXT("SecondaryEquip"));
                if(Offhand<MIN_int32||Offhand>MAX_uint32||FMath::FloorToDouble(Offhand)!=Offhand)Issues.Add(TEXT("Invalid secondary equipment: ")+Monster);
                else R->SetNumberField(TEXT("secondary_equip"),Offhand<0?Offhand+4294967296.:Offhand);
                Monsters.Add(Obj(R));
            }
            else Issues.Add(TEXT("Character table needs an adapter: ")+Name);
        }
    }
    Out->SetArrayField(TEXT("buffs"),Buffs);Out->SetArrayField(TEXT("excluded_buff_spells"),Excluded);
    Out->SetArrayField(TEXT("assist_items"),Consumables);Out->SetArrayField(TEXT("buff_items"),BuffItems);
    const A* Recipes=nullptr;D->TryGetArrayField(TEXT("pea_recipes"),Recipes);
    for(const auto& Item:Consumables)if(N(Item->AsObject(),TEXT("type"))==9||N(Item->AsObject(),TEXT("type"))==11)
    {
        if(!Recipes||Recipes->IsEmpty()){Issues.AddUnique(TEXT("Pea splitting requires gameinfodb.ugd beside the source profile"));break;}
        if(N(Item->AsObject(),TEXT("type"))==9)
        {bool Found=false;for(const auto& Recipe:*Recipes)if(S(Recipe->AsObject(),TEXT("output"))==S(Item->AsObject(),TEXT("name")))Found=true;if(!Found)Issues.Add(TEXT("No splitting recipe for component: ")+S(Item->AsObject(),TEXT("name")));}
    }
    Out->SetArrayField(TEXT("pea_recipes"),Recipes?*Recipes:A{});
    const J* Catalog=nullptr;
    Out->SetObjectField(TEXT("monster_catalog"),D->TryGetObjectField(TEXT("monster_catalog"),Catalog)?*Catalog:MakeShared<FJsonObject>());
    Out->SetArrayField(TEXT("item_buff_targets"),ItemTargets);
    Out->SetArrayField(TEXT("weapon_items"),Equipment);Out->SetArrayField(TEXT("monsters"),Monsters);Out->SetArrayField(TEXT("recharge_handlers"),Handlers);
    if(Handlers.Num()>256||Monsters.Num()>512||ItemTargets.Num()>512||BuffItems.Num()>512||Consumables.Num()>512)Issues.Add(TEXT("Character settings exceed runtime collection limits"));
    A Keys;for(const auto& Pair:Out->Values)Keys.Add(Str(FString(Pair.Key)));Out->SetArrayField(TEXT("usd_keys"),Keys);
    return Out;
}
}
