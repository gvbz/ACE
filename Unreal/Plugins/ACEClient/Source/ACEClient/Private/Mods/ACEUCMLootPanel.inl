// SUCMPanel members. The same editor is rendered on desktop and VR.
TSharedPtr<FJsonObject> LootDraft;
TSharedPtr<SScrollBox> LootRuleScroll;
FString LootRowsSignature,LootRowsSearch;
int32 LootRowsPage=INDEX_NONE;
int32 LootEditIndex=INDEX_NONE;
FString LootProfileName,LootSearch;
bool EditingSalvage=false;
bool ShowLootFilters=false,ShowLootOptions=false;
int32 SalvageMaterial=0;
TSharedPtr<FJsonObject> SalvageDraft;
int32 LootPage=0;
TSharedPtr<FJsonObject> ConditionDraft;
int32 ConditionIndex=INDEX_NONE;
FString LootDraftBaseline,ConditionBaseline;
TWeakPtr<FJsonObject> LootSourceProfile;
bool LootNarrow=false,ShowLootFiles=false,LootHasSource=false;

TSharedRef<SWidget> LootTool(const FString& Caption,TFunction<void()> Action)
{
    return SNew(SACEPluginButton).Tag(FName(*(TEXT("UCMLootTool.")+Caption))).ButtonStyle(&Style()).IsFocusable(false).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding))
        .OnClicked_Lambda([Action](){Action();return FReply::Handled();})[SNew(SACEPluginText).Text(FText::FromString(Caption)).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Ink)];
}
TSharedRef<SWidget> LootSelectRow(const FString& Caption,const FString& Detail,TAttribute<bool> Selected,TFunction<void()> Action)
{
    return SNew(SACEPluginTab).Selected(Selected).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding))
        .OnClicked_Lambda([Action](){Action();return FReply::Handled();})
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Text(FText::FromString(Caption)).Font(FACEPluginStyle::NativeFont("Regular",14)).AutoWrapText(true).ColorAndOpacity_Lambda([Selected]{return Selected.Get()?Accent:Ink;})]
             +SVerticalBox::Slot().AutoHeight()[Text(Detail,12,Muted)]];
}
FString LootDraftJson()const
{
    FString Json;if(LootDraft)FJsonSerializer::Serialize(LootDraft.ToSharedRef(),TJsonWriterFactory<>::Create(&Json));return Json;
}
bool CanLeaveLootRule()
{
    if(!CanLeaveLootRequirement())return false;
    if(LootDraft&&(LootEditIndex==INDEX_NONE||LootDraftJson()!=LootDraftBaseline))
    {Host->Notice=TEXT("Unsaved rule changes. Apply rule or Discard changes before choosing another rule or profile.");return false;}
    return true;
}
bool CanLeaveLootRequirement()
{
    if(!ConditionDraft)return true;
    FString Json;FJsonSerializer::Serialize(ConditionDraft.ToSharedRef(),TJsonWriterFactory<>::Create(&Json));
    if(ConditionIndex==INDEX_NONE||Json!=ConditionBaseline)
    {Host->Notice=TEXT("Apply or discard the requirement changes first.");return false;}
    return true;
}
void SelectLootRequirement(int32 Index)
{
    if(!CanLeaveLootRequirement())return;
    const TArray<TSharedPtr<FJsonValue>>* Conditions=nullptr;
    if(!LootDraft->TryGetArrayField(TEXT("conditions"),Conditions)||!Conditions->IsValidIndex(Index))return;
    ConditionIndex=Index;ConditionDraft=ACEUCMLootPresentation::Clone((*Conditions)[Index]->AsObject());
    ConditionBaseline.Reset();FJsonSerializer::Serialize(ConditionDraft.ToSharedRef(),TJsonWriterFactory<>::Create(&ConditionBaseline));Rebuild();
}
void SelectLootRule(int32 Index,bool Clone=false)
{
    if(!CanLeaveLootRule())return;
    const auto Rules=CurrentLootRules();if(!Rules.IsValidIndex(Index))return;
    LootDraft=ACEUCMLootPresentation::Clone(Rules[Index]->AsObject());LootEditIndex=Clone?INDEX_NONE:Index;
    if(Clone)LootDraft->SetStringField(TEXT("label"),Str(LootDraft,TEXT("label"))+TEXT(" copy"));
    LootDraftBaseline=LootDraftJson();ConditionDraft.Reset();ConditionIndex=INDEX_NONE;ShowLootFilters=false;
    Rebuild();
}
void NewLootRule()
{
    if(!CanLeaveLootRule())return;
    LootDraft=MakeShared<FJsonObject>();LootDraft->SetStringField(TEXT("label"),TEXT("New rule"));
    LootDraft->SetStringField(TEXT("action"),TEXT("keep"));LootDraft->SetStringField(TEXT("name_mode"),TEXT("prefix"));
    LootEditIndex=INDEX_NONE;ConditionIndex=INDEX_NONE;ConditionDraft.Reset();ShowLootFilters=false;Rebuild();
}
void ApplyLootRule()
{
    if(!LootDraft)return;
    if(!CanLeaveLootRequirement())return;
    auto Rules=CurrentLootRules();
    const int32 PreviousIndex=LootEditIndex;
    if(LootEditIndex==INDEX_NONE)
    {
        if(Rules.Num()>=2048){Host->Notice=TEXT("Maximum 2048 loot rules.");return;}
        LootEditIndex=Rules.Num();Rules.Add(MakeShared<FJsonValueObject>(LootDraft));
    }
    else if(Rules.IsValidIndex(LootEditIndex))Rules[LootEditIndex]=MakeShared<FJsonValueObject>(LootDraft);
    if(!WriteLootRules(Rules)){LootEditIndex=PreviousIndex;Rebuild();}
}
void DiscardLootRule()
{
    LootDraft.Reset();ConditionDraft.Reset();ConditionIndex=INDEX_NONE;Rebuild();
}
void LootTabs()
{
    auto Menu=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
    const TCHAR* Names[]={TEXT("Rules"),TEXT("Salvage combination"),TEXT("Save as"),TEXT("Looting settings")};
    for(int32 I=0;I<4;++I)
        Menu->AddSlot()[SNew(SACEPluginTab).Tag(TEXT("UCMLootTab")).Text(FText::FromString(Names[I])).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding))
            .Selected_Lambda([this,I](){return I==(EditingSalvage?1:ShowLootFiles?2:ShowLootOptions?3:0);})
            .OnClicked_Lambda([this,I](){
                if(I==(EditingSalvage?1:ShowLootFiles?2:ShowLootOptions?3:0))return FReply::Handled();
                if(!CanLeaveLootRule())return FReply::Handled();
                if(EditingSalvage)SalvageDraft.Reset();
                EditingSalvage=I==1;ShowLootFiles=I==2;ShowLootOptions=I==3;Rebuild();return FReply::Handled();})];
    Add(Menu);
}
void LootPanel()
{
    if(EditingSalvage){LootTabs();SalvagePanel();return;}
    if(LootHasSource&&LootSourceProfile.Pin()!=P())
    {LootDraft.Reset();ConditionDraft.Reset();LootEditIndex=INDEX_NONE;ConditionIndex=INDEX_NONE;LootPage=0;LootRuleScroll.Reset();}
    LootSourceProfile=P();LootHasSource=true;
    const auto Rules=CurrentLootRules();
    if(!LootDraft&&Rules.Num())
    {
        LootEditIndex=FMath::Clamp(LootEditIndex,0,Rules.Num()-1);
        LootDraft=ACEUCMLootPresentation::Clone(Rules[LootEditIndex]->AsObject());LootDraftBaseline=LootDraftJson();
    }
    if(Host->UCMSectionName(TEXT("Loot")).IsEmpty())
    {
        const FString Imported=FPaths::GetCleanFilename(Str(P(),TEXT("utl_source")));
        const FString Source=Imported.IsEmpty()?Str(P(),TEXT("loot_profile")):Imported;
        if(!Source.IsEmpty())Add(Text(TEXT("Source: ")+Source,16,Accent));
    }
    if(Bool(P(),TEXT("loot_modified")))Add(Text(TEXT("Rules edited"),14,Muted));
    LootTabs();
    if(ShowLootFiles){LootProfileFiles();return;}
    if(ShowLootOptions){LootSettings();return;}
    const auto NewRule=LootTool(TEXT("New rule"),[this](){NewLootRule();});
    if(Rules.IsEmpty()&&!LootDraft)
    {
        Add(Text(TEXT("This profile has no loot rules."),15,Muted));
        Add(NewRule);
        return;
    }
    Add(Text(TEXT("Rules run in order. The first match wins."),14,Muted));
    const auto Outer=Body,OuterSection=SectionBody;SectionBody.Reset();
    auto RuleList=SNew(SVerticalBox).Tag(TEXT("LootRuleList"));Body=RuleList;
    Add(Text(TEXT("Loot rules"),18));
    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).HintText(FText::FromString(TEXT("Find a rule..."))).Text(FText::FromString(LootSearch))
        .OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type){LootSearch=T.ToString();LootPage=0;Rebuild();}));
    TArray<int32> Visible;for(int I=0;I<Rules.Num();++I)if(LootSearch.IsEmpty()||Str(Rules[I]->AsObject(),TEXT("label")).Contains(LootSearch))Visible.Add(I);
    LootPage=FMath::Clamp(LootPage,0,FMath::Max(0,(Visible.Num()-1)/50));
    FString Signature;
    for(int Row=LootPage*50;Row<FMath::Min(Visible.Num(),(LootPage+1)*50);++Row)
    {
        const int I=Visible[Row];const auto R=Rules[I]->AsObject();
        const FString Label=Str(R,TEXT("label"),TEXT("Unnamed rule")),Action=Str(R,TEXT("action"),TEXT("keep"));
        Signature+=FString::Printf(TEXT("%d:%d:%s:%d:%s:%d;"),I,Label.Len(),*Label,Action.Len(),*Action,Bool(R,TEXT("enabled"),true));
    }
    const bool NewPage=LootRowsPage!=LootPage||LootRowsSearch!=LootSearch;
    if(!LootRuleScroll||NewPage||LootRowsSignature!=Signature)
    {
        const float Offset=LootRuleScroll&&!NewPage?LootRuleScroll->GetScrollOffset():0.f;
        auto Rows=SNew(SVerticalBox);
        for(int Row=LootPage*50;Row<FMath::Min(Visible.Num(),(LootPage+1)*50);++Row)
        {
            const int I=Visible[Row];const auto R=Rules[I]->AsObject();
            Rows->AddSlot().AutoHeight().Padding(0,0,0,2)[LootSelectRow(FString::Printf(TEXT("%d. %s"),I+1,*Str(R,TEXT("label"),TEXT("Unnamed rule"))),
                Str(R,TEXT("action"),TEXT("keep"))+(Bool(R,TEXT("enabled"),true)?TEXT(""):TEXT(" / disabled")),
                TAttribute<bool>::CreateLambda([this,I]{return LootEditIndex==I;}),[this,I](){SelectLootRule(I);})];
        }
        if(Visible.IsEmpty())Rows->AddSlot().AutoHeight()[Text(Rules.IsEmpty()?TEXT("No saved rules yet."):TEXT("No matching rules."),14,Muted)];
        LootRuleScroll=SNew(SScrollBox)+SScrollBox::Slot()[Rows];LootRuleScroll->SetScrollOffset(Offset);
        LootRowsSignature=Signature;LootRowsPage=LootPage;LootRowsSearch=LootSearch;
    }
    // Retain the widgets and their measured wrapping while only the detail pane changes.
    RuleList->AddSlot().FillHeight(1)[LootRuleScroll.ToSharedRef()];
    auto Paging=SNew(SHorizontalBox);
    Paging->AddSlot().AutoWidth()[SNew(SBox).IsEnabled(LootPage>0)[LootTool(TEXT("<"),[this](){--LootPage;Rebuild();})]];
    int32 EnabledCount=0;for(const auto& Rule:Rules)if(Bool(Rule->AsObject(),TEXT("enabled"),true))++EnabledCount;
    Paging->AddSlot().FillWidth(1).VAlign(VAlign_Center).Padding(6,0)[Text(FString::Printf(TEXT("%d active / %d disabled | %d matches | page %d / %d"),EnabledCount,Rules.Num()-EnabledCount,Visible.Num(),LootPage+1,FMath::Max(1,(Visible.Num()+49)/50)),12,Muted)];
    Paging->AddSlot().AutoWidth()[SNew(SBox).IsEnabled((LootPage+1)*50<Visible.Num())[LootTool(TEXT(">"),[this](){++LootPage;Rebuild();})]];
    Add(Paging);
    auto Tools=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
    Tools->AddSlot()[NewRule];
    Tools->AddSlot()[SNew(SBox).IsEnabled(Rules.IsValidIndex(LootEditIndex))[LootTool(TEXT("Clone"),[this](){SelectLootRule(LootEditIndex,true);})]];
    Tools->AddSlot()[SNew(SBox).IsEnabled(Rules.IsValidIndex(LootEditIndex))[LootTool(TEXT("Delete"),[this](){if(!CanLeaveLootRule())return;auto R=CurrentLootRules();if(R.IsValidIndex(LootEditIndex)){R.RemoveAt(LootEditIndex);WriteLootRules(R);}})]];
    for(int Direction:{-1,1})Tools->AddSlot()[SNew(SBox).IsEnabled(Rules.IsValidIndex(LootEditIndex)&&Rules.IsValidIndex(LootEditIndex+Direction))
        [LootTool(Direction<0?TEXT("Move up"):TEXT("Move down"),[this,Direction](){if(!CanLeaveLootRule())return;auto R=CurrentLootRules();if(!R.IsValidIndex(LootEditIndex+Direction))return;R.Swap(LootEditIndex,LootEditIndex+Direction);LootEditIndex+=Direction;WriteLootRules(R);})]];
    Add(Tools);
    auto Detail=SNew(SVerticalBox).Tag(TEXT("LootRuleDetail"));Body=Detail;SectionBody.Reset();
    Add(Text(LootEditIndex==INDEX_NONE?TEXT("New rule"):FString::Printf(TEXT("Rule %d"),LootEditIndex+1),18));
    auto Fields=SNew(SVerticalBox);Body=Fields;LootDetails();Body=Detail;SectionBody.Reset();
    Detail->AddSlot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[Fields]];
    Add(Text(TEXT("Apply rule saves these changes and stops UCM."),13,Accent));
    auto Footer=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,4));
    Footer->AddSlot()[LootTool(TEXT("Apply rule"),[this](){ApplyLootRule();})];
    Footer->AddSlot()[LootTool(TEXT("Discard changes"),[this](){DiscardLootRule();})];Add(Footer);
    Body=Outer;SectionBody=OuterSection;
    if(LootNarrow)Add(SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(280)[Tile(RuleList)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SBox).HeightOverride(640)[Tile(Detail)]]);
    else Add(SNew(SBox).HeightOverride_Lambda([this](){return FMath::Clamp(GetCachedGeometry().GetLocalSize().Y-350.f,390.f,1000.f);})
        [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(.34f).Padding(0,0,8,0)[Tile(RuleList)]
         +SHorizontalBox::Slot().FillWidth(.66f)[Tile(Detail)]]);
}

