#include "Mods/ACEPluginStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEItemTools.h"
#include "ACESession.h"
#include "UI/ACERetailObjectNames.h"
#include "Engine/GameInstance.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace ACEItemToolsPanel
{
using namespace ACEItemTools;
const FLinearColor Background(.025f,.016f,.008f),Card(.012f,.009f,.004f),Ink(1.f,.98f,.92f),Muted(.86f,.83f,.75f),Accent(1,.76f,.25f);
const FButtonStyle& ButtonStyle() {return FACEPluginStyle::Current()->Button;}
TSharedRef<STextBlock> Text(const FString& Value,int32 Size=14,FLinearColor Color=Ink)
{return SNew(SACEPluginText).Text(FText::FromString(Value)).Font(FACEPluginStyle::NativeFont("Regular",Size)).ColorAndOpacity(Color).AutoWrapText(true);}
TSharedRef<SWidget> Button(const FString& Label,TFunction<void()> Action)
{return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&ButtonStyle()).IsFocusable(false).OnClicked_Lambda([Action](){Action();return FReply::Handled();})[Text(Label)];}
TSharedRef<SWidget> ItemMenuButton(const FString& Label,TFunction<void()> Action)
{return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->MenuItem).IsFocusable(false)
    .OnClicked_Lambda([Action](){Action();FSlateApplication::Get().DismissAllMenus();return FReply::Handled();})[Text(Label)];}

class SItemToolsPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SItemToolsPanel):_ShowTitle(true){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(FString,Page) SLATE_ARGUMENT(bool,ShowTitle) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Host=Args._Host;FACEPluginStyle::Use(Args._Host,TEXT("itemtools"));Page=Args._Page.IsEmpty()?TEXT("Give"):Args._Page;
        for(const auto& P:Host->Plugins)if(P->Id==TEXT("itemtools"))Plugin=P;
        if(!Plugin)return;
        auto Tabs=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
        for(const TCHAR* Name:{TEXT("Give"),TEXT("Buy / Sell"),TEXT("Profiles"),TEXT("Chests"),TEXT("Restock"),TEXT("Merge")})
            Tabs->AddSlot()[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(TEXT("ItemToolsTab")).Text(FText::FromString(Name))
                .Selected_Lambda([this,Name](){return Page==Name;})
                .OnClicked_Lambda([this,Name](){Page=Name;Rebuild();Scroll->ScrollToStart();return FReply::Handled();})];
        ChildSlot[SNew(SBorder).BorderImage(&FACEPluginStyle::Current()->Background).Padding(16)
            [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(Args._ShowTitle?EVisibility::Visible:EVisibility::Collapsed)[Text(TEXT("Item Tools"),26)]]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,4)[Text(TEXT("Give, trade, restock and merge across all your packs."),14,Muted)]]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).IsEnabled_Lambda([this](){return Plugin->Running;})
                    [Button(TEXT("Stop"),[this](){Host->Stop(Plugin->Id,TEXT("Stopped by player"));})]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(Accent)
                .Font(FACEPluginStyle::NativeFont("Regular",14)).Text_Lambda([this](){
                    FString Progress;
                    if(Plugin->Running&&Host->ItemToolsProgress)
                    {
                        const FString Mode=String(Plugin->Profile,TEXT("mode"));
                        if(Mode==TEXT("give")||Mode==TEXT("profile"))
                        {
                            const double Done=Number(Host->ItemToolsProgress,TEXT("completed")),Total=Number(Host->ItemToolsProgress,TEXT("total"));
                            Progress=Total>0?FString::Printf(TEXT("%.0f / %.0f items given"),Done,Total):FString::Printf(TEXT("%.0f items given"),Done);
                        }
                        else Progress=FString::Printf(TEXT("%.0f confirmed actions"),Number(Host->ItemToolsProgress,TEXT("operations")));
                    }
                    return FText::FromString(Progress.IsEmpty()?Plugin->Status:Progress+TEXT("\n")+Plugin->Status);
                })]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Tabs]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(Scroll,SScrollBox)+SScrollBox::Slot()
                [SAssignNew(Body,SVerticalBox).IsEnabled_Lambda([this](){return !Plugin->Running;})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(Muted)
                .Visibility_Lambda([this](){return Host.IsValid()&&!Host->Notice.IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})
                .Font(FACEPluginStyle::NativeFont("Regular",12)).Text_Lambda([this](){return FText::FromString(Host.IsValid()?Host->Notice:FString());})]]];
        Rebuild();
    }
