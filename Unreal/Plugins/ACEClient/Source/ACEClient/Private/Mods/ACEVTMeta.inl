// Native compilation of legacy data only. Never execute Decal command strings.
namespace
{
#include "ACEVTExpression.inl"
J VTProperties(const V& Value)
{
    auto Out=MakeShared<FJsonObject>();const A* Rows=nullptr;
    if(Value->Type==EJson::Object&&Value->AsObject()->TryGetArrayField(TEXT("rows"),Rows))
        for(const auto& Row:*Rows){const auto& Cells=Row->AsArray();if(Cells.Num()==2&&Cells[0]->Type==EJson::String)Out->SetField(Cells[0]->AsString(),Cells[1]);}
    return Out;
}
J VTCommand(const FString& Source,TArray<FString>& Issues)
{
    auto Out=MakeShared<FJsonObject>();FString Command=NativeCommand(Source);
    Out->SetStringField(TEXT("source_command"),Command);
    // Parse both new and saved legacy commands through the same grammar; keep
    // the canonical acue spelling in serialized actions and diagnostic output.
    for(const auto& Alias:TArray<TPair<FString,FString>>{{TEXT("/acueucm"),TEXT("/ucm")},{TEXT("/acueplugin"),TEXT("/plugin")}})
        if(Command.StartsWith(Alias.Key) && (Command.Len()==Alias.Key.Len() || FChar::IsWhitespace(Command[Alias.Key.Len()])))
        {Command=Alias.Value+Command.Mid(Alias.Key.Len());break;}
    TArray<FString> Words;Command.ParseIntoArrayWS(Words);const FString Lower=Command.ToLower();
    if(Command.Len()>1024){Issues.Add(TEXT("Command exceeds 1024 characters"));return Out;}
    if(Command.IsEmpty()){Out->SetStringField(TEXT("op"),TEXT("noop"));return Out;}
    if(Lower.StartsWith(TEXT("/plugin "))&&Words.Num()>=2&&Command.Len()<=520&&!Command.Contains(TEXT("\n"))&&!Command.Contains(TEXT("\r")))
    {Out->SetStringField(TEXT("op"),TEXT("plugin"));Out->SetStringField(TEXT("text"),Command.Mid(8));return Out;}
    if(Lower==TEXT("/ucm tapjump")){Out->SetStringField(TEXT("op"),TEXT("jump"));Out->SetNumberField(TEXT("charge"),.1);return Out;}
    if(Lower==TEXT("/ucm reverseroute")||Lower==TEXT("/ucm reverseroutequery"))
    {Out->SetStringField(TEXT("op"),Lower.EndsWith(TEXT("query"))?TEXT("reverse_query"):TEXT("reverse_route"));return Out;}
    if(Lower.StartsWith(TEXT("/ucm setattackbar "))&&Words.Num()==3)
    {double Power=0;if(LexTryParseString(Power,*Words[2])&&FMath::IsFinite(Power)&&Power>=0&&Power<=1){Out->SetStringField(TEXT("op"),TEXT("attack_bar"));Out->SetNumberField(TEXT("power"),Power);return Out;}}
    if(Lower==TEXT("/ucm start")||Lower==TEXT("/ucm stop")){Out->SetStringField(TEXT("op"),Lower.EndsWith(TEXT("stop"))?TEXT("stop"):TEXT("start"));return Out;}
    auto Expression=[&](const FString& Text){VTExpression Parser{Text};auto E=Parser.Expr(1,0);Parser.Space();if(!Parser.Error.IsEmpty()||Parser.At!=Text.Len())Issues.Add(TEXT("Invalid command expression: ")+Parser.Error);Out->SetStringField(TEXT("op"),TEXT("expression"));Out->SetObjectField(TEXT("expression"),E);return Out;};
    if(Words.Num()==3&&Words[0].Equals(TEXT("/ucm"),ESearchCase::IgnoreCase)&&TArray<FString>{TEXT("enablecombat"),TEXT("enablebuffing"),TEXT("enablenav"),TEXT("enablelooting")}.Contains(Words[1].ToLower()))return VTCommand(TEXT("/ucm opt set ")+Words[1]+TEXT(" ")+Words[2],Issues);
    const TMap<FString,FString> Recalls={{TEXT("/mp"),TEXT("marketplace")},{TEXT("/ls"),TEXT("lifestone")},{TEXT("/ah"),TEXT("allegiance")},{TEXT("/hr"),TEXT("house")},{TEXT("/hom"),TEXT("mansion")}};
    if(const FString* Recall=Recalls.Find(Lower)){Out->SetStringField(TEXT("op"),TEXT("recall"));Out->SetStringField(TEXT("destination"),*Recall);return Out;}
    if(Lower.StartsWith(TEXT("/f "))){Out->SetStringField(TEXT("op"),TEXT("say"));Out->SetStringField(TEXT("text"),Command.Mid(3));Out->SetNumberField(TEXT("channel"),2048);return Out;}
    if(Lower.StartsWith(TEXT("/ucm mexec ")))return Expression(Command.Mid(11));
    if(Lower.StartsWith(TEXT("/mt face "))&&Words.Num()==3)
    {double Angle=0;if(LexTryParseString(Angle,*Words[2])&&FMath::IsFinite(Angle)&&Angle>=0&&Angle<=360){Out->SetStringField(TEXT("op"),TEXT("face"));Out->SetNumberField(TEXT("heading"),FMath::Fmod(Angle+90,360));return Out;}}
    if(Lower.StartsWith(TEXT("/ucm jump "))&&(Words.Num()==5||Words.Num()==6))
    {
        double Heading=0,Millis=0;const FString Shift=Words[3].ToLower(),Direction=Words.Num()==6?Words[5].ToLower():TEXT("forward");
        if(LexTryParseString(Heading,*Words[2])&&FMath::IsFinite(Heading)&&Heading>=0&&Heading<=700&&LexTryParseString(Millis,*Words[4])&&FMath::IsFinite(Millis)&&Millis>=0&&Millis<=10000
            &&(Shift==TEXT("true")||Shift==TEXT("false"))&&TArray<FString>{TEXT("forward"),TEXT("strafeleft"),TEXT("straferight")}.Contains(Direction))
        {
            Out->SetStringField(TEXT("op"),TEXT("jump"));Out->SetNumberField(TEXT("heading"),FMath::Fmod(Heading+90,360));Out->SetBoolField(TEXT("current_heading"),Heading>360);
            Out->SetBoolField(TEXT("walk_jump"),Shift==TEXT("true"));Out->SetNumberField(TEXT("charge"),FMath::Clamp(Millis/1000,0.,1.));
            Out->SetNumberField(TEXT("forward"),Direction==TEXT("forward")?1:0);Out->SetNumberField(TEXT("strafe"),Direction==TEXT("strafeleft")?-1:Direction==TEXT("straferight")?1:0);return Out;
        }
    }
    if(Words.Num()>=2&&Words[0]==TEXT("/mt")&&TArray<FString>{TEXT("jump"),TEXT("jumpw"),TEXT("jumpz"),TEXT("jumpx"),TEXT("jumpc"),TEXT("sjump"),TEXT("sjumpw"),TEXT("sjumpz"),TEXT("sjumpx"),TEXT("sjumpc")}.Contains(Words[1].ToLower()))
    {
        double Millis=0;const FString Mode=Words[1].ToLower();
        if(Words.Num()==2||(Words.Num()==3&&LexTryParseString(Millis,*Words[2])&&FMath::IsFinite(Millis)&&Millis>=0&&Millis<=10000))
        {Out->SetStringField(TEXT("op"),TEXT("jump"));Out->SetNumberField(TEXT("charge"),FMath::Clamp(Millis/1000,0.,1.));Out->SetNumberField(TEXT("forward"),Mode.EndsWith(TEXT("w"))?1:Mode.EndsWith(TEXT("x"))?-1:0);Out->SetNumberField(TEXT("strafe"),Mode.EndsWith(TEXT("z"))?-1:Mode.EndsWith(TEXT("c"))?1:0);Out->SetBoolField(TEXT("walk_jump"),Mode.StartsWith(TEXT("s")));return Out;}
    }
    if(Lower.StartsWith(TEXT("/mt fellow "))&&Words.Num()>=3)
    {
        const FString Operation=Words[2].ToLower(),Name=Words.Num()>3?Command.Mid(Command.Find(Words[3])):FString();
        if((Words.Num()==3&&TArray<FString>{TEXT("open"),TEXT("close"),TEXT("quit"),TEXT("disband")}.Contains(Operation))||(!Name.IsEmpty()&&(Operation==TEXT("create")||Operation==TEXT("recruit"))))
        {Out->SetStringField(TEXT("op"),TEXT("fellowship"));Out->SetStringField(TEXT("operation"),Operation);Out->SetStringField(TEXT("name"),Name);return Out;}
    }
    for(const auto& Prefix:TArray<FString>{TEXT("/ucm meta load "),TEXT("/ucm loot load "),TEXT("/ucm looting load "),TEXT("/ucm settings load "),TEXT("/ucm nav load ")})if(Lower.StartsWith(Prefix))
    {
        const FString Kind=Prefix.Contains(TEXT("meta"))?TEXT("met"):Prefix.Contains(TEXT("loot"))?TEXT("utl"):Prefix.Contains(TEXT("settings"))?TEXT("usd"):TEXT("nav");
        FString Name=Command.Mid(Prefix.Len()).TrimStartAndEnd();if(!Name.EndsWith(TEXT(".")+Kind,ESearchCase::IgnoreCase))Name+=TEXT(".")+Kind;
        if(Name.IsEmpty()||Name.Contains(TEXT("/"))||Name.Contains(TEXT("\\"))||Name.Contains(TEXT(".."))||Name.Contains(TEXT(":"))){Issues.Add(TEXT("Profile reference must be a filename in the import folder"));return Out;}
        Out->SetStringField(TEXT("op"),TEXT("load"));Out->SetStringField(TEXT("kind"),Kind);Out->SetStringField(TEXT("profile"),Name.ToLower());return Out;
    }
    for(const auto& Prefix:TArray<FString>{TEXT("/mt give "),TEXT("/mt givep ")})if(Lower.StartsWith(Prefix))
    {
        const FString Text=Command.Mid(Prefix.Len());const int To=Text.Find(TEXT(" to "),ESearchCase::IgnoreCase,ESearchDir::FromEnd);
        if(To>0&&To+4<Text.Len()){Out->SetStringField(TEXT("op"),TEXT("give"));Out->SetStringField(TEXT("item"),Text.Left(To));Out->SetStringField(TEXT("target"),Text.Mid(To+4));Out->SetBoolField(TEXT("prefix"),Prefix.Contains(TEXT("givep")));return Out;}
    }
    for(const auto& Prefix:TArray<FString>{TEXT("/mt select "),TEXT("/mt selectp ")})if(Lower.StartsWith(Prefix))
    {Out->SetStringField(TEXT("op"),TEXT("select"));Out->SetStringField(TEXT("item"),Command.Mid(Prefix.Len()));Out->SetBoolField(TEXT("prefix"),Prefix.Contains(TEXT("selectp")));return Out;}
    if(Lower==TEXT("/mt send msg r")){Out->SetStringField(TEXT("op"),TEXT("use_selected"));return Out;}
    if(Lower.StartsWith(TEXT("/mt combatstate "))&&Words.Num()==3)
    {
        const TMap<FString,int> Modes={{TEXT("peace"),1},{TEXT("melee"),2},{TEXT("missile"),4},{TEXT("magic"),8}};
        if(const int* Mode=Modes.Find(Words[2].ToLower())){Out->SetStringField(TEXT("op"),TEXT("combat_mode"));Out->SetNumberField(TEXT("mode"),*Mode);return Out;}
    }
    if(Lower==TEXT("/mt click ok")||Lower==TEXT("/mt click yes")||Lower==TEXT("/mt click no"))
    {Out->SetStringField(TEXT("op"),TEXT("confirm"));Out->SetBoolField(TEXT("accept"),Lower!=TEXT("/mt click no"));return Out;}
    if(Lower==TEXT("/mt logout")||Lower==TEXT("/mt logoff")){Out->SetStringField(TEXT("op"),TEXT("logout"));return Out;}
    if(Lower==TEXT("/ucm forcebuff")||Lower==TEXT("/ucm cancelforcebuff")){Out->SetStringField(TEXT("op"),TEXT("forcebuff"));Out->SetBoolField(TEXT("value"),Lower==TEXT("/ucm forcebuff"));return Out;}
    if(Lower.StartsWith(TEXT("/ucm echo "))){Out->SetStringField(TEXT("op"),TEXT("echo"));Out->SetStringField(TEXT("text"),Command.Mid(10));return Out;}
    if(Lower==TEXT("/mt send space")){Out->SetStringField(TEXT("op"),TEXT("jump"));Out->SetNumberField(TEXT("charge"),0);return Out;}
    for(const auto& PrefixText:TArray<FString>{TEXT("/mt use "),TEXT("/mt usep "),TEXT("/mt usei "),TEXT("/mt useip "),TEXT("/mt usel "),TEXT("/mt uselp ")})if(Lower.StartsWith(PrefixText))
    {
        const bool Prefix=PrefixText.EndsWith(TEXT("p "));const FString Name=Command.Mid(PrefixText.Len());const int On=Name.Find(TEXT(" on "),ESearchCase::IgnoreCase);
        Out->SetStringField(TEXT("op"),TEXT("use"));Out->SetStringField(TEXT("item"),On==INDEX_NONE?Name:Name.Left(On));
        Out->SetStringField(TEXT("scope"),PrefixText.Contains(TEXT("usel"))?TEXT("world"):PrefixText.Contains(TEXT("usei"))?TEXT("inventory"):TEXT("both"));
        if(On!=INDEX_NONE)Out->SetStringField(TEXT("target"),Name.Mid(On+4));Out->SetBoolField(TEXT("prefix"),Prefix);return Out;
    }
    if(Lower.StartsWith(TEXT("/say "))||Lower.StartsWith(TEXT("/s ")))
    {
        const FString Text=Command.Mid(Lower.StartsWith(TEXT("/say "))?5:3);
        if(Text.Len()<=500){Out->SetStringField(TEXT("op"),TEXT("say"));Out->SetStringField(TEXT("text"),Text);return Out;}
    }
    if(Words.Num()==5&&Lower.StartsWith(TEXT("/ucm opt set ")))
    {
        const FString Key=Words[3].ToLower(),Value=Words[4].ToLower();
        const TMap<FString,FString> SupplyNumbers={{TEXT("manastonelootcount"),TEXT("mana_stone_loot_count")},{TEXT("manatankminimummana"),TEXT("mana_tank_minimum")},{TEXT("staminatohealthmultiplier"),TEXT("stamina_health_multiplier")},{TEXT("manatohealthmultiplier"),TEXT("mana_health_multiplier")},{TEXT("ghostmonsterspellattemptcount"),TEXT("ghost_spell_attempts")},{TEXT("ghostdeletehptrackerseconds"),TEXT("ghost_hp_seconds")}};
        if(SupplyNumbers.Contains(Key))
        {
            double Number=0;const bool Fraction=Key.EndsWith(TEXT("multiplier"));
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=(Fraction?100.:1000000.)&&(Fraction||FMath::FloorToDouble(Number)==Number))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),SupplyNumbers[Key]);Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("switchwandstodebuff")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("switch_debuff_wand"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if(Key==TEXT("usearcs")||Key==TEXT("arcrange")||Key==TEXT("debuffeachfirst"))
        {
            double Number=0;const bool Range=Key==TEXT("arcrange");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&(Range?(Number>=0&&Number<=1):(Number>=1&&Number<=3&&FMath::FloorToDouble(Number)==Number)))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Range?TEXT("arc_range"):Key==TEXT("usearcs")?TEXT("use_arcs"):TEXT("debuff_each_first"));Out->SetNumberField(TEXT("value"),Range?Number*240:Number);return Out;}
        }
        if(Key==TEXT("blacklistmonsterattemptcount")||Key==TEXT("blacklistmonstertimeoutseconds"))
        {
            double Number=0;const bool Count=Key==TEXT("blacklistmonsterattemptcount");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=(Count?1000:86400)&&FMath::FloorToDouble(Number)==Number)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Count?TEXT("monster_attempts"):TEXT("monster_blacklist_seconds"));Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("blacklistcorpseopenattemptcount")||Key==TEXT("blacklistcorpseopentimeoutseconds"))
        {
            double Number=0;const bool Count=Key==TEXT("blacklistcorpseopenattemptcount");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=1&&Number<=(Count?1000:86400)&&FMath::FloorToDouble(Number)==Number)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Count?TEXT("corpse_attempts"):TEXT("corpse_blacklist_seconds"));Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("targetselectmethod")||Key==TEXT("targetselectanglerange")||Key==TEXT("attackminimumdistance"))
        {
            double Number=0;const bool Mode=Key==TEXT("targetselectmethod");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&(Mode?(Number>=1&&Number<=3&&FMath::FloorToDouble(Number)==Number):Number<=1))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Mode?TEXT("target_select"):Key==TEXT("attackminimumdistance")?TEXT("minimum_range"):TEXT("target_angle_range"));Out->SetNumberField(TEXT("value"),Mode?Number:Number*240);return Out;}
        }
        if(Key==TEXT("buffprofile_prots")||Key==TEXT("buffprofile_banes"))
        {
            double Number=0;
            if(LexTryParseString(Number,*Value)&&Number>=1&&Number<=8&&FMath::FloorToDouble(Number)==Number)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Key==TEXT("buffprofile_prots")?TEXT("protection_profile"):TEXT("bane_profile"));Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("buffprofile-prots")||Key==TEXT("buffprofile-banes"))
        {
            FString Selection=Value.ToUpper();bool Valid=Selection==TEXT("ALL")||Selection==TEXT("NONE");
            if(!Valid){Valid=Selection.Len()>0&&Selection.Len()<=7;for(TCHAR C:Selection)if(!FString(TEXT("ABCLFPS")).Contains(FString::Chr(C)))Valid=false;}
            if(Valid){Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Key==TEXT("buffprofile-prots")?TEXT("protection_custom"):TEXT("bane_custom"));Out->SetStringField(TEXT("value"),Selection);return Out;}
        }
        if(Key==TEXT("summonpets")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("summon_pets"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if(Key==TEXT("petrangemode")||Key==TEXT("petcustomrange")||Key==TEXT("petmonsterdensity"))
        {
            double Number=0;const bool Range=Key==TEXT("petcustomrange"),Mode=Key==TEXT("petrangemode");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&(Range?Number<=1:(FMath::FloorToDouble(Number)==Number&&Number<=(Mode?1:128)&& (Mode||Number>=1))))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Range?TEXT("pet_range"):Mode?TEXT("pet_range_mode"):TEXT("pet_min_targets"));Out->SetNumberField(TEXT("value"),Range?Number*240:Number);return Out;}
        }
        if(Key==TEXT("fastcastbuffs")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("fast_cast_buffs"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if((Key==TEXT("castdispelself")||Key==TEXT("usedispelitems"))&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Key==TEXT("castdispelself")?TEXT("dispel_self"):TEXT("dispel_items"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if(Key==TEXT("splitpeas")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("split_peas"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        const TMap<FString,FString> ComponentOptions={{TEXT("spellcompmin-critical"),TEXT("component_critical")},{TEXT("spellcompmin-normal"),TEXT("component_normal")},{TEXT("spellcompmin-idle"),TEXT("component_idle")}};
        if(ComponentOptions.Contains(Key))
        {
            double Number=0;if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=10000&&FMath::FloorToDouble(Number)==Number)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),ComponentOptions[Key]);Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("allowdebufffallback")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("debuff_fallback"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if(Key==TEXT("debuffprecastseconds"))
        {
            double Number=0;if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=300)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("debuff_refresh"));Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("ringdistance")||Key==TEXT("minimumringtargets"))
        {
            double Number=0;const bool Distance=Key==TEXT("ringdistance");
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>0
                &&(Distance?Number<=200./240.:(Number<=256&&FMath::FloorToDouble(Number)==Number)))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Distance?TEXT("ring_range"):TEXT("ring_min_targets"));Out->SetNumberField(TEXT("value"),Distance?Number*240:Number);return Out;}
        }
        if(Key==TEXT("targetlock")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("target_lock"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        const TMap<FString,FString> CraftCounts={{TEXT("idlecraftcount_healthkits"),TEXT("craft_health_kits")},{TEXT("idlecraftcount_healthfood"),TEXT("craft_health_food")},{TEXT("idlecraftcount_stamkits"),TEXT("craft_stamina_kits")},{TEXT("idlecraftcount_stamfood"),TEXT("craft_stamina_food")},{TEXT("idlecraftcount_manakits"),TEXT("craft_mana_kits")},{TEXT("idlecraftcount_manafood"),TEXT("craft_mana_food")}};
        if(Key==TEXT("randomhelperbuffs")&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("random_helper_buffs"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if((Key==TEXT("dojiggle")||Key==TEXT("jumpoutwandcasting"))&&(Value==TEXT("true")||Value==TEXT("false")))
        {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Key==TEXT("dojiggle")?TEXT("cast_jiggle"):TEXT("jump_out_wand"));Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;}
        if(Key==TEXT("randomhelperintervalseconds"))
        {
            double Number=0;if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=1&&Number<=86400)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("random_helper_interval"));Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        if(Key==TEXT("debuffselectionmethod"))
        {
            int32 Mode=Value.Equals(TEXT("SpellLevel"),ESearchCase::IgnoreCase)||Value==TEXT("1")?1:Value.Equals(TEXT("Skill"),ESearchCase::IgnoreCase)||Value==TEXT("2")?2:0;
            if(Mode){Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),TEXT("debuff_selection"));Out->SetNumberField(TEXT("value"),Mode);return Out;}
        }
        if(CraftCounts.Contains(Key))
        {
            double Number=0;if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=10000&&FMath::FloorToDouble(Number)==Number)
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),CraftCounts[Key]);Out->SetNumberField(TEXT("value"),Number);return Out;}
        }
        const TMap<FString,FString> ExtraNumbers={{TEXT("recharge-helper-hitp"),TEXT("helper_health_threshold")},{TEXT("recharge-helper-stam"),TEXT("helper_stamina_threshold")},{TEXT("recharge-helper-mana"),TEXT("helper_mana_threshold")},{TEXT("helperdistancehitp"),TEXT("helper_health_range")},{TEXT("helperdistancestam"),TEXT("helper_stamina_range")},{TEXT("helperdistancemana"),TEXT("helper_mana_range")},{TEXT("minimumhealkitsuccesschance"),TEXT("kit_min_success")},{TEXT("idlebufftopofftimeseconds"),TEXT("idle_buff_seconds")},{TEXT("refillwornmana-item-manapercent"),TEXT("item_mana_threshold")},{TEXT("spelldiffexcessthreshold-buff"),TEXT("buff_skill_margin")},{TEXT("spelldiffexcessthreshold-hunt"),TEXT("skill_margin")},{TEXT("recharge-notarg-hitp"),TEXT("idle_health_threshold")},{TEXT("recharge-notarg-stam"),TEXT("idle_stamina_threshold")},{TEXT("recharge-notarg-mana"),TEXT("idle_mana_threshold")},{TEXT("corpseapproachrange-max"),TEXT("loot_range")}};
        if(ExtraNumbers.Contains(Key))
        {
            double Number=0;const bool Distance=Key==TEXT("corpseapproachrange-max")||Key.StartsWith(TEXT("helperdistance"));
            if(LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0&&Number<=(Distance?200./240.:Key==TEXT("idlebufftopofftimeseconds")?86400.:Key.StartsWith(TEXT("spelldiff"))?10000.:100.))
            {Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),ExtraNumbers[Key]);Out->SetNumberField(TEXT("value"),Distance?Number*240:Number);return Out;}
        }
        const TMap<FString,FString> Bools={{TEXT("autocraftitems"),TEXT("auto_craft")},{TEXT("autofellowmanagement"),TEXT("auto_fellowship")},{TEXT("manachargeswhenoff"),TEXT("mana_charges_when_off")},{TEXT("deleteghostmonsters"),TEXT("delete_ghost_monsters")},{TEXT("deleteghostmonstersbyhptracker"),TEXT("delete_ghost_hp")},{TEXT("usekitsinmagicmode"),TEXT("kits_in_magic")},{TEXT("gotopeacemodetousekits"),TEXT("kit_peace")},{TEXT("idlebufftopoff"),TEXT("idle_buff_topoff")},{TEXT("autoattackpower"),TEXT("auto_attack_power")},{TEXT("userecklessness"),TEXT("use_recklessness")},{TEXT("lootfellowcorpses"),TEXT("loot_fellow_corpses")},{TEXT("lootallcorpses"),TEXT("loot_all_corpses")},{TEXT("refillwornmana"),TEXT("item_mana")},{TEXT("readunknownscrolls"),TEXT("read_unknown_scrolls")},{TEXT("enablebuffing"),TEXT("buffing")},{TEXT("enablenav"),TEXT("navigation")},{TEXT("enablelooting"),TEXT("looting")},{TEXT("autostack"),TEXT("autostack")},{TEXT("enablemeta"),TEXT("meta_enabled")},{TEXT("idlepeacemode"),TEXT("idle_peace")},{TEXT("opendoors"),TEXT("open_doors")},{TEXT("stopmacrondeath"),TEXT("stop_on_death")},{TEXT("stopmacroondeath"),TEXT("stop_on_death")},{TEXT("autocram"),TEXT("autocram")},{TEXT("navpriorityboost"),TEXT("nav_priority")},{TEXT("lootpriorityboost"),TEXT("loot_priority")},{TEXT("lootonlyrarecorpses"),TEXT("loot_only_rare")},{TEXT("autosalvage"),TEXT("salvage_combine")},{TEXT("combinesalvage"),TEXT("salvage_combine")},{TEXT("followaroundcorners"),TEXT("follow_corners")}};
        if((Bools.Contains(Key)||Key==TEXT("enablecombat"))&&(Value==TEXT("true")||Value==TEXT("false")))
        {
            Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Key==TEXT("enablecombat")?TEXT("combat"):Bools[Key]);
            if(Key==TEXT("enablecombat"))Out->SetStringField(TEXT("value"),Value==TEXT("true")?TEXT("auto"):TEXT("off"));else Out->SetBoolField(TEXT("value"),Value==TEXT("true"));return Out;
        }
        const TMap<FString,FString> Numbers={{TEXT("attackdistance"),TEXT("radius")},{TEXT("approachdistance"),TEXT("approach_range")},{TEXT("navclosestoprange"),TEXT("waypoint_radius")},{TEXT("rebufftimeremainingseconds"),TEXT("refresh_seconds")},{TEXT("defaultmeleeattackheight"),TEXT("height")},{TEXT("recharge-norm-mana"),TEXT("mana_threshold")},{TEXT("recharge-norm-hitp"),TEXT("health_threshold")},{TEXT("recharge-norm-stam"),TEXT("stamina_threshold")},{TEXT("dooropenrange"),TEXT("door_range")},{TEXT("doorlockpickdiffexcessthreshold"),TEXT("door_lockpick_margin")}};
        double Number=0;
        if(Numbers.Contains(Key)&&LexTryParseString(Number,*Value)&&FMath::IsFinite(Number)&&Number>=0)
        {
            if(Key.EndsWith(TEXT("distance"))||Key==TEXT("navclosestoprange")||Key==TEXT("dooropenrange"))Number*=240;
            if((Key.Contains(TEXT("recharge"))&&Number>100)||(Key==TEXT("defaultmeleeattackheight")&&(Number<1||Number>3))||(Key.EndsWith(TEXT("distance"))&&Number>200))
            {Issues.Add(TEXT("Option value is outside the supported range: ")+Command);return Out;}
            Out->SetStringField(TEXT("op"),TEXT("option"));Out->SetStringField(TEXT("key"),Numbers[Key]);Out->SetNumberField(TEXT("value"),Number);return Out;
        }
    }
    if(Lower.StartsWith(TEXT("/ucm setmetastate ")))
    {Out->SetStringField(TEXT("op"),TEXT("state"));Out->SetStringField(TEXT("state"),Command.Mid(18).TrimStartAndEnd());return Out;}
    // VT passes ordinary text through the game's chat parser as local speech.
    // Unknown slash commands still need an explicit adapter.
    if(!Command.IsEmpty()&&!Command.StartsWith(TEXT("/"))&&Command.Len()<=500&&!Command.Contains(TEXT("\n"))&&!Command.Contains(TEXT("\r")))
    {Out->SetStringField(TEXT("op"),TEXT("say"));Out->SetStringField(TEXT("text"),Command);return Out;}
    Issues.AddUnique(TEXT("Command needs an adapter: ")+Command);Out->SetStringField(TEXT("op"),TEXT("unsupported"));return Out;
}
J VTNode(int32 Type,const V& Data,bool Action,TArray<FString>& Issues,int32 Depth=0)
{
    auto Out=MakeShared<FJsonObject>();Out->SetNumberField(TEXT("type"),Type);Out->SetField(TEXT("value"),Data);
    if(Depth>24){Issues.AddUnique(TEXT("Meta nesting exceeds 24 levels."));return Out;}
    const bool Children=Action?Type==3:(Type==2||Type==3||Type==21);
    if(Children)
    {
        const A* Rows=nullptr;A Nodes;
        if(Data->Type!=EJson::Object||!Data->AsObject()->TryGetArrayField(TEXT("rows"),Rows)||Rows->Num()>256)Issues.AddUnique(TEXT("Invalid nested meta nodes."));
        else for(const auto& Row:*Rows){const auto& Cells=Row->AsArray();if(Cells.Num()!=2||Cells[0]->Type!=EJson::Number){Issues.AddUnique(TEXT("Invalid child node."));continue;}Nodes.Add(Obj(VTNode(int(Cells[0]->AsNumber()),Cells[1],Action,Issues,Depth+1)));}
        if(!Action&&Type==21&&Nodes.Num()!=1)Issues.AddUnique(TEXT("Not requires exactly one child."));
        Out->SetArrayField(TEXT("children"),Nodes);Out->RemoveField(TEXT("value"));return Out;
    }
    const J Props=VTProperties(Data);Out->SetObjectField(TEXT("properties"),Props);
    if(Data->Type==EJson::Object)Out->RemoveField(TEXT("value"));
    if((Action&&(Type==7||Type==8))||(!Action&&Type==26))
    {
        const FString Text=S(Props,TEXT("e"));VTExpression Parser{Text};auto Expression=Parser.Expr(1,0);Parser.Space();
        if(Parser.Error.IsEmpty()&&Parser.At!=Text.Len())Parser.Error=TEXT("Unexpected expression suffix");
        if(!Parser.Error.IsEmpty())Issues.AddUnique(Parser.Error+TEXT(": ")+Text.Left(200));
        else
        {
            if(Action&&Type==8)
            {
                // Only command-producing expressions are migrated. Chat match
                // patterns, names and ordinary expression strings stay intact.
                TFunction<void(const J&)> Rewrite=[&](const J& Node)
                {
                    if(S(Node,TEXT("op"))==TEXT("literal")&&Node->HasTypedField<EJson::String>(TEXT("value")))
                    {
                        const FString Literal=S(Node,TEXT("value")),Trimmed=Literal.TrimStartAndEnd();
                        const FString Native=NativeCommand(Literal);
                        if(Native!=Trimmed)Node->SetStringField(TEXT("value"),Literal.Left(Literal.Len()-Literal.TrimStart().Len())+Native+Literal.Right(Literal.Len()-Literal.TrimEnd().Len()));
                    }
                    // Only the beginning of a concatenated command is a command
                    // token. Arguments and nested function inputs remain data.
                    else if((S(Node,TEXT("op"))==TEXT("+")||S(Node,TEXT("op"))==TEXT("-"))&&Node->HasTypedField<EJson::Object>(TEXT("left")))Rewrite(Node->GetObjectField(TEXT("left")));
                };
                Rewrite(Expression);Props->RemoveField(TEXT("e"));
            }
            Out->SetObjectField(TEXT("expression"),Expression);
        }
        return Out;
    }
    if(Action)
    {
        if(Type==11||Type==12)
        {
            const FString Option=S(Props,TEXT("o"));TArray<FString> Problems;auto Mapping=VTCommand(TEXT("/ucm opt set ")+Option+TEXT(" true"),Problems);
            const bool Boolean=Problems.IsEmpty();double Probe=1;
            if(!Boolean)
            {
                Problems.Reset();Mapping=VTCommand(TEXT("/ucm opt set ")+Option+TEXT(" 1"),Problems);
                // One map unit is 240m, beyond some legitimate option limits.
                // Probe a smaller value, retaining the conversion ratio.
                if(!Problems.IsEmpty()){Probe=.1;Problems.Reset();Mapping=VTCommand(TEXT("/ucm opt set ")+Option+TEXT(" 0.1"),Problems);}
            }
            if(!Problems.IsEmpty()||S(Props,TEXT("v")).IsEmpty()){Issues.Add(TEXT("Unknown VT option or empty variable: ")+Option);return Out;}
            Out->SetStringField(TEXT("key"),S(Mapping,TEXT("key")));Out->SetBoolField(TEXT("boolean"),Boolean);
            Out->SetNumberField(TEXT("scale"),Boolean?1:N(Mapping,TEXT("value"),Probe)/Probe);
            if(Type==12){const FString Text=S(Props,TEXT("v"));VTExpression Parser{Text};auto E=Parser.Expr(1,0);Parser.Space();if(!Parser.Error.IsEmpty()||Parser.At!=Text.Len())Issues.Add(TEXT("Invalid VT option expression: ")+Text);else Out->SetObjectField(TEXT("expression"),E);}
            return Out;
        }
        if(Type==0||Type==6||Type==10)return Out;
        if(Type==1&&Data->Type==EJson::String)return Out;
        if(Type==2&&Data->Type==EJson::String){Out->SetStringField(TEXT("value"),NativeCommand(Data->AsString()));Out->SetObjectField(TEXT("command"),VTCommand(Data->AsString(),Issues));return Out;}
        if(Type==5&&Props->HasTypedField<EJson::String>(TEXT("st"))&&Props->HasTypedField<EJson::String>(TEXT("ret")))return Out;
        if(Type==9&&Props->HasTypedField<EJson::String>(TEXT("s"))&&N(Props,TEXT("r"))>0&&N(Props,TEXT("t"))>0)return Out;
        if(Type==4&&Data->Type==EJson::Object)
        {
            FString Blob=S(Data->AsObject(),TEXT("blob"));Reader R{Blob};const FString Name=R.Line();R.Count();
            FString Error;J Route;
            if(R.At==Blob.Len()) {Route=MakeShared<FJsonObject>();Route->SetArrayField(TEXT("route"),{});Route->SetBoolField(TEXT("loop_route"),false);Route->SetBoolField(TEXT("reverse_route"),false);}
            else if(auto Nav=Read(Blob.Mid(R.At),TEXT("nav"),Error)){TArray<FString> Problems;Route=Convert(Nav,Problems);for(const auto& P:Problems)Issues.AddUnique(TEXT("Embedded route ")+Name+TEXT(": ")+P);}
            if(!R.Error.IsEmpty()||!Error.IsEmpty()||!Route)Issues.AddUnique(TEXT("Invalid embedded route: ")+Name+TEXT(" ")+R.Error+Error);
            else { Out->SetObjectField(TEXT("route"),Route); Out->SetStringField(TEXT("route_name"),Name); }
            Out->RemoveField(TEXT("value"));return Out;
        }
    }
    else
    {
        if(Type==0||Type==1||Type==7||Type==8||Type==9||Type==10||Type==15||Type==19||Type==20)return Out;
        if((Type==5||Type==6||Type==22||Type==17||Type==18||Type==24)&&Data->Type==EJson::Number)return Out;
        if(Type==23&&Props->HasTypedField<EJson::Number>(TEXT("sid"))&&Props->HasTypedField<EJson::Number>(TEXT("sec")))return Out;
        if(Type==25&&Props->HasTypedField<EJson::Number>(TEXT("dist")))return Out;
        if(Type==14&&Props->HasTypedField<EJson::Number>(TEXT("p"))&&N(Props,TEXT("c"))>=0&&N(Props,TEXT("r"))<=200)return Out;
        if(Type==28&&IsSupportedPattern(S(Props,TEXT("p"))))return Out;
        if(Type==4&&Data->Type==EJson::String&&IsSupportedPattern(Data->AsString()))return Out;
        if((Type==11||Type==12||Type==13)&&Props->HasTypedField<EJson::String>(TEXT("n"))&&Props->HasTypedField<EJson::Number>(TEXT("c"))&&(Type!=13||(IsSupportedPattern(S(Props,TEXT("n")))&&N(Props,TEXT("c"))<=128&&N(Props,TEXT("r"))<=200)))return Out;
        if(Type==16&&Props->HasTypedField<EJson::Number>(TEXT("r"))&&N(Props,TEXT("r"))<=200)return Out;
    }
    Issues.AddUnique(FString::Printf(TEXT("Meta %s %d needs an adapter or has invalid data."),Action?TEXT("action"):TEXT("condition"),Type));return Out;
}
J VTMeta(const J& Document,TArray<FString>& Issues)
{
    auto Out=MakeShared<FJsonObject>();A Rules;bool HasRuleTable=false;
    for(const auto& T:Document->GetArrayField(TEXT("tables")))
    {
        const auto Table=T->AsObject();if(S(Table,TEXT("name"))!=TEXT("CondAct")){if(Table->GetArrayField(TEXT("rows")).Num())Issues.AddUnique(TEXT("Unsupported meta table: ")+S(Table,TEXT("name")));continue;}
        HasRuleTable=true;
        const auto& Columns=Table->GetArrayField(TEXT("columns"));
        if(Columns.Num()!=5||Columns[0]->AsString()!=TEXT("CType")||Columns[1]->AsString()!=TEXT("AType")||Columns[2]->AsString()!=TEXT("CData")||Columns[3]->AsString()!=TEXT("AData")||Columns[4]->AsString()!=TEXT("State"))
        {Issues.Add(TEXT("Invalid CondAct table schema."));continue;}
        for(const auto& Row:Table->GetArrayField(TEXT("rows")))
        {
            const auto& Cells=Row->AsArray();if(Cells.Num()!=5||Cells[0]->Type!=EJson::Number||Cells[1]->Type!=EJson::Number||Cells[4]->Type!=EJson::String){Issues.Add(TEXT("Invalid meta rule."));continue;}
            auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("state"),Cells[4]->AsString());TArray<FString> Problems;
            Rule->SetObjectField(TEXT("condition"),VTNode(int(Cells[0]->AsNumber()),Cells[2],false,Problems));
            Rule->SetObjectField(TEXT("action"),VTNode(int(Cells[1]->AsNumber()),Cells[3],true,Problems));
            for(const auto& P:Problems)Issues.AddUnique(FString::Printf(TEXT("Rule %d [%s]: %s"),Rules.Num()+1,*Cells[4]->AsString(),*P));Rules.Add(Obj(Rule));
        }
    }
    if(Rules.Num()>2048)Issues.Add(TEXT("Meta exceeds 2048 rules."));
    if(!HasRuleTable)Issues.Add(TEXT("Meta is missing its CondAct table."));
    Out->SetArrayField(TEXT("vt_meta"),Rules);Out->SetBoolField(TEXT("meta_enabled"),true);return Out;
}
}