TSharedRef<SWidget> LootPropertyPicker(const FString& Field,int32 Current,TFunction<void(int32)> Changed)
{
    auto Row=SNew(SVerticalBox);
    Row->AddSlot().AutoHeight()[SNew(SACEPluginComboButton).Tag(FName(*(TEXT("UCMLootPicker.")+Field))).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
        .ButtonContent()[Text(ACEUCMLootPresentation::PropertyLabel(Field,Current),15)]
        .OnGetMenuContent_Lambda([Field,Changed]()->TSharedRef<SWidget>
        {
            auto List=SNew(SVerticalBox);
            auto Populate=[Field,Changed,List](const FString& Filter)
            {
                List->ClearChildren();int Count=0;
                for(const auto& Entry:ACEUCMLootPresentation::Properties(Field))
                {
                    const FString Label=FString::Printf(TEXT("%s (%d)"),Entry.Name,Entry.Id);
                    if(!Filter.IsEmpty()&&!Label.Contains(Filter))continue;
                    if(++Count>80)break;
                    List->AddSlot().AutoHeight()[MenuButton(Label,[Changed,Id=Entry.Id](){FSlateApplication::Get().DismissAllMenus();Changed(Id);})];
                }
                if(Count>80)List->AddSlot().AutoHeight()[Text(TEXT("Type to narrow the remaining properties."),13,Muted)];
                if(!Count)List->AddSlot().AutoHeight()[Text(TEXT("No named match. Enter a custom ID below the selector."),13,Muted)];
            };
            Populate(FString());
            return SNew(SBox).WidthOverride(310).MaxDesiredHeight(330)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).HintText(FText::FromString(Field==TEXT("weapon_skill")?TEXT("Search skill name or ID"):TEXT("Search property name or ID"))).OnTextChanged_Lambda([Populate](const FText& T){Populate(T.ToString());})]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[List]]];
        })];
    Row->AddSlot().AutoHeight().Padding(0,4)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Text(TEXT("ID: "),14,Muted)]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SACEPluginSpinBox<int32>).Tag(FName(*(TEXT("UCMLootID.")+Field))).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(MAX_int32).Value(Current)
            .OnValueCommitted_Lambda([Changed](int32 V,ETextCommit::Type){Changed(V);})]];
    return Row;
}