private:
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<FACEClientPlugin> Plugin;
    TSharedPtr<SVerticalBox> Body;
    TSharedPtr<SScrollBox> Scroll;
    FString Page,CharacterProfile;
    TMap<FString,int32> GiveCounts;TMap<uint32,int32> Owned;TSet<uint32> Stocked;double NextCounts=0;
    TSharedPtr<FJsonObject> P()const{return Plugin->Profile;}
    UACEClientSubsystem* Client()const{return Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();}
    void Save(){Host->SaveProfile(Plugin->Id,Plugin->ProfileName,Host->ProfileJson(Plugin->Id),false);}
    void Add(TSharedRef<SWidget> Widget){Body->AddSlot().AutoHeight().Padding(0,0,0,12)[Widget];}
    void Heading(const FString& Title,const FString& Help){Add(Text(Title,20));Add(Text(Help,14,Muted));}
    TSharedRef<SWidget> Tile(TSharedRef<SWidget> Widget)
    {return SNew(SBorder).BorderImage(&FACEPluginStyle::Current()->SectionFrame).Padding(10)[Widget];}
    void Pick(const FString& Key,const FString& Kind,const FString& Label)
    {
        auto Row=SNew(SVerticalBox);
        Row->AddSlot().AutoHeight()[Text(Label,14,Muted)];
        Row->AddSlot().AutoHeight().Padding(0,5)[SNew(SACEPluginText).AutoWrapText(true).Font(FACEPluginStyle::NativeFont("Regular",17))
            .Text_Lambda([this,Key](){FString Name=String(P(),*(Key+TEXT("_name")));if(Name.IsEmpty())Name=TEXT("Nothing selected");
                else if((Key==TEXT("target")||Key==TEXT("chest")||Key.StartsWith(TEXT("mule")))&&!Guid(P(),*(Key+TEXT("_id"))))Name+=TEXT(" - select again this login");return FText::FromString(Name);})];
        auto Buttons=SNew(SHorizontalBox);
        Buttons->AddSlot().AutoWidth()[Button(TEXT("Use selected"),[this,Key,Kind](){Host->SelectItemToolsObject(Key,Kind);})];
        Buttons->AddSlot().AutoWidth().Padding(6,0)[Button(TEXT("Clear"),[this,Key](){for(const TCHAR* Suffix:{TEXT("_id"),TEXT("_name"),TEXT("_wcid")})P()->RemoveField(*(Key+Suffix));Save();})];
        Row->AddSlot().AutoHeight()[Buttons];Add(Tile(Row));
    }
    void Quantity(const FString& Key,const FString& Caption,int32 Default=0,int32 Max=100000)
    {
        Add(Tile(SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Text(Caption)]
            +SHorizontalBox::Slot().AutoWidth().Padding(10,0)[SNew(SBox).WidthOverride(112)[SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(Max).MinDesiredWidth(104).Font(FACEPluginStyle::NativeFont("Regular",14))
                .Value_Lambda([this,Key,Default](){return int32(Number(P(),*Key,Default));})
                .OnValueChanged_Lambda([this,Key](int32 Value){P()->SetNumberField(*Key,Value);})
                .OnEndSliderMovement_Lambda([this](int32){Save();})
                .OnValueCommitted_Lambda([this,Key](int32 Value,ETextCommit::Type){P()->SetNumberField(*Key,Value);Save();})]]));
    }
    void Toggle(const FString& Key,const FString& Label,bool Default=false)
    {
        Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked_Lambda([this,Key,Default](){bool V=Default;P()->TryGetBoolField(Key,V);return V?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this,Key](ECheckBoxState S){P()->SetBoolField(Key,S==ECheckBoxState::Checked);Save();})[SNew(SACEPluginText).Text(FText::FromString(Label)).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Ink).WrapTextAt_Lambda([this](){return FMath::Max(180.f,GetCachedGeometry().GetLocalSize().X-65.f);})]);
    }
    void Start(const FString& Mode,const FString& Label)
    {
        Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked_Lambda([this](){return Plugin->Enabled?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this](ECheckBoxState S){Host->SetEnabled(Plugin->Id,S==ECheckBoxState::Checked);})[Text(TEXT("Enable Item Tools inventory actions"))]);
        Add(SNew(SBox).IsEnabled_Lambda([this](){return Plugin->Enabled;})[Button(Label,[this,Mode](){Host->StartItemTools(Mode);})]);
        Add(Text(TEXT("Waits for each transfer to be confirmed. UCM pauses during this job and resumes afterwards."),12,Muted));
    }
    void Rules(const FString& Key,const FString& Label)
    {
        auto Content=SNew(SVerticalBox);
        Content->AddSlot().AutoHeight()[Text(Label,14,Muted)];
        Content->AddSlot().AutoHeight().Padding(0,6)[SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
            .OnGetMenuContent_Lambda([this,Key]()->TSharedRef<SWidget>{auto List=SNew(SVerticalBox);
                List->AddSlot().AutoHeight()[ItemMenuButton(TEXT("None"),[this,Key](){Host->LoadItemToolsRules(Key,FString());})];
                for(const auto& File:Host->GetProfileFiles(TEXT("utl")))List->AddSlot().AutoHeight()[ItemMenuButton(File,[this,Key,File](){Host->LoadItemToolsRules(Key,File);})];
                return SNew(SBox).MaxDesiredHeight(300).MinDesiredWidth(240)[SNew(SScrollBox)+SScrollBox::Slot()[List]];})
            .ButtonContent()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).AutoWrapText(true).Text_Lambda([this,Key](){const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
                const int32 N=P()->TryGetArrayField(Key+TEXT("_rules"),Values)?Values->Num():0;FString Name=String(P(),*(Key+TEXT("_file")));
                return FText::FromString(N?FString::Printf(TEXT("%s  (%d rules)"),*Name,N):TEXT("None - choose loot profile"));})]];
        Content->AddSlot().AutoHeight()[Button(TEXT("Copy UCM's active rules"),[this,Key](){for(const auto& U:Host->Plugins)if(U->Id==TEXT("ucm"))
            {const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;if(U->Profile->TryGetArrayField(TEXT("loot_rules"),Values)){P()->SetArrayField(Key+TEXT("_rules"),*Values);P()->SetStringField(Key+TEXT("_file"),TEXT("Copy of UCM rules"));Save();}}})];
        Add(Tile(Content));
    }
    void Library()
    {
        Add(Text(TEXT("Loot profile folder"),14,Muted));
        Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Host->GetProfileFolder())).OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type){Host->SetProfileFolder(Value.ToString());}));
        Add(Button(TEXT("Refresh profiles"),[this](){Rebuild();Host->Notice=TEXT("Profile list refreshed. Re-select a profile to apply edits from disk.");}));
        Add(Text(TEXT("Reads .utl files without changing the originals. First matching rule wins; only unlimited Keep items are given; Keep # limits are honored when collecting from chests. Use the Loot Editor to edit rules."),12,Muted));
    }
    void RestockList()
    {
        Add(Button(TEXT("Add selected item / vendor stock"),[this](){auto* C=Client();FACEWorldObject Item;const int32 Id=C->GetSelectedObject().Guid;
            bool Found=C->GetWorldObject(Id,Item)&&Transferable(C,Item);for(const auto& V:C->GetVendorMerchandise())if(V.Guid==Id){Item=V;Found=true;}
            if(!Found){Host->Notice=TEXT("Select an inventory item or vendor stock first.");return;}
            const TArray<TSharedPtr<FJsonValue>>* Old=nullptr;TArray<TSharedPtr<FJsonValue>> Rows;if(P()->TryGetArrayField(TEXT("restock"),Old))Rows=*Old;
            for(const auto& V:Rows)if(Number(V->AsObject(),TEXT("wcid"))==Item.WeenieClassId){Host->Notice=TEXT("That item is already in the restock list.");return;}
            auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));R->SetNumberField(TEXT("wcid"),Item.WeenieClassId);R->SetNumberField(TEXT("quantity"),100);
            Rows.Add(MakeShared<FJsonValueObject>(R));P()->SetArrayField(TEXT("restock"),Rows);Save();Rebuild();}));
        const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
        if(!P()->TryGetArrayField(TEXT("restock"),Rows)||Rows->IsEmpty()){Add(Text(TEXT("Your list is empty. Add a component, ammunition or another supply above."),14,Muted));return;}
        for(int32 Index=0;Index<Rows->Num();++Index)
        {
            auto R=(*Rows)[Index]->AsObject();auto Box=SNew(SVerticalBox);
            Box->AddSlot().AutoHeight()[Text(String(R,TEXT("name")),17)];
            Box->AddSlot().AutoHeight().Padding(0,5)[SNew(SACEPluginText).ColorAndOpacity(Muted).Text_Lambda([this,R](){const uint32 Wcid=uint32(Number(R,TEXT("wcid")));
                return FText::FromString(FString::Printf(TEXT("Owned: %d  |  %s"),Owned.FindRef(Wcid),Client()->GetOpenVendorGuid()?(Stocked.Contains(Wcid)?TEXT("Available at vendor"):TEXT("Not sold here")):TEXT("Open a vendor to check stock")));})];
            Box->AddSlot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Text(TEXT("Keep this many"))]
                +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SACEPluginSpinBox<int32>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(100000).MinDesiredWidth(100).Font(FACEPluginStyle::NativeFont("Regular",14)).Value_Lambda([this,Index](){return int32(Number(P()->GetArrayField(TEXT("restock"))[Index]->AsObject(),TEXT("quantity")));})
                    .OnValueChanged_Lambda([this,Index](int32 N){P()->GetArrayField(TEXT("restock"))[Index]->AsObject()->SetNumberField(TEXT("quantity"),N);})
                    .OnEndSliderMovement_Lambda([this](int32){Save();})
                    .OnValueCommitted_Lambda([this,Index](int32 N,ETextCommit::Type){P()->GetArrayField(TEXT("restock"))[Index]->AsObject()->SetNumberField(TEXT("quantity"),N);Save();})]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Remove"),[this,Index](){auto Values=P()->GetArrayField(TEXT("restock"));Values.RemoveAt(Index);P()->SetArrayField(TEXT("restock"),Values);Save();Rebuild();})]];
            Add(Tile(Box));
        }
    }
    virtual void Tick(const FGeometry& Geometry,double Now,float Delta) override
    {
        SCompoundWidget::Tick(Geometry,Now,Delta);
        if(Now<NextCounts)return;NextCounts=Now+.5;
        Owned.Reset();Stocked.Reset();GiveCounts.Reset();
        if(auto Session=Client()->GetSession())for(const auto& Pair:Session->GetWorldObjects())
            if(Client()->IsOwnedInventoryItem(Pair.Value))
            {
                Owned.FindOrAdd(Pair.Value.WeenieClassId)+=FMath::Max(1,Pair.Value.StackSize);
                FACEWorldObject Target;const bool IsPlayer=Client()->GetWorldObject(Guid(P(),TEXT("target_id")),Target)&&Target.bIsPlayer;
                if(Transferable(Client(),Pair.Value)&&(!IsPlayer||!Pair.Value.Attuned))GiveCounts.FindOrAdd(ACERetailObjectNames::Name(Pair.Value))+=FMath::Max(1,Pair.Value.StackSize);
            }
        for(const auto& I:Client()->GetVendorMerchandise())Stocked.Add(I.WeenieClassId);
        if(CharacterProfile!=Plugin->ProfileName){CharacterProfile=Plugin->ProfileName;Rebuild();}
    }
    void Rebuild()
    {
        if(!Host.IsValid())return;FACEPluginStyle::Use(Host.Get(),TEXT("itemtools"));
        Body->ClearChildren();
        if(Page==TEXT("Give"))
        {
            Heading(TEXT("Give matching items"),TEXT("Select a player or NPC in the world, then select an inventory item. All eligible items with that name are included across your packs."));
            Pick(TEXT("target"),TEXT("recipient"),TEXT("Recipient"));Pick(TEXT("give"),TEXT("item"),TEXT("Item to give"));
            Add(SNew(SACEPluginText).ColorAndOpacity(Accent).Text_Lambda([this](){return FText::FromString(FString::Printf(TEXT("Eligible units across all packs: %d"),GiveCounts.FindRef(String(P(),TEXT("give_name")))));}));
            Quantity(TEXT("give_limit"),TEXT("Maximum units to give (0 = all)"));Quantity(TEXT("delay_ms"),TEXT("Pause between transfers (milliseconds)"),250,5000);
            Add(Text(TEXT("Equipped, retained and offered items are excluded. Only empty containers can be given."),12,Muted));Start(TEXT("give"),TEXT("Start giving"));
        }
        else if(Page==TEXT("Merge"))
        {
            Heading(TEXT("Merge similar items"),TEXT("Combine matching partial stacks across your main backpack and all carried bags, up to each item's maximum stack size."));
            Add(Text(TEXT("Equipped, retained and offered items are excluded. Full stacks stay intact. Each merge waits for server confirmation; Stop cancels the remaining work."),14,Muted));
            Quantity(TEXT("merge_delay_ms"),TEXT("Pause between merges (milliseconds)"),0,5000);
            Start(TEXT("merge"),TEXT("Merge similar items"));
            Add(Text(TEXT("Chat command: /acueitems merge"),12,Muted));
        }
        else if(Page==TEXT("Buy / Sell"))
        {
            Heading(TEXT("Vendor exchange"),TEXT("Open a vendor and select its stock to buy. Choose an inventory item to sell for funding, or leave the buy item empty to sell only."));
            Pick(TEXT("buy"),TEXT("stock"),TEXT("Buy from vendor"));Pick(TEXT("sell"),TEXT("item"),TEXT("Sell / fund purchases with"));
            Quantity(TEXT("buy_limit"),TEXT("Units to buy (0 = until funds or space run out)"));Toggle(TEXT("redeem_notes"),TEXT("Redeem trade notes when more pyreals are needed"),true);
            Toggle(TEXT("server_bank"),TEXT("Use server bank for purchases (server validates funds; skips note sales)"));
            Start(TEXT("shop"),TEXT("Start vendor exchange"));
        }
        else if(Page==TEXT("Profiles"))
        {
            Heading(TEXT("Give by loot profile"),TEXT("Send inventory items that match Keep rules to a player or NPC. Rules marked Keep #, Ignore, Sell, Salvage or Read stay in your inventory, matching GiveVT."));
            Pick(TEXT("target"),TEXT("recipient"),TEXT("Recipient"));Rules(TEXT("profile"),TEXT("Transfer profile"));Start(TEXT("profile"),TEXT("Give profile matches"));Library();
        }
        else if(Page==TEXT("Chests"))
        {
            Heading(TEXT("Open, collect and distribute"),TEXT("Use a key, collect Keep matches, then give new loot to up to two recipients using their own profiles. Clear the key for looter-only; clear the collection profile for opener-only."));
            Pick(TEXT("chest"),TEXT("chest"),TEXT("Chest"));Pick(TEXT("key"),TEXT("item"),TEXT("Key (optional)"));Rules(TEXT("chest"),TEXT("Collection profile"));
            Pick(TEXT("mule1"),TEXT("recipient"),TEXT("First recipient (optional)"));Rules(TEXT("mule1"),TEXT("First recipient's profile"));
            Pick(TEXT("mule2"),TEXT("recipient"),TEXT("Second recipient (optional)"));Rules(TEXT("mule2"),TEXT("Second recipient's profile"));
            Quantity(TEXT("chest_cycles"),TEXT("Chest cycles (0 = repeat)"),1,10000);Quantity(TEXT("chest_delay"),TEXT("Wait between cycles (seconds)"),3,600);
            Start(TEXT("chest"),TEXT("Start chest job"));Library();
        }
        else
        {
            Heading(TEXT("Character supplies"),TEXT("Set the total quantity to keep across all packs. Buys only the shortage from the open vendor. This list is saved separately for each character and server."));
            Toggle(TEXT("auto_restock"),TEXT("Restock automatically when a matching vendor opens"));Toggle(TEXT("redeem_notes"),TEXT("Redeem trade notes to fund restocking"),true);
            Toggle(TEXT("server_bank"),TEXT("Use server bank for purchases (server validates funds; skips note sales)"));
            RestockList();Start(TEXT("restock"),TEXT("Buy missing supplies"));Add(Text(TEXT("Chat command: /acueitems restock"),12,Muted));
        }
    }
};
}

