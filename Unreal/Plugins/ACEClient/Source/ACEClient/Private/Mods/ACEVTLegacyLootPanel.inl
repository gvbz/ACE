TSharedPtr<FJsonObject> LegacyRule;
int32 LegacyRuleIndex=INDEX_NONE,LegacyPage=0;
FString LegacyFilter;
bool ShowLegacyExtras=false;
FString LegacyPropertyKind(int32 Type,int32 Field)const
{
    if(Field==1&&(Type==1||Type==2||Type==3||Type==4||Type==5||Type==11||Type==12||Type==13||Type==2003||Type==2005))return Type==1?TEXT("string"):(Type==4||Type==5||Type==2005)?TEXT("float"):TEXT("int");
    if(Type==7&&Field==0)return TEXT("class");
    if((Type==1000&&Field==1)||(Type==1004&&Field==0))return TEXT("skill");
    return FString();
}
void SetLegacyValue(const TSharedPtr<FJsonObject>& Req,int32 Index,const FString& Value)
{
    TArray<FString> Fields;Str(Req,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(Fields,TEXT("\n"),false);
    if(Fields.Num()&&Fields.Last().IsEmpty())Fields.Pop();
    if(Fields.IsValidIndex(Index)){Fields[Index]=Value;Req->SetStringField(TEXT("body"),FString::Join(Fields,TEXT("\r\n"))+TEXT("\r\n"));}
}
FString LegacyRequirementSummary(const TSharedPtr<FJsonObject>& Req)const
{
    const int Type=int(Num(Req,TEXT("type")));const FString* Schema=LegacySchemas().Find(Type);
    if(!Schema)return FString::Printf(TEXT("Unknown requirement %d: "),Type)+Str(Req,TEXT("body"));
    TArray<FString> Fields,Values;Schema->ParseIntoArray(Fields,TEXT("|"));
    Str(Req,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(Values,TEXT("\n"),false);if(Values.Num()&&Values.Last().IsEmpty())Values.Pop();
    FString Summary=Fields[0];
    for(int I=0;I<Values.Num();++I)
    {
        const FString Kind=LegacyPropertyKind(Type,I);int32 Id=0;
        const FString Value=!Kind.IsEmpty()&&LexTryParseString(Id,*Values[I])?ACEUCMLootPresentation::PropertyLabel(Kind,Id):Values[I];
        Summary+=TEXT("\n")+(Fields.IsValidIndex(I+1)?Fields[I+1]:TEXT("Extra field"))+TEXT(": ")+Value;
    }
    if(Type>=14&&Type<=16)Summary+=TEXT("\nMatches the DAT palette sample using Classic hue and saturation/value tolerances.");
    return Summary;
}
const TMap<int32,FString>& LegacySchemas()const
{
    static const TMap<int32,FString> S={
        {0,TEXT("Spell name|Pattern")},{1,TEXT("String property|Pattern|String property ID")},
        {2,TEXT("Integer at most|Value|Integer property ID")},{3,TEXT("Integer at least|Value|Integer property ID")},
        {4,TEXT("Decimal at most|Value|Decimal property ID")},{5,TEXT("Decimal at least|Value|Decimal property ID")},
        {6,TEXT("Damage percent|Minimum")},{7,TEXT("Object class|Class ID")},{8,TEXT("Spell count|Minimum count")},
        {9,TEXT("Spell matching count|Include pattern|Exclude pattern|Minimum count")},{10,TEXT("Minimum damage|Minimum")},
        {11,TEXT("Integer flags|Flag mask|Integer property ID")},{12,TEXT("Integer equals|Value|Integer property ID")},{13,TEXT("Integer not equal|Value|Integer property ID")},
        {14,TEXT("Similar color|Red|Green|Blue|Hue tolerance|Saturation/value tolerance")},
        {15,TEXT("Armor color|Red|Green|Blue|Hue tolerance|Saturation/value tolerance|Armor group")},
        {16,TEXT("Slot color|Red|Green|Blue|Hue tolerance|Saturation/value tolerance|Slot")},
        {17,TEXT("Exact palette|Slot|Palette")},{1000,TEXT("Character skill|Minimum|Skill ID")},{1001,TEXT("Main pack space|Minimum free slots")},
        {1002,TEXT("Character minimum level|Minimum")},{1003,TEXT("Character maximum level|Maximum")},{1004,TEXT("Character base skill|Skill ID|Minimum|Maximum")},
        {2000,TEXT("Buffed median damage|Minimum")},{2001,TEXT("Buffed missile damage|Minimum")},{2003,TEXT("Buffed integer|Minimum|Property ID")},
        {2005,TEXT("Buffed decimal|Minimum|Property ID")},{2006,TEXT("Buffed tinkered damage|Minimum")},{2007,TEXT("Total ratings|Minimum")},
        {2008,TEXT("Tinkered melee target|Damage over time|Melee defense|Attack")},{9999,TEXT("Disable rule|Disabled (true / false)")}};
    return S;
}
void LegacyLootPanel()
{
    if(!LegacyDocument||Str(LegacyDocument,TEXT("format"))!=TEXT("utl")){Page=TEXT("Import");ImportPanel();return;}
    Heading(TEXT("Classic UTL document"),TEXT("Editing the original-format document, not the active converted rules. All requirement values, expressions and extra blocks are retained. Save a copy, or explicitly apply it after compatibility validation."));
    Add(Text(TEXT("Source: ")+(LegacyPath.IsEmpty()?TEXT("New document"):LegacyPath),13,Accent));
    Add(Button(TEXT("Back to active loot rules"),[this](){LegacyRule.Reset();Page=TEXT("Loot");Rebuild();}));
    auto Rules=LegacyDocument->GetArrayField(TEXT("rules"));
    if(LegacyRule)
    {
        auto Input=[&](const TCHAR* Key,const FString& Caption)
        {
            Add(Text(Caption,15,Muted));Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).Text(FText::FromString(Str(LegacyRule,Key))).OnTextChanged_Lambda([this,Key](const FText& V){LegacyRule->SetStringField(Key,V.ToString());}));
        };
        Input(TEXT("label"),TEXT("Rule name"));Input(TEXT("expression"),TEXT("Classic expression metadata (preserved; Classic does not evaluate it)"));
        const TArray<FString> Actions={TEXT("No loot"),TEXT("Keep"),TEXT("Salvage"),TEXT("Sell"),TEXT("Read"),TEXT("User 1"),TEXT("User 2"),TEXT("User 3"),TEXT("User 4"),TEXT("User 5"),TEXT("Keep up to")};
        auto Menu=SNew(SVerticalBox);for(int I=0;I<Actions.Num();++I)Menu->AddSlot().AutoHeight()[MenuButton(Actions[I],[this,I](){LegacyRule->SetNumberField(TEXT("action_id"),I);FSlateApplication::Get().DismissAllMenus();Rebuild();})];
        const int Action=int(Num(LegacyRule,TEXT("action_id")));
        Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(Actions.IsValidIndex(Action)?Actions[Action]:FString::Printf(TEXT("Custom action %d"),Action))].MenuContent()[Menu]);
        if(Action==10){Add(Text(TEXT("Keep # (matching display names; 0 keeps none)"),14,Muted));Add(SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",16)).MinValue(0).MaxValue(MAX_int32).Value(int(Num(LegacyRule,TEXT("amount"),1))).OnValueChanged_Lambda([this](int V){LegacyRule->SetNumberField(TEXT("amount"),V);}));}
        if(Action>=5&&Action<=9)Add(Text(TEXT("User actions need their original third-party plugin. The value can be edited and saved, but UCM will not activate it."),14,FLinearColor(1,.65f,.35f)));
        Add(Text(TEXT("Priority (stored by VT; file order controls first-match evaluation)"),14,Muted));
        Add(SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",16)).MinValue(MIN_int32).MaxValue(MAX_int32).Value(int(Num(LegacyRule,TEXT("priority")))).OnValueChanged_Lambda([this](int V){LegacyRule->SetNumberField(TEXT("priority"),V);}));
        auto Reqs=LegacyRule->GetArrayField(TEXT("requirements"));
        for(int I=0;I<Reqs.Num();++I)
        {
            auto Req=Reqs[I]->AsObject();const int Type=int(Num(Req,TEXT("type")));const FString* Schema=LegacySchemas().Find(Type);
            TArray<FString> Fields,Values;if(Schema)Schema->ParseIntoArray(Fields,TEXT("|"));
            Str(Req,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(Values,TEXT("\n"),false);if(Values.Num()&&Values.Last().IsEmpty())Values.Pop();
            Heading(Schema?Fields[0]:FString::Printf(TEXT("Unknown requirement %d"),Type),TEXT(""));
            if(Schema&&Values.Num()==Fields.Num()-1)
            {
                for(int J=0;J<Values.Num();++J)
                {
                    Add(Text(Fields[J+1],14,Muted));
                    const FString Kind=LegacyPropertyKind(Type,J);int32 Property=0;
                    if(!Kind.IsEmpty()&&LexTryParseString(Property,*Values[J]))
                    {Add(LootPropertyPicker(Kind,Property,[this,Req,J](int32 V){SetLegacyValue(Req,J,FString::FromInt(V));Rebuild();}));continue;}
                    if(Type==9999)
                    {Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked(Values[J].Equals(TEXT("true"),ESearchCase::IgnoreCase)?ECheckBoxState::Checked:ECheckBoxState::Unchecked).OnCheckStateChanged_Lambda([this,Req,J](ECheckBoxState S){SetLegacyValue(Req,J,S==ECheckBoxState::Checked?TEXT("true"):TEXT("false"));})[Text(TEXT("Disable this rule"))]);continue;}
                    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).Text(FText::FromString(Values[J])).OnTextChanged_Lambda([Req,J](const FText& V)
                    {
                        TArray<FString> Updated;Str(Req,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(Updated,TEXT("\n"),false);
                        if(Updated.Num()&&Updated.Last().IsEmpty())Updated.Pop();if(Updated.IsValidIndex(J)){Updated[J]=V.ToString();Req->SetStringField(TEXT("body"),FString::Join(Updated,TEXT("\r\n"))+TEXT("\r\n"));}
                    }));
                }
            }
            else Add(SNew(SBox).HeightOverride(100)[SNew(SACEPluginMultiLineEditableTextBox).Font(FACEPluginStyle::NativeFont("Regular",16)).Text(FText::FromString(Str(Req,TEXT("body")))).OnTextChanged_Lambda([Req](const FText& V){Req->SetStringField(TEXT("body"),V.ToString());})]);
            if(Type>=14&&Type<=16)
            {
                Add(Text(TEXT("RGB components: 0-255. Hue tolerance is in degrees; saturation/value tolerance is 0-1. Color requirements remain editable but cannot be activated in UCM yet."),13,FLinearColor(1,.65f,.35f)));
                Add(SNew(SBox).WidthOverride(72).HeightOverride(24).HAlign(HAlign_Left)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor_Lambda([Req]()
                {TArray<FString> C;Str(Req,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(C,TEXT("\n"),false);return C.Num()>=3?FLinearColor(FColor(FMath::Clamp(FCString::Atoi(*C[0]),0,255),FMath::Clamp(FCString::Atoi(*C[1]),0,255),FMath::Clamp(FCString::Atoi(*C[2]),0,255))):FLinearColor::White;})]);
            }
            for(int Direction:{-1,1})if(Reqs.IsValidIndex(I+Direction))Add(Button(Direction<0?TEXT("Move requirement up"):TEXT("Move requirement down"),[this,Reqs,I,Direction]()mutable{Reqs.Swap(I,I+Direction);LegacyRule->SetArrayField(TEXT("requirements"),Reqs);Rebuild();}));
            Add(Button(TEXT("Copy requirement"),[this,Req](){auto Values=LegacyRule->GetArrayField(TEXT("requirements"));Values.Add(MakeShared<FJsonValueObject>(ACEUCMLootPresentation::Clone(Req)));LegacyRule->SetArrayField(TEXT("requirements"),Values);Rebuild();}));
            Add(Button(TEXT("Remove requirement"),[this,I,Reqs]()mutable{Reqs.RemoveAt(I);LegacyRule->SetArrayField(TEXT("requirements"),Reqs);Rebuild();}));
        }
        auto Requirements=SNew(SVerticalBox);TArray<int32> Types;LegacySchemas().GetKeys(Types);Types.Sort();
        for(int Type:Types){TArray<FString> Fields;LegacySchemas()[Type].ParseIntoArray(Fields,TEXT("|"));Requirements->AddSlot().AutoHeight()[MenuButton(Fields[0],[this,Type,Fields]()
        {
            auto Req=MakeShared<FJsonObject>();Req->SetNumberField(TEXT("type"),Type);FString RequirementBody;
            for(int I=1;I<Fields.Num();++I)RequirementBody+=(Type==9999?TEXT("true"):Fields[I].Contains(TEXT("attern"))?TEXT(""):TEXT("0"))+FString(TEXT("\r\n"));
            Req->SetStringField(TEXT("body"),RequirementBody);auto List=LegacyRule->GetArrayField(TEXT("requirements"));List.Add(MakeShared<FJsonValueObject>(Req));LegacyRule->SetArrayField(TEXT("requirements"),List);FSlateApplication::Get().DismissAllMenus();Rebuild();
        })];}
        Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(TEXT("+ Add requirement"))].MenuContent()[SNew(SBox).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Requirements]]]);
        Add(Button(TEXT("Save rule draft"),[this,Rules]()mutable{auto Check=MakeShared<FJsonObject>();Check->SetStringField(TEXT("format"),TEXT("utl"));Check->SetArrayField(TEXT("rules"),{MakeShared<FJsonValueObject>(LegacyRule)});FString Error;ACEVTProfile::WriteLoot(Check,Error);if(!Error.IsEmpty()){Host->Notice=Error;return;}if(LegacyRuleIndex==INDEX_NONE)Rules.Add(MakeShared<FJsonValueObject>(LegacyRule));else if(Rules.IsValidIndex(LegacyRuleIndex))Rules[LegacyRuleIndex]=MakeShared<FJsonValueObject>(LegacyRule);LegacyDocument->SetArrayField(TEXT("rules"),Rules);LegacyRule.Reset();Rebuild();}));
        Add(Button(TEXT("Cancel rule changes"),[this](){LegacyRule.Reset();Rebuild();}));return;
    }
    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).HintText(FText::FromString(TEXT("Filter rule names"))).Text(FText::FromString(LegacyFilter)).OnTextCommitted_Lambda([this](const FText& V,ETextCommit::Type){LegacyFilter=V.ToString();LegacyPage=0;Rebuild();}));
    Add(Button(TEXT("+ New rule"),[this](){LegacyRule=MakeShared<FJsonObject>();LegacyRule->SetStringField(TEXT("label"),TEXT("New rule"));LegacyRule->SetNumberField(TEXT("action_id"),1);LegacyRule->SetNumberField(TEXT("amount"),1);LegacyRule->SetArrayField(TEXT("requirements"),{});LegacyRuleIndex=INDEX_NONE;Rebuild();}));
    TArray<int32> Visible;for(int I=0;I<Rules.Num();++I)if(LegacyFilter.IsEmpty()||Str(Rules[I]->AsObject(),TEXT("label")).Contains(LegacyFilter))Visible.Add(I);
    LegacyPage=FMath::Clamp(LegacyPage,0,FMath::Max(0,(Visible.Num()-1)/25));
    Add(Text(FString::Printf(TEXT("%d rules • %d matching • page %d"),Rules.Num(),Visible.Num(),LegacyPage+1),15,Muted));
    auto Paging=SNew(SHorizontalBox);Paging->AddSlot().FillWidth(1)[Button(TEXT("Previous"),[this](){LegacyPage=FMath::Max(0,LegacyPage-1);Rebuild();})];Paging->AddSlot().FillWidth(1)[Button(TEXT("Next"),[this](){++LegacyPage;Rebuild();})];Add(Paging);
    for(int Row=LegacyPage*25;Row<FMath::Min(Visible.Num(),(LegacyPage+1)*25);++Row)
    {
        const int I=Visible[Row];auto R=Rules[I]->AsObject();auto RuleCard=SNew(SVerticalBox);RuleCard->AddSlot().AutoHeight()[Text(FString::Printf(TEXT("%d. %s"),I+1,*Str(R,TEXT("label"))),16)];
        const TArray<FString> Actions={TEXT("Skip"),TEXT("Keep"),TEXT("Salvage"),TEXT("Sell"),TEXT("Read"),TEXT("User 1"),TEXT("User 2"),TEXT("User 3"),TEXT("User 4"),TEXT("User 5"),TEXT("Keep #")};const int Action=int(Num(R,TEXT("action_id")));
        RuleCard->AddSlot().AutoHeight()[Text(FString::Printf(TEXT("Action: %s | Priority: %.0f"),Actions.IsValidIndex(Action)?*Actions[Action]:TEXT("Custom"),Num(R,TEXT("priority")))+(Action==10?FString::Printf(TEXT(" | Count: %.0f"),Num(R,TEXT("amount"))):TEXT("")),14,Accent)];
        if(!Str(R,TEXT("expression")).IsEmpty())RuleCard->AddSlot().AutoHeight()[Text(TEXT("Expression metadata: ")+Str(R,TEXT("expression")),14,Muted)];
        for(const auto& Req:R->GetArrayField(TEXT("requirements")))RuleCard->AddSlot().AutoHeight().Padding(0,6)[Text(LegacyRequirementSummary(Req->AsObject()),14,Muted)];
        auto Buttons=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5,5));
        Buttons->AddSlot()[Button(TEXT("Edit"),[this,R,I](){FString Json;FJsonSerializer::Serialize(R,TJsonWriterFactory<>::Create(&Json));FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),LegacyRule);LegacyRuleIndex=I;Rebuild();})];
        Buttons->AddSlot()[Button(TEXT("Copy"),[this,R](){LegacyRule=ACEUCMLootPresentation::Clone(R);LegacyRule->SetStringField(TEXT("label"),Str(R,TEXT("label"))+TEXT(" copy"));LegacyRuleIndex=INDEX_NONE;Rebuild();})];
        for(int Direction:{-1,1})if(Rules.IsValidIndex(I+Direction))Buttons->AddSlot()[Button(Direction<0?TEXT("Up"):TEXT("Down"),[this,Rules,I,Direction]()mutable{Rules.Swap(I,I+Direction);LegacyDocument->SetArrayField(TEXT("rules"),Rules);Rebuild();})];
        Buttons->AddSlot()[Button(TEXT("Remove"),[this,Rules,I]()mutable{Rules.RemoveAt(I);LegacyDocument->SetArrayField(TEXT("rules"),Rules);Rebuild();})];RuleCard->AddSlot().AutoHeight()[Buttons];Add(Tile(RuleCard));
    }
    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).Text(FText::FromString(LegacyName)).HintText(FText::FromString(TEXT("Output filename"))).OnTextChanged_Lambda([this](const FText& V){LegacyName=V.ToString();}));
    Add(Button(TEXT("Save UTL copy"),[this](){Host->SaveLegacyLoot(LegacyName,LegacyDocument);}));
    Add(Button(TEXT("Save copy and apply to active loot"),[this]()
    {
        TArray<FString> Issues;auto Converted=ACEVTProfile::Convert(LegacyDocument,Issues);
        if(!Converted||Issues.Num()){Host->Notice=TEXT("Cannot activate: ")+FString::Join(Issues,TEXT("\n"));return;}
        if(!Host->SaveLegacyLoot(LegacyName,LegacyDocument))return;
        auto Updated=ACEUCMLootPresentation::Clone(P());Updated->RemoveField(TEXT("salvage_policy"));Updated->RemoveField(TEXT("loot_profile"));Updated->RemoveField(TEXT("loot_modified"));
        for(const auto& Pair:Converted->Values)Updated->Values.Add(Pair.Key,Pair.Value);
        Updated->SetStringField(TEXT("utl_source"),Host->UserDirectory()/TEXT("LegacyLootProfiles")/(LegacyName+TEXT(".utl")));
        FString Json;FJsonSerializer::Serialize(Updated.ToSharedRef(),TJsonWriterFactory<>::Create(&Json));
        if(Host->SaveProfile(Plugin->Id,Plugin->ProfileName,Json)){Page=TEXT("Loot");Rebuild();}
    }));
    Add(Button(TEXT("Check compatibility of saved copy"),[this](){if(Host->SaveLegacyLoot(LegacyName,LegacyDocument)){Page=TEXT("Import");PreviewLegacy(Host->UserDirectory()/TEXT("LegacyLootProfiles")/(LegacyName+TEXT(".utl")));}}));
    Add(Button(ShowLegacyExtras?TEXT("Hide extra file blocks"):TEXT("View / edit extra file blocks"),[this](){ShowLegacyExtras=!ShowLegacyExtras;Rebuild();}));
    if(ShowLegacyExtras)
    {
        Add(Text(TEXT("Advanced original-format data. Salvage combining can also be edited in the active loot settings. Invalid blocks prevent activation; unknown blocks are preserved."),14,Muted));
        const TArray<TSharedPtr<FJsonValue>>* Extras=nullptr;
        if(LegacyDocument->TryGetArrayField(TEXT("extras"),Extras))for(const auto& Extra:*Extras)
        {
            const auto Block=Extra->AsObject();Add(Text(Str(Block,TEXT("name")),16));
            Add(SNew(SBox).HeightOverride(160)[SNew(SACEPluginMultiLineEditableTextBox).Text(FText::FromString(Str(Block,TEXT("body")))).OnTextChanged_Lambda([Block](const FText& T){Block->SetStringField(TEXT("body"),T.ToString());})]);
        }
    }
}