void LootText(const TCHAR* Key,const FString& Caption)
{
    Add(Text(Caption,15,Muted));
    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16))
        .Text(FText::FromString(Str(LootDraft,Key)))
        .OnTextChanged_Lambda([this,Key](const FText& V){LootDraft->SetStringField(Key,V.ToString());}));
}
void LootChoice(const TCHAR* Key,const FString& Caption,const TArray<TPair<FString,FString>>& Options,bool Numeric=false)
{
    Add(Text(Caption,15,Muted));
    auto Menu=SNew(SVerticalBox);
    for(const auto& Option:Options)Menu->AddSlot().AutoHeight()[MenuButton(Option.Value,[this,Key,Option,Numeric]()
    {
        if(Numeric)LootDraft->SetNumberField(Key,FCString::Atod(*Option.Key));else LootDraft->SetStringField(Key,Option.Key);
        FSlateApplication::Get().DismissAllMenus();
    })];
    Add(SNew(SACEPluginComboButton).Tag(FName(*(FString(TEXT("UCMLootChoice."))+Key))).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
        .ButtonContent()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",16)).ColorAndOpacity(Ink)
            .Text_Lambda([this,Key,Options,Numeric]()
            {
                const FString Value=Numeric?FString::Printf(TEXT("%.0f"),Num(LootDraft,Key)):Str(LootDraft,Key,FString(Key)==TEXT("conditions_mode")?TEXT("all"):TEXT(""));
                for(const auto& O:Options)if(O.Key==Value)return FText::FromString(O.Value);
                return FText::FromString(Value.IsEmpty()?TEXT("Choose"):Value);
            })]
        .MenuContent()[SNew(SBox).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Menu]]]);
}
void LootRange(const FString& Field,const FString& Caption,double Maximum)
{
    Add(Text(Caption+TEXT(" — minimum / maximum (0 = no limit)"),15,Muted));
    auto Row=SNew(SHorizontalBox);
    for(const FString Prefix:{TEXT("min_"),TEXT("max_")})
    {
        const FString Key=Prefix+Field;
        Row->AddSlot().FillWidth(1).Padding(0,0,8,0)[SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(Maximum)
            .Font(FACEPluginStyle::NativeFont("Regular",16)).Value(Num(LootDraft,*Key))
            .OnValueChanged_Lambda([this,Key](double V){LootDraft->SetNumberField(Key,V);})];
    }
    Add(Row);
}
TArray<TSharedPtr<FJsonValue>> CurrentLootRules() const
{
    const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;
    return P()->TryGetArrayField(TEXT("loot_rules"),Rules)?*Rules:TArray<TSharedPtr<FJsonValue>>{};
}
bool WriteLootRules(const TArray<TSharedPtr<FJsonValue>>& Rules)
{
    auto Updated=MakeShared<FJsonObject>();Updated->Values=P()->Values;
    Updated->SetArrayField(TEXT("loot_rules"),Rules);
    Updated->SetBoolField(TEXT("loot_modified"),true);
    FString Json;FJsonSerializer::Serialize(Updated,TJsonWriterFactory<>::Create(&Json));
    if(!Host->SaveProfile(Plugin->Id,Plugin->ProfileName,Json))return false;
    LootSourceProfile=P();LootDraft.Reset();ConditionDraft.Reset();ConditionIndex=INDEX_NONE;Rebuild();return true;
}
void LootDetails()
{
        const TArray<TSharedPtr<FJsonValue>>* Problems=nullptr;
        const bool NeedsAdapter=LootDraft->TryGetArrayField(TEXT("compatibility_issues"),Problems)&&Problems->Num();
        Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsEnabled(!NeedsAdapter).IsChecked(Bool(LootDraft,TEXT("enabled"),true)?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State){LootDraft->SetBoolField(TEXT("enabled"),State==ECheckBoxState::Checked);})[Text(TEXT("Enable this rule"),14)]);
        if(NeedsAdapter)
        {
            Add(Text(TEXT("Disabled: original Classic requirements need an adapter. Use Looting settings > original Classic UTL copy to edit preserved values."),14,FLinearColor(1,.65f,.35f)));
            for(const auto& Problem:*Problems)Add(Text(Problem->AsString(),13,Muted));
        }
        LootText(TEXT("label"),TEXT("Rule name"));
        LootChoice(TEXT("action"),TEXT("When this rule matches"),{{TEXT("keep"),TEXT("Keep in inventory")},{TEXT("skip"),TEXT("Skip / leave on corpse")},{TEXT("salvage"),TEXT("Loot, then salvage with Ust (destroys item)")},{TEXT("sell"),TEXT("Loot, then sell at the next open vendor")},{TEXT("read"),TEXT("Loot and read a spell scroll")}});
        LootConditions();
        Add(Button(ShowLootFilters?TEXT("Hide basic item filters"):TEXT("Show basic item filters (name, material, value and workmanship)"),[this](){ShowLootFilters=!ShowLootFilters;Rebuild();}));
        if(!ShowLootFilters)
        {
            FString Summary;
            if(!Str(LootDraft,TEXT("name")).IsEmpty())Summary=TEXT("Name ")+Str(LootDraft,TEXT("name_mode"),TEXT("prefix"))+TEXT(": ")+Str(LootDraft,TEXT("name"));
            if(Num(LootDraft,TEXT("material")))Summary+=TEXT("\nMaterial: ")+FString(ACERetailObjectNames::GetMaterialTypeName(uint32(Num(LootDraft,TEXT("material")))));
            if(Num(LootDraft,TEXT("type")))Summary+=FString::Printf(TEXT("\nItem type mask: %.0f"),Num(LootDraft,TEXT("type")));
            for(const TCHAR* Field:{TEXT("workmanship"),TEXT("value"),TEXT("burden"),TEXT("rating")})
            {
                const double Lo=Num(LootDraft,*(FString(TEXT("min_"))+Field)),Hi=Num(LootDraft,*(FString(TEXT("max_"))+Field));
                if(Lo>0||Hi>0)Summary+=FString::Printf(TEXT("\n%s: %g to %s"),Field,Lo,Hi>0?*FString::Printf(TEXT("%g"),Hi):TEXT("any"));
            }
            if(!Summary.IsEmpty())Add(Text(Summary.TrimStartAndEnd(),14,Accent));
        }
    if(ShowLootFilters)
        {
        LootText(TEXT("name"),TEXT("Item name (blank = any)"));
        LootChoice(TEXT("name_mode"),TEXT("Name matching — case insensitive"),{{TEXT("prefix"),TEXT("Begins with")},{TEXT("exact"),TEXT("Exact name")},{TEXT("contains"),TEXT("Contains text")},{TEXT("any"),TEXT("OR - any text separated by |")},{TEXT("all"),TEXT("AND - all text separated by |")},{TEXT("regex"),TEXT("Regular expression (VT)")}});
        TArray<TPair<FString,FString>> Materials={{TEXT("0"),TEXT("Any material")}};
        TArray<TPair<FString,FString>> Named;
        for(int32 Id=1;Id<=77;++Id)Named.Add({FString::FromInt(Id),ACERetailObjectNames::GetMaterialTypeName(Id)});
        Named.Sort([](const auto& A,const auto& B){return A.Value<B.Value;});Materials.Append(Named);
        LootChoice(TEXT("material"),TEXT("Salvage material"),Materials,true);
        LootChoice(TEXT("type"),TEXT("Item type"),{{TEXT("0"),TEXT("Any type")},{TEXT("1"),TEXT("Melee weapon")},{TEXT("256"),TEXT("Missile weapon / ammunition")},{TEXT("32768"),TEXT("Casting device")},{TEXT("2"),TEXT("Armor")},{TEXT("4"),TEXT("Clothing")},{TEXT("8"),TEXT("Jewelry")},{TEXT("32"),TEXT("Food")},{TEXT("4096"),TEXT("Spell component")},{TEXT("1073741824"),TEXT("Salvage bag")}},true);
        LootRange(TEXT("workmanship"),TEXT("Salvage workmanship"),1000);
        LootRange(TEXT("value"),TEXT("Item / stack value (pyreals)"),MAX_int32);
        LootRange(TEXT("burden"),TEXT("Item / stack burden"),MAX_int32);
        LootRange(TEXT("rating"),TEXT("Damage rating (requires appraisal)"),10000);
        }
        Add(Text(TEXT("Keep up to, including equipped items (0 = unlimited)"),15,Muted));
        Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(double(MAX_uint32)).Delta(1).Value(Num(LootDraft,TEXT("keep_up_to")))
            .OnValueChanged_Lambda([this](double V){LootDraft->SetNumberField(TEXT("keep_up_to"),V);}));
        Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked(Bool(LootDraft,TEXT("count_by_name"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState S){LootDraft->SetBoolField(TEXT("count_by_name"),S==ECheckBoxState::Checked);})
            [Text(TEXT("Count matching display names (Classic Keep #); off counts item templates"),14)]);
        Add(Button(TEXT("Use selected item's name and material"),[this]()
        {
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Item;
            if(!C->GetWorldObject(C->GetSelectedObject().Guid,Item)){Host->Notice=TEXT("Select an item first.");return;}
            LootDraft->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));LootDraft->SetStringField(TEXT("name_mode"),TEXT("exact"));
            LootDraft->SetNumberField(TEXT("material"),Item.MaterialType);Rebuild();
        }));

}
void LootNotificationSettings()
{
    Add(Toggle(TEXT("loot_notify_pickup"),TEXT("Announce looted rule matches"),TEXT("Print the item, amount and matching rule in local chat after UCM confirms the pickup."),true));
    Add(Toggle(TEXT("loot_notify_match"),TEXT("Announce item ID matches"),TEXT("Show the matching rule when you select or assess an item, even with UCM stopped. Background scans stay silent."),true));
}
void LootSettings()
{
    LootNotificationSettings();
    const FString Original=Str(P(),TEXT("utl_source"));
    if(!Original.IsEmpty())Add(Button(TEXT("View / edit original Classic UTL copy"),[this,Original](){PreviewLegacy(Original);if(LegacyDocument){LegacyName=TEXT("Loot_edited");LegacyRule.Reset();Page=TEXT("Legacy loot");Rebuild();}}));
    Slider(TEXT("loot_range"),TEXT("Corpse search range"),1,60,15,Accent,TEXT(" m"));
    Add(Toggle(TEXT("salvage_inventory"),TEXT("Apply salvage rules to existing inventory"),TEXT("While UCM runs, destroys matching items in all packs using the active loot profile and an Ust. First matching rule wins. Skips equipped, retained, traded, tinkered and inscribed items; Keep/Skip rules protect items. Off by default.")));
    Add(Toggle(TEXT("autostack"),TEXT("Auto-combine stackable items"),TEXT("While idle, merge partial stacks across all packs. Equipped, retained and traded items are skipped. Salvage bags have their own setting.")));
    Add(Toggle(TEXT("salvage_combine"),TEXT("Combine partial salvage bags"),TEXT("Uses imported workmanship groups and value thresholds; consumes matching bags with an Ust.")));
    Add(Toggle(TEXT("loot_only_rare"),TEXT("Only rare corpses"),TEXT("Appraises corpses and checks the server's rare-generation description.")));
    Add(Toggle(TEXT("loot_fellow_corpses"),TEXT("Loot fellowship corpses"),TEXT("Honors the killer's share-loot setting and waits for reserved corpses.")));
    Add(Toggle(TEXT("loot_all_corpses"),TEXT("Loot other corpses"),TEXT("Waits 100 seconds after seeing a corpse. Another player's rare corpse remains excluded.")));
    Add(Toggle(TEXT("read_unknown_scrolls"),TEXT("Learn unknown spells"),TEXT("Collect and read scrolls your current skills can learn, before applying loot rules. Known spells and carried duplicates follow normal rules.")));
    Add(Toggle(TEXT("autocram"),TEXT("Move loose items into packs"),TEXT("Keeps main-pack slots free using owned packs.")));

}
void LootProfileFiles()
{
    Add(Text(TEXT("Enter a new name to save and select a copy. Switching profiles stops UCM."),15,Muted));
    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).HintText(FText::FromString(TEXT("Profile name, e.g. Copper_W7")))
        .Text(FText::FromString(LootProfileName)).OnTextChanged_Lambda([this](const FText& V){LootProfileName=V.ToString();}));
    Add(Button(TEXT("Save loot profile"),[this](){if(Host->SaveLootProfile(LootProfileName))Rebuild();}));
}