TSharedRef<SWidget> UACEPluginSubsystem::MakeItemToolsPanel(bool bShowTitle)
{
    UpdateItemToolsCharacter();return SNew(ACEItemToolsPanel::SItemToolsPanel).Host(this).ShowTitle(bShowTitle);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEItemToolsPanelTest,"ACE.Plugins.ItemTools.Panel",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEItemToolsPanelTest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();TSharedPtr<FACEClientPlugin> P;
    for(const auto& Plugin:H->Plugins)if(Plugin->Id==TEXT("itemtools"))P=Plugin;
    if(!TestTrue(TEXT("Item Tools is discovered as a standalone plugin"),P.IsValid()))return false;
    P->Profile=MakeShared<FJsonObject>();P->Profile->SetStringField(TEXT("give_name"),TEXT("Prismatic Taper"));P->Profile->SetStringField(TEXT("target_name"),TEXT("Quartermaster"));
    auto Supply=MakeShared<FJsonObject>();Supply->SetStringField(TEXT("name"),TEXT("Prismatic Taper"));Supply->SetNumberField(TEXT("wcid"),100);Supply->SetNumberField(TEXT("quantity"),500);
    P->Profile->SetArrayField(TEXT("restock"),{MakeShared<FJsonValueObject>(Supply)});
    if(FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(false,true);
        const FString Folder=FPaths::ProjectSavedDir()/TEXT("ItemToolsReview/UI");IFileManager::Get().MakeDirectory(*Folder,true);
        for(const TCHAR* Page:{TEXT("Give"),TEXT("Buy / Sell"),TEXT("Profiles"),TEXT("Chests"),TEXT("Restock"),TEXT("Merge")})for(int Width:{780,460})
        {
            auto Panel=SNew(ACEItemToolsPanel::SItemToolsPanel).Host(H).Page(Page);
            const FVector2D Size(Width,900);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            TestEqual(TEXT("Each activity renders at desktop and narrow width"),Pixels.Num(),Width*900);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Width,900,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(Folder/FString::Printf(TEXT("%s-%d.png"),*FString(Page).Replace(TEXT(" / "),TEXT("-")),Width)));Target->ReleaseResource();
        }
    }
    return true;
}
#endif