void LootConditions()
{
    const auto& Calculated=ACEUCMLootPresentation::Calculated();
    Heading(TEXT("Requirements"),TEXT("Basic item filters always apply."));
    LootChoice(TEXT("conditions_mode"),TEXT("Combine requirements"),{{TEXT("all"),TEXT("AND - all requirements")},{TEXT("any"),TEXT("OR - any requirement")}});
    const TArray<TSharedPtr<FJsonValue>>* Stored=nullptr;TArray<TSharedPtr<FJsonValue>> Conditions;
    if(LootDraft->TryGetArrayField(TEXT("conditions"),Stored))Conditions=*Stored;
    auto List=SNew(SVerticalBox);
    for(int I=0;I<Conditions.Num();++I)
    {
        auto C=Conditions[I]->AsObject();
        List->AddSlot().AutoHeight().Padding(0,0,0,2)[LootSelectRow(
            FString::Printf(TEXT("%d. "),I+1)+ACEUCMLootPresentation::ConditionSummary(C),
            ACEUCMLootPresentation::DataRequirement(C),ConditionIndex==I,
            [this,I](){SelectLootRequirement(I);})];
    }
    if(Conditions.IsEmpty())List->AddSlot().AutoHeight()[Text(TEXT("No requirements. Add one below, or set basic item filters."),14,Muted)];
    Add(SNew(SBox).HeightOverride(150)[SNew(SScrollBox)+SScrollBox::Slot()[List]]);
    auto Tools=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
    Tools->AddSlot()[LootTool(TEXT("New requirement"),[this](){if(!CanLeaveLootRequirement())return;ConditionDraft=MakeShared<FJsonObject>();ConditionDraft->SetStringField(TEXT("field"),TEXT("int"));ConditionDraft->SetStringField(TEXT("op"),TEXT("ge"));ConditionDraft->SetNumberField(TEXT("key"),19);ConditionIndex=INDEX_NONE;Rebuild();})];
    for(const auto& SkillProperty:TArray<TPair<int32,FString>>{{218103840,TEXT("Weapon skill")},{159,TEXT("Wield requirement skill")}})
        Tools->AddSlot()[LootTool(SkillProperty.Value,[this,Key=SkillProperty.Key]()
        {
            if(!CanLeaveLootRequirement())return;
            ConditionDraft=MakeShared<FJsonObject>();ConditionDraft->SetStringField(TEXT("field"),TEXT("int"));
            ConditionDraft->SetStringField(TEXT("op"),TEXT("eq"));ConditionDraft->SetNumberField(TEXT("key"),Key);
            ConditionDraft->SetNumberField(TEXT("value"),44);ConditionIndex=INDEX_NONE;Rebuild();
        })];
    Tools->AddSlot()[LootTool(TEXT("Weapon type"),[this]()
    {
        if(!CanLeaveLootRequirement())return;ConditionDraft=MakeShared<FJsonObject>();
        ConditionDraft->SetStringField(TEXT("field"),TEXT("int"));ConditionDraft->SetStringField(TEXT("op"),TEXT("in"));
        ConditionDraft->SetNumberField(TEXT("key"),353);ConditionDraft->SetArrayField(TEXT("values"),{MakeShared<FJsonValueNumber>(2)});
        ConditionIndex=INDEX_NONE;Rebuild();
    })];
    Tools->AddSlot()[SNew(SBox).IsEnabled(Conditions.IsValidIndex(ConditionIndex))[LootTool(TEXT("Clone"),[this,Conditions](){if(!Conditions.IsValidIndex(ConditionIndex))return;if(!CanLeaveLootRequirement())return;ConditionDraft=ACEUCMLootPresentation::Clone(Conditions[ConditionIndex]->AsObject());ConditionIndex=INDEX_NONE;Rebuild();})]];
    Tools->AddSlot()[SNew(SBox).IsEnabled(Conditions.IsValidIndex(ConditionIndex))[LootTool(TEXT("Delete"),[this,Conditions]()mutable{if(!CanLeaveLootRequirement())return;ConditionDraft.Reset();Conditions.RemoveAt(ConditionIndex);LootDraft->SetArrayField(TEXT("conditions"),Conditions);ConditionIndex=INDEX_NONE;Rebuild();})]];
    for(int Direction:{-1,1})Tools->AddSlot()[SNew(SBox).IsEnabled(Conditions.IsValidIndex(ConditionIndex)&&Conditions.IsValidIndex(ConditionIndex+Direction))
        [LootTool(Direction<0?TEXT("Up"):TEXT("Down"),[this,Conditions,Direction]()mutable{if(!CanLeaveLootRequirement())return;ConditionDraft.Reset();Conditions.Swap(ConditionIndex,ConditionIndex+Direction);ConditionIndex+=Direction;LootDraft->SetArrayField(TEXT("conditions"),Conditions);Rebuild();})]];
    Add(Tools);
    if(!ConditionDraft)return;
    Add(Text(ConditionIndex==INDEX_NONE?TEXT("New requirement"):FString::Printf(TEXT("Requirement %d"),ConditionIndex+1),17,Accent));
    Add(Text(TEXT("Requirement type"),14,Muted));
    const TArray<TPair<FString,FString>> Types={{TEXT("int"),TEXT("Integer property")},{TEXT("float"),TEXT("Decimal property")},{TEXT("string"),TEXT("Text / name")},{TEXT("spells"),TEXT("Spell matching")},{TEXT("legacy"),TEXT("Calculated / character")}};
    auto TypeMenu=SNew(SVerticalBox);FString CurrentType;
    for(const auto& Type:Types)
    {
        if(Str(ConditionDraft,TEXT("field"))==Type.Key)CurrentType=Type.Value;
        TypeMenu->AddSlot().AutoHeight()[MenuButton(Type.Value,[this,Field=Type.Key]()
        {
            FSlateApplication::Get().DismissAllMenus();if(Str(ConditionDraft,TEXT("field"))==Field)return;
            ConditionDraft=MakeShared<FJsonObject>();ConditionDraft->SetStringField(TEXT("field"),Field);ConditionDraft->SetStringField(TEXT("op"),TEXT("ge"));
            ConditionDraft->SetNumberField(TEXT("key"),Field==TEXT("string")?1:Field==TEXT("legacy")?1002:19);
            ConditionDraft->SetNumberField(TEXT("value"),Field==TEXT("spells")?1:0);
            if(Field==TEXT("string")){ConditionDraft->SetStringField(TEXT("match_mode"),TEXT("contains"));ConditionDraft->SetBoolField(TEXT("ignore_case"),true);}
            if(Field==TEXT("legacy"))ConditionDraft->SetArrayField(TEXT("args"),{MakeShared<FJsonValueNumber>(1)});Rebuild();
        })];
    }
    Add(SNew(SACEPluginComboButton).Tag(TEXT("UCMLootType")).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(CurrentType,15)].MenuContent()[TypeMenu]);
    const FString Field=Str(ConditionDraft,TEXT("field"));
    const int32 PropertyKey=int32(Num(ConditionDraft,TEXT("key")));
    const bool WeaponSkill=ACEUCMLootPresentation::IsWeaponSkillProperty(Field,PropertyKey);
    const FString ValueKind=ACEUCMLootPresentation::ValueKind(Field,PropertyKey);
    if(Field==TEXT("legacy"))
    {
        const int Key=int(Num(ConditionDraft,TEXT("key")));const auto* Definition=Calculated.Find(Key);
        auto Menu=SNew(SVerticalBox);TArray<int> Keys;Calculated.GetKeys(Keys);Keys.Sort();
        for(int Type:Keys){const auto Choice=Calculated[Type];Menu->AddSlot().AutoHeight()[MenuButton(Choice.Name,[this,Type,Choice](){ConditionDraft->SetNumberField(TEXT("key"),Type);TArray<TSharedPtr<FJsonValue>> Args;for(const auto& Label:Choice.Arguments)Args.Add(MakeShared<FJsonValueNumber>(0));ConditionDraft->SetArrayField(TEXT("args"),Args);FSlateApplication::Get().DismissAllMenus();Rebuild();})];}
        Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(Definition?Definition->Name:TEXT("Retired legacy condition"),16)].MenuContent()[SNew(SBox).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Menu]]]);
        if(Definition)for(int Index=0;Index<Definition->Arguments.Num();++Index)
        {
            const auto& Args=ConditionDraft->GetArrayField(TEXT("args"));Add(Text(Definition->Arguments[Index],15,Muted));
            const bool Property=(Key==2003||Key==2005)&&Index==1,Skill=(Key==1000&&Index==1)||(Key==1004&&Index==0),Class=Key==7&&Index==0;
            if(Property||Skill||Class)
            {
                Add(LootPropertyPicker(Class?TEXT("class"):Skill?TEXT("skill"):Key==2005?TEXT("float"):TEXT("int"),Args.IsValidIndex(Index)?int(Args[Index]->AsNumber()):0,[this,Index](int32 V)
                {auto Values=ConditionDraft->GetArrayField(TEXT("args"));while(Values.Num()<=Index)Values.Add(MakeShared<FJsonValueNumber>(0));Values[Index]=MakeShared<FJsonValueNumber>(V);ConditionDraft->SetArrayField(TEXT("args"),Values);Rebuild();}));
                continue;
            }
            Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MaxFractionalDigits(12).MinValue(TOptional<double>()).MaxValue(TOptional<double>()).MinSliderValue(TOptional<double>()).MaxSliderValue(TOptional<double>()).Value(Args.IsValidIndex(Index)?Args[Index]->AsNumber():0)
                .OnValueChanged_Lambda([this,Index](double V){auto Values=ConditionDraft->GetArrayField(TEXT("args"));while(Values.Num()<=Index)Values.Add(MakeShared<FJsonValueNumber>(0));Values[Index]=MakeShared<FJsonValueNumber>(V);ConditionDraft->SetArrayField(TEXT("args"),Values);}));
        }
        if(Key==15)
        {
            Add(Text(TEXT("Armor palette group"),14,Muted));auto Groups=SNew(SVerticalBox);
            TArray<FString> Names;ACEUCMLootPresentation::ColorGroups().GetKeys(Names);Names.Sort();
            for(const auto& Name:Names)Groups->AddSlot().AutoHeight()[MenuButton(Name,[this,Name]()
            {
                ConditionDraft->SetStringField(TEXT("group"),Name);TArray<TSharedPtr<FJsonValue>> Slots;
                for(int32 Slot:ACEUCMLootPresentation::ColorGroups()[Name])Slots.Add(MakeShared<FJsonValueNumber>(Slot));
                ConditionDraft->SetArrayField(TEXT("slots"),Slots);FSlateApplication::Get().DismissAllMenus();Rebuild();
            })];
            Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(Str(ConditionDraft,TEXT("group"),TEXT("Choose armor group")),15)]
                .MenuContent()[SNew(SBox).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Groups]]]);
        }
    }
    else if(Field!=TEXT("spells"))
    {
        Add(Text(TEXT("Property name / ID (custom server IDs are accepted)"),14,Muted));
        Add(LootPropertyPicker(Field,int(Num(ConditionDraft,TEXT("key"),19)),[this](int32 V){ConditionDraft->SetNumberField(TEXT("key"),V);Rebuild();}));
        if(WeaponSkill)Add(Text(PropertyKey==159
            ?TEXT("WieldReqAttribute is the requirement's skill ID when WieldReqType is Skill (1), Raw Skill (2), or Training (8). Use EquipSkill to match the weapon's attack skill instead.")
            :PropertyKey==218103840?TEXT("EquipSkill is the weapon's attack skill from appraisal (VT). It can differ from the skill required to wield the item.")
            :TEXT("WeaponSkill is the raw item property. For VT rules, use EquipSkill from the weapon appraisal profile."),13,Muted));
        if(Field==TEXT("float"))Add(Text(TEXT("Values are stored as decimals: a +15% defense bonus is 1.15; a +130% bow damage modifier is 2.3. Workmanship can be fractional."),13,Muted));
    }
    if(Field==TEXT("string")||Field==TEXT("spells"))
    {
        if(Field==TEXT("string"))
        {
            Add(Text(TEXT("Text comparison"),14,Muted));auto Modes=SNew(SVerticalBox);
            const TArray<TPair<FString,FString>> Choices={{TEXT("contains"),TEXT("Contains text")},{TEXT("exact"),TEXT("Exact text")},{TEXT("prefix"),TEXT("Starts with text")},
                {TEXT("any"),TEXT("OR - any text separated by |")},{TEXT("all"),TEXT("AND - all text separated by |")},{TEXT("regex"),TEXT("Regular expression (VT)")}};
            FString Current;
            for(const auto& Choice:Choices)
            {
                if(Str(ConditionDraft,TEXT("match_mode"),TEXT("regex"))==Choice.Key)Current=Choice.Value;
                Modes->AddSlot().AutoHeight()[MenuButton(Choice.Value,[this,Choice](){ConditionDraft->SetStringField(TEXT("match_mode"),Choice.Key);FSlateApplication::Get().DismissAllMenus();Rebuild();})];
            }
            Add(SNew(SACEPluginComboButton).Tag(TEXT("UCMLootTextMode")).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(Current,15)].MenuContent()[Modes]);
            Add(SNew(SCheckBox).Tag(TEXT("UCMLootIgnoreCase")).Style(&FACEPluginStyle::Current()->Check)
                .IsChecked(Bool(ConditionDraft,TEXT("ignore_case"),Str(ConditionDraft,TEXT("match_mode"),TEXT("regex"))!=TEXT("regex"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State){ConditionDraft->SetBoolField(TEXT("ignore_case"),State==ECheckBoxState::Checked);})[Text(TEXT("Ignore letter case"),14)]);
            Add(Text(TEXT("Example: sword|unarmed with OR matches either word. Weapon classifications use Integer property: WeaponMasteryCategory (353), not a string property."),13,Muted));
        }
        Add(Text(TEXT("Match text / alternatives"),15));
        Add(SNew(SACEPluginEditableTextBox).Tag(TEXT("UCMLootPattern")).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Str(ConditionDraft,TEXT("pattern")))).OnTextChanged_Lambda([this](const FText& V){ConditionDraft->SetStringField(TEXT("pattern"),V.ToString());}));
        if(Field==TEXT("spells"))
        {
            Add(Text(TEXT("Exclude matching spell names (blank = none)"),15));
            Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Str(ConditionDraft,TEXT("exclude")))).OnTextChanged_Lambda([this](const FText& V){ConditionDraft->SetStringField(TEXT("exclude"),V.ToString());}));
            Add(Text(TEXT("Minimum matching spells"),15));
            Add(SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(1024).Value(int(Num(ConditionDraft,TEXT("value"),1))).OnValueChanged_Lambda([this](int V){ConditionDraft->SetNumberField(TEXT("value"),V);}));
            if(!ConditionDraft->HasField(TEXT("value")))ConditionDraft->SetNumberField(TEXT("value"),1);
        }
    }
    else if(Field!=TEXT("legacy"))
    {
        Add(Text(TEXT("Comparison"),14,Muted));
        const TArray<TPair<FString,FString>> Comparisons={{TEXT("ge"),TEXT(">= At least")},{TEXT("le"),TEXT("<= At most")},{TEXT("eq"),TEXT("= Equal to")},{TEXT("ne"),TEXT("!= Not equal to")},{TEXT("bits"),TEXT("Has any matching flag")},{TEXT("in"),TEXT("OR - any of these values")}};
        auto Ops=SNew(SVerticalBox);FString CurrentOp;
        for(const auto& Op:Comparisons)
        {
            if(Str(ConditionDraft,TEXT("op"))==Op.Key)CurrentOp=Op.Value;
            Ops->AddSlot().AutoHeight()[MenuButton(Op.Value,[this,Op](){ConditionDraft->SetStringField(TEXT("op"),Op.Key);FSlateApplication::Get().DismissAllMenus();Rebuild();})];
        }
        Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(CurrentOp,15)].MenuContent()[Ops]);
        const double Value=Num(ConditionDraft,TEXT("value"));
        if(Str(ConditionDraft,TEXT("op"))==TEXT("in"))
        {
            if(!ConditionDraft->HasField(TEXT("values")))ConditionDraft->SetArrayField(TEXT("values"),{MakeShared<FJsonValueNumber>(Value)});
            if(!ValueKind.IsEmpty())for(const auto& Entry:ACEUCMLootPresentation::Properties(ValueKind))
            {
                const bool Selected=ConditionDraft->GetArrayField(TEXT("values")).ContainsByPredicate([&](const auto& V){return V->AsNumber()==Entry.Id;});
                Add(SNew(SCheckBox).Tag(FName(*FString::Printf(TEXT("UCMLootValue.%d"),Entry.Id))).Style(&FACEPluginStyle::Current()->Check).IsChecked(Selected?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this,Id=Entry.Id](ECheckBoxState State){auto Values=ConditionDraft->GetArrayField(TEXT("values"));Values.RemoveAll([Id](const auto& V){return V->AsNumber()==Id;});if(State==ECheckBoxState::Checked)Values.Add(MakeShared<FJsonValueNumber>(Id));ConditionDraft->SetArrayField(TEXT("values"),Values);ConditionDraft->RemoveField(TEXT("invalid_values"));Rebuild();})[Text(Entry.Name,14)]);
            }
            FString Values;for(const auto& V:ConditionDraft->GetArrayField(TEXT("values"))){if(!Values.IsEmpty())Values+=TEXT("|");Values+=FString::Printf(TEXT("%.15g"),V->AsNumber());}
            if(ConditionDraft->HasField(TEXT("invalid_values")))Values=Str(ConditionDraft,TEXT("invalid_values"));
            Add(Text(TEXT("Values / custom IDs separated by |"),14,Muted));
            Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Values)).OnTextChanged_Lambda([this](const FText& Text)
            {
                TArray<FString> Parts;Text.ToString().ParseIntoArray(Parts,TEXT("|"),false);TArray<TSharedPtr<FJsonValue>> Values;
                for(FString Part:Parts){Part.TrimStartAndEndInline();double V=0;if(!LexTryParseString(V,*Part)||!FMath::IsFinite(V)){ConditionDraft->SetStringField(TEXT("invalid_values"),Text.ToString());return;}Values.Add(MakeShared<FJsonValueNumber>(V));}
                ConditionDraft->RemoveField(TEXT("invalid_values"));ConditionDraft->SetArrayField(TEXT("values"),Values);
            }));
        }
        else if(!ValueKind.IsEmpty()&&Value>=0&&Value<=MAX_int32&&Value==FMath::FloorToDouble(Value))
        {
            Add(Text(ValueKind==TEXT("weapon_type")?TEXT("Weapon type / ID (custom server IDs are accepted)"):TEXT("Skill name / ID (custom server IDs are accepted)"),14,Muted));
            Add(LootPropertyPicker(ValueKind,int32(Value),[this](int32 V){ConditionDraft->SetNumberField(TEXT("value"),V);Rebuild();}));
        }
        else
        {
            Add(Text(TEXT("Value"),14,Muted));
            Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MaxFractionalDigits(12).MinValue(TOptional<double>()).MaxValue(TOptional<double>()).MinSliderValue(TOptional<double>()).MaxSliderValue(TOptional<double>()).Value(Value).OnValueChanged_Lambda([this](double V){ConditionDraft->SetNumberField(TEXT("value"),V);}));
        }
        Add(Button(Bool(ConditionDraft,TEXT("missing_zero"))?TEXT("Missing value: use 0 (VT)"):TEXT("Missing value: does not match"),[this](){ConditionDraft->SetBoolField(TEXT("missing_zero"),!Bool(ConditionDraft,TEXT("missing_zero")));Rebuild();}));
    }
    Add(Button(TEXT("Apply requirement"),[this,Conditions]()mutable
    {
        if(!ACEUCMLootPresentation::ValidTextMatch(ConditionDraft)){Host->Notice=TEXT("Invalid text match or regular expression (maximum 2,048 characters and 32 capture groups).");return;}
        if(ConditionDraft->HasField(TEXT("invalid_values"))||(Str(ConditionDraft,TEXT("op"))==TEXT("in")&&ConditionDraft->GetArrayField(TEXT("values")).IsEmpty())){Host->Notice=TEXT("Enter at least one valid value, separated by |.");return;}
        const int Key=int(Num(ConditionDraft,TEXT("key")));
        if(Str(ConditionDraft,TEXT("field"))==TEXT("legacy")&&Key>=14&&Key<=16&&!ACEUCMLootPresentation::ValidColorMatch(ConditionDraft)){Host->Notice=TEXT("Use RGB values 0-255, hue tolerance 0-360, saturation/value tolerance 0-2, and select an armor group or palette slot.");return;}
        if(ConditionIndex==INDEX_NONE){if(Conditions.Num()>=ACEVTProfile::MaxLootConditions){Host->Notice=FString::Printf(TEXT("Maximum %d conditions per rule."),ACEVTProfile::MaxLootConditions);return;}ConditionIndex=Conditions.Num();Conditions.Add(MakeShared<FJsonValueObject>(ConditionDraft));}
        else if(Conditions.IsValidIndex(ConditionIndex))Conditions[ConditionIndex]=MakeShared<FJsonValueObject>(ConditionDraft);
        LootDraft->SetArrayField(TEXT("conditions"),Conditions);ConditionDraft.Reset();Rebuild();
    }));
    Add(Button(TEXT("Discard requirement changes"),[this](){ConditionDraft.Reset();Rebuild();}));
}

void SalvagePanel()
{
    Heading(TEXT("Salvage combining"),TEXT("Bags combine only with the same material and workmanship group. Material overrides replace the default groups. A value target delays full bags until their combined value reaches the target; zero uses workmanship only. Saving stops UCM."));
    const TSharedPtr<FJsonObject>* Stored=nullptr;
    TSharedPtr<FJsonObject> Policy=P()->TryGetObjectField(TEXT("salvage_policy"),Stored)?*Stored:nullptr;
    if(!SalvageDraft)
    {
        SalvageDraft=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Ranges;
        const TArray<TSharedPtr<FJsonValue>>* Source=nullptr;
        if(SalvageMaterial&&Policy&&Policy->HasTypedField<EJson::Object>(TEXT("materials")))Policy->GetObjectField(TEXT("materials"))->TryGetArrayField(FString::FromInt(SalvageMaterial),Source);
        if(!Source&&Policy)Policy->TryGetArrayField(TEXT("default"),Source);
        if(Source)for(const auto& V:*Source){auto Copy=MakeShared<FJsonObject>();Copy->Values=V->AsObject()->Values;Ranges.Add(MakeShared<FJsonValueObject>(Copy));}
        else for(const auto& Pair:TArray<TPair<double,double>>{{1,6},{7,8},{9,9},{10,10}}){auto R=MakeShared<FJsonObject>();R->SetNumberField(TEXT("min"),Pair.Key);R->SetNumberField(TEXT("max"),Pair.Value);Ranges.Add(MakeShared<FJsonValueObject>(R));}
        SalvageDraft->SetArrayField(TEXT("ranges"),Ranges);
        double Target=0;if(SalvageMaterial&&Policy&&Policy->HasTypedField<EJson::Object>(TEXT("values")))Policy->GetObjectField(TEXT("values"))->TryGetNumberField(FString::FromInt(SalvageMaterial),Target);
        SalvageDraft->SetNumberField(TEXT("target"),Target);
    }
    auto Menu=SNew(SVerticalBox);
    for(int32 Id=0;Id<=77;++Id)Menu->AddSlot().AutoHeight()[MenuButton(Id?ACERetailObjectNames::GetMaterialTypeName(Id):TEXT("Default groups"),[this,Id](){SalvageMaterial=Id;SalvageDraft.Reset();FSlateApplication::Get().DismissAllMenus();Rebuild();})];
    Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text(SalvageMaterial?ACERetailObjectNames::GetMaterialTypeName(SalvageMaterial):TEXT("Default groups"),16)]
        .MenuContent()[SNew(SBox).MaxDesiredHeight(320)[SNew(SScrollBox)+SScrollBox::Slot()[Menu]]]);
    Add(Text(TEXT("Workmanship groups: minimum / maximum. Classic assigns gaps to the preceding group: 1-6 includes 6.99 until the next group starts at 7. Materials never mix."),16,Muted));
    const auto Ranges=SalvageDraft->GetArrayField(TEXT("ranges"));
    for(int32 Index=0;Index<Ranges.Num();++Index)
    {
        auto Row=SNew(SHorizontalBox);auto Range=Ranges[Index]->AsObject();
        for(const FString Key:{TEXT("min"),TEXT("max")})Row->AddSlot().FillWidth(1).Padding(0,0,8,0)[SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",16)).MinValue(0).MaxValue(1000).Value(Num(Range,*Key))
            .OnValueChanged_Lambda([Range,Key](double V){Range->SetNumberField(Key,V);})];
        Row->AddSlot().AutoWidth()[Button(TEXT("Remove"),[this,Index](){auto R=SalvageDraft->GetArrayField(TEXT("ranges"));R.RemoveAt(Index);SalvageDraft->SetArrayField(TEXT("ranges"),R);Rebuild();})];Add(Row);
    }
    Add(Button(TEXT("+ Workmanship group"),[this](){auto R=SalvageDraft->GetArrayField(TEXT("ranges"));if(R.Num()>=64)return;auto Next=MakeShared<FJsonObject>();const double Start=R.Num()?Num(R.Last()->AsObject(),TEXT("max"))+1:1;Next->SetNumberField(TEXT("min"),Start);Next->SetNumberField(TEXT("max"),Start);R.Add(MakeShared<FJsonValueObject>(Next));SalvageDraft->SetArrayField(TEXT("ranges"),R);Rebuild();}));
    if(SalvageMaterial)
    {
        Add(Text(TEXT("Minimum combined value for a full bag (pyreals; 0 = disabled)"),15,Muted));
        Add(SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",16)).MinValue(0).MaxValue(MAX_int32).Value(int32(Num(SalvageDraft,TEXT("target"))))
            .OnValueChanged_Lambda([this](int32 V){SalvageDraft->SetNumberField(TEXT("target"),V);}));
    }
    auto Save=[this,Policy](bool Remove)
    {
        auto Updated=MakeShared<FJsonObject>();Updated->Values=P()->Values;
        auto Copy=MakeShared<FJsonObject>();if(Policy)Copy->Values=Policy->Values;
        if(!Copy->HasField(TEXT("default"))){TArray<TSharedPtr<FJsonValue>> Defaults;for(const auto& Pair:TArray<TPair<double,double>>{{1,6},{7,8},{9,9},{10,10}}){auto R=MakeShared<FJsonObject>();R->SetNumberField(TEXT("min"),Pair.Key);R->SetNumberField(TEXT("max"),Pair.Value);Defaults.Add(MakeShared<FJsonValueObject>(R));}Copy->SetArrayField(TEXT("default"),Defaults);}
        if(SalvageMaterial)
        {
            auto Materials=MakeShared<FJsonObject>(),Values=MakeShared<FJsonObject>();
            if(Copy->HasTypedField<EJson::Object>(TEXT("materials")))Materials->Values=Copy->GetObjectField(TEXT("materials"))->Values;
            if(Copy->HasTypedField<EJson::Object>(TEXT("values")))Values->Values=Copy->GetObjectField(TEXT("values"))->Values;
            const FString Key=FString::FromInt(SalvageMaterial);
            if(Remove){Materials->RemoveField(Key);Values->RemoveField(Key);}
            else{Materials->SetArrayField(Key,SalvageDraft->GetArrayField(TEXT("ranges")));Values->SetNumberField(Key,Num(SalvageDraft,TEXT("target")));}
            Copy->SetObjectField(TEXT("materials"),Materials);Copy->SetObjectField(TEXT("values"),Values);
        }
        else Copy->SetArrayField(TEXT("default"),SalvageDraft->GetArrayField(TEXT("ranges")));
        Updated->SetObjectField(TEXT("salvage_policy"),Copy);FString Json;FJsonSerializer::Serialize(Updated,TJsonWriterFactory<>::Create(&Json));
        if(Host->SaveProfile(Plugin->Id,Plugin->ProfileName,Json)){SalvageDraft.Reset();Rebuild();}
        else Host->Notice=TEXT("Groups must have ascending, non-overlapping ranges with minimum no greater than maximum.");
    };
    Add(Button(TEXT("Save groups"),[Save](){Save(false);}));
    if(SalvageMaterial)Add(Button(TEXT("Use default groups for this material"),[Save](){Save(true);}));
    Add(Button(TEXT("Back to loot rules"),[this](){EditingSalvage=false;SalvageDraft.Reset();Rebuild();}));
}
