#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACERetailTextEntry.h"
#include "ACEClientSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"

namespace
{
FString JournalTimerText(double End)
{
 if(End<=0)return TEXT("None");
 int64 Seconds=static_cast<int64>(End-FDateTime::UtcNow().ToUnixTimestamp());
 if(Seconds<=0)return TEXT("Ready");
 // ClientUISystem::DeltaTimeToString uses 30-day months and omits zero units.
 FString Text;
 const int64 Units[]={2592000,86400,3600,60};
 const TCHAR* Suffixes[]={TEXT("mo"),TEXT("d"),TEXT("h"),TEXT("m")};
 for(int32 I=0;I<4;++I)
 {
  const int64 Count=Seconds/Units[I];Seconds%=Units[I];
  if(Count)Text+=FString::Printf(TEXT("%lld%s "),Count,Suffixes[I]);
 }
 return Text+FString::Printf(TEXT("%llds"),Seconds);
}
}

// Retail gmJournalUI stores personal pages on the client, separate from server contracts.
void UACEUIGameplayBinder::SaveJournal()
{
 if (JournalFile.IsEmpty()) return;
 TArray<TSharedPtr<FJsonValue>> Pages;
 for (const auto& P : JournalPages)
 {
  auto O=MakeShared<FJsonObject>();
  O->SetStringField(TEXT("label"),P.Label); O->SetStringField(TEXT("title"),P.Title);
  O->SetStringField(TEXT("notes"),P.Notes); O->SetStringField(TEXT("location"),P.Location);
  O->SetNumberField(TEXT("days"),P.Days); O->SetNumberField(TEXT("hours"),P.Hours);
  O->SetNumberField(TEXT("minutes"),P.Minutes); O->SetNumberField(TEXT("timerEnd"),P.TimerEnd);
  Pages.Add(MakeShared<FJsonValueObject>(O));
 }
 auto Root=MakeShared<FJsonObject>(); Root->SetArrayField(TEXT("pages"),Pages);
 FString Text; FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text));
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(JournalFile),true);
 const FString Temporary=JournalFile+TEXT(".tmp");
 if (!FFileHelper::SaveStringToFile(Text,*Temporary,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
  || !IFileManager::Get().Move(*JournalFile,*Temporary,true,true))
  UE_LOG(LogTemp,Warning,TEXT("ACE: Unable to save journal %s"),*JournalFile);
}

void UACEUIGameplayBinder::HandleJournalEdited(const FText&)
{
 if (bUpdatingJournal || !JournalPages.IsValidIndex(JournalPageIndex) || JournalEntries.Num()<7 || !JournalNotes) return;
 auto& Page=JournalPages[JournalPageIndex];
 Page.Label=JournalEntries[0]->GetText().ToString().Left(16);
 Page.Title=JournalEntries[1]->GetText().ToString().Left(32);
 Page.Location=JournalEntries[2]->GetText().ToString().Left(16);
 Page.Notes=JournalNotes->GetText().ToString().Left(2048);
 Page.Days=FMath::Clamp(FCString::Atoi(*JournalEntries[3]->GetText().ToString()),0,999);
 Page.Hours=FMath::Clamp(FCString::Atoi(*JournalEntries[4]->GetText().ToString()),0,23);
 Page.Minutes=FMath::Clamp(FCString::Atoi(*JournalEntries[5]->GetText().ToString()),0,59);
 SaveJournal();
}

bool UACEUIGameplayBinder::HandleJournalNamedClick(const FString& Name)
{
 if (ActivePanelPage!=TEXT("QuestManagementPanel_Field")) return false;
 for (const TCHAR* Tab : {TEXT("Contracts"),TEXT("Journal"),TEXT("PageList")})
  if (Name==FString(Tab)+TEXT("Tab"))
  { ActiveQuestTab=FString(Tab)+TEXT("Page"); bJournalFieldsDirty=true; RefreshQuestOverlays(); return true; }
 if (ActiveQuestTab==TEXT("ContractsPage")) return false;
 if (ActiveQuestTab==TEXT("PageListPage") && Name.Contains(TEXT("PageListBoxScrollbar")))
 { ScrollQuestList(Name.Contains(TEXT("Up"),ESearchCase::IgnoreCase) ? 1.f : -1.f); return true; }
 if (Name==TEXT("NewPageButton")) { JournalPages.AddDefaulted(); JournalPageIndex=JournalPages.Num()-1; }
 else if (Name==TEXT("JournalPreviousButton")) --JournalPageIndex;
 else if (Name==TEXT("JournalNextButton")) ++JournalPageIndex;
 else if (Name==TEXT("FirstPageButton")) JournalPageIndex=0;
 else if (Name==TEXT("LastPageButton")) JournalPageIndex=JournalPages.Num()-1;
 else if (Name==TEXT("DeletePageButton")) { if(JournalPages.IsValidIndex(JournalPageIndex))JournalPages.RemoveAt(JournalPageIndex); }
 else if (Name==TEXT("AddLocationButton"))
 {
  if (JournalPages.IsValidIndex(JournalPageIndex) && Client)
  {
   FACEWorldObject Self;
   if (Client->GetWorldObject(Client->GetPlayerGuid(),Self))
   {
    const uint32 Cell=uint32(Self.Position.CellId);
    const double NS=(((Cell>>16)&255)*192.+Self.Position.Location.Y)/240.-102.4;
    const double EW=(((Cell>>24)&255)*192.+Self.Position.Location.X)/240.-102.4;
    JournalPages[JournalPageIndex].Location=FString::Printf(TEXT("%.1f%s %.1f%s"),FMath::Abs(NS),NS>=0?TEXT("N"):TEXT("S"),FMath::Abs(EW),EW>=0?TEXT("E"):TEXT("W"));
   }
  }
 }
 else if (Name==TEXT("StartTimerButton"))
 {
  if (JournalPages.IsValidIndex(JournalPageIndex))
  {
   auto& P=JournalPages[JournalPageIndex];
   P.TimerEnd=P.TimerEnd>0 ? 0 : double(FDateTime::UtcNow().ToUnixTimestamp())+P.Days*86400+P.Hours*3600+P.Minutes*60;
  }
 }
 else if (Name==TEXT("SearchPageButton")) JournalSearch=JournalEntries.IsValidIndex(6)?JournalEntries[6]->GetText().ToString():FString();
 else if (Name==TEXT("ResetSearchButton")) {JournalSearch.Reset(); if(JournalEntries.IsValidIndex(6))JournalEntries[6]->SetText(FText::GetEmpty());}
 else if (Name==TEXT("TitleSortButton") || Name==TEXT("LabelSortButton") || Name==TEXT("TimerSortButton") || Name==TEXT("PageSortButton"))
 {
  bJournalSortDescending=JournalSortColumn==Name ? !bJournalSortDescending : false;
  JournalSortColumn=Name; JournalScrollOffset=0;
 }
 else return false;
 if (JournalPages.IsEmpty()) JournalPages.AddDefaulted();
 JournalPageIndex=FMath::Clamp(JournalPageIndex,0,JournalPages.Num()-1);
 bJournalFieldsDirty=true; SaveJournal(); RefreshQuestOverlays(); return true;
}

void UACEUIGameplayBinder::RefreshJournalOverlays()
{
 const FString File=(JournalStorageRoot.IsEmpty() ? FPaths::ProjectSavedDir()/TEXT("Journal") : JournalStorageRoot)/(FMD5::HashAnsiString(
  *FString::Printf(TEXT("%s:%08X"),*Client->GetServerName(),Client->GetPlayerGuid()))+TEXT(".json"));
 if (JournalFile!=File)
 {
  JournalFile=File; JournalPages.Reset(); JournalPageIndex=0; JournalScrollOffset=0; JournalSearch.Reset();
  FString Text; TSharedPtr<FJsonObject> Root;
  if(FFileHelper::LoadFileToString(Text,*File) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root) && Root)
  {
   const TArray<TSharedPtr<FJsonValue>>* Pages=nullptr;
   if(Root->TryGetArrayField(TEXT("pages"),Pages)) for(const auto& V:*Pages)
   {
    const auto O=V->AsObject(); if(!O)continue;
    auto& P=JournalPages.AddDefaulted_GetRef();
    O->TryGetStringField(TEXT("label"),P.Label); O->TryGetStringField(TEXT("title"),P.Title);
    O->TryGetStringField(TEXT("notes"),P.Notes); O->TryGetStringField(TEXT("location"),P.Location);
    O->TryGetNumberField(TEXT("days"),P.Days); O->TryGetNumberField(TEXT("hours"),P.Hours);
    O->TryGetNumberField(TEXT("minutes"),P.Minutes); O->TryGetNumberField(TEXT("timerEnd"),P.TimerEnd);
   }
  }
  if(JournalPages.IsEmpty())JournalPages.AddDefaulted(); bJournalFieldsDirty=true;
 }
 while(JournalEntries.Num()<7)
 {
  auto* Entry=Canvas->WidgetTree->ConstructWidget<UACERetailTextEntry>();
  Entry->ContentMargins=FMargin(3,0); Entry->TextColor=FLinearColor::Black;
  const int32 Index=JournalEntries.Num();
  Entry->MaxLength=Index==0 || Index==2 ? 16 : Index==1 ? 32 : Index==6 ? 1000 : 3;
  Entry->bDigitsOnly=Index>=3 && Index<=5;
  Entry->OnTextChanged.AddDynamic(this,&UACEUIGameplayBinder::HandleJournalEdited);
  JournalEntries.Add(Entry);
 }
 if(!JournalNotes)
 {
  JournalNotes=Canvas->WidgetTree->ConstructWidget<UACERetailTextEntry>();
  JournalNotes->bMultiline=true; JournalNotes->MaxLength=2048;
  JournalNotes->ContentMargins=FMargin(3,0); JournalNotes->TextColor=FLinearColor::Black;
  JournalNotes->OnTextChanged.AddDynamic(this,&UACEUIGameplayBinder::HandleJournalEdited);
 }
 JournalPageIndex=FMath::Clamp(JournalPageIndex,0,JournalPages.Num()-1);
 // DAT buttons start ghosted until gmJournalUI populates the journal.
 // Update their input state as well as their captions after every page change.
 auto Enable=[&](const TCHAR* Name,bool Enabled)
 {
  if(auto Button=Manager->FindElementUnder(ActiveQuestTab,Name))
  { Button->bActivatable=Enabled; Button->bGhosted=!Enabled; }
 };
 Enable(TEXT("NewPageButton"),true);
 Enable(TEXT("JournalPreviousButton"),JournalPageIndex>0);
 Enable(TEXT("FirstPageButton"),JournalPageIndex>0);
 Enable(TEXT("JournalNextButton"),JournalPageIndex+1<JournalPages.Num());
 Enable(TEXT("LastPageButton"),JournalPageIndex+1<JournalPages.Num());
 Enable(TEXT("DeletePageButton"),JournalPages.IsValidIndex(JournalPageIndex));
 const auto& P=JournalPages[JournalPageIndex];
 if(bJournalFieldsDirty)
 {
  bUpdatingJournal=true;
  const FString Values[]={P.Label,P.Title,P.Location,FString::FromInt(P.Days),FString::FromInt(P.Hours),FString::FromInt(P.Minutes)};
  for(int32 I=0;I<6;++I)JournalEntries[I]->SetText(FText::FromString(Values[I]));
  JournalNotes->SetText(FText::FromString(P.Notes)); bUpdatingJournal=false; bJournalFieldsDirty=false;
 }
 int32 LabelIndex=0;
 auto Caption=[&](const TCHAR* Name,const FString& Value)
 {
  if(JournalLabels.Num()<=LabelIndex)JournalLabels.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
  PlaceTextUnder(JournalLabels[LabelIndex++],ActiveQuestTab,Name,Value,9,FLinearColor::Black,100050);
 };
 if(ActiveQuestTab==TEXT("JournalPage"))
 {
  const TCHAR* Names[]={TEXT("PageLabelEntryBox"),TEXT("PageTitleEntryBox"),TEXT("LocationText"),TEXT("DaysEntryBox"),TEXT("HoursEntryBox"),TEXT("MinutesEntryBox")};
  for(int32 I=0;I<6;++I) if(auto E=Manager->FindElementUnder(ActiveQuestTab,Names[I]))
  { JournalEntries[I]->SetRetailElement(Canvas->GetResourceResolver(),E); Canvas->PlaceWidgetAtElement(JournalEntries[I],E,100051); JournalEntries[I]->SetVisibility(ESlateVisibility::Visible); }
  if(auto E=Manager->FindElementUnder(ActiveQuestTab,TEXT("NotesEntryBox")))
  {
   JournalNotes->SetRetailElement(Canvas->GetResourceResolver(),E); Canvas->PlaceWidgetAtElement(JournalNotes,E,100051); JournalNotes->SetVisibility(ESlateVisibility::Visible);
   const float End=JournalNotes->GetScrollOffsetOfEnd();
   SyncDatScrollbar(Manager->FindElementUnder(ActiveQuestTab,TEXT("NotesScrollbar")),End>0?JournalNotes->GetScrollOffset()/End:0.f,
    E->Height>0 ? float(E->Height)/(E->Height+End) : 1.f);
  }
  Caption(TEXT("NewPageButton"),TEXT("New")); Caption(TEXT("PageLabelLabel"),TEXT("Label:"));
  Caption(TEXT("PageTitleLabel"),TEXT("Title:")); Caption(TEXT("NotesLabel"),TEXT("Notes:"));
  Caption(TEXT("FirstPageButton"),TEXT("First")); Caption(TEXT("LastPageButton"),TEXT("Last"));
  Caption(TEXT("PageNumberLabel"),FString::Printf(TEXT("%d / %d"),JournalPageIndex+1,JournalPages.Num()));
  Caption(TEXT("LocationLabel"),TEXT("Location:")); Caption(TEXT("AddLocationButton"),TEXT("Add"));
  Caption(TEXT("TimerLabel"),TEXT("Timer:")); Caption(TEXT("DaysLabel"),TEXT("d"));
  Caption(TEXT("HoursLabel"),TEXT("h")); Caption(TEXT("MinutesLabel"),TEXT("m"));
  const bool Running=P.TimerEnd>0;
  for(int32 I=3;I<6;++I)if(Running)JournalEntries[I]->SetVisibility(ESlateVisibility::Collapsed);
  Caption(TEXT("StartTimerButton"),Running?TEXT("Stop"):TEXT("Start"));
  Caption(TEXT("TimerText"),Running?JournalTimerText(P.TimerEnd):FString());
 }
 else
 {
  Caption(TEXT("PageSortButton"),TEXT("#")); Caption(TEXT("TitleSortButton"),TEXT("Title"));
  Caption(TEXT("TimerSortButton"),TEXT("Timer")); Caption(TEXT("LabelSortButton"),TEXT("Label"));
  Caption(TEXT("DeletePageButton"),TEXT("Delete")); Caption(TEXT("SearchPageButton"),TEXT("Search:")); Caption(TEXT("ResetSearchButton"),TEXT("Reset"));
  if(auto E=Manager->FindElementUnder(ActiveQuestTab,TEXT("SearchEntryBox")))
  { JournalEntries[6]->SetRetailElement(Canvas->GetResourceResolver(),E); Canvas->PlaceWidgetAtElement(JournalEntries[6],E,100051); JournalEntries[6]->SetVisibility(ESlateVisibility::Visible); JournalEntries[6]->TextColor=FLinearColor::White; }
  auto List=Manager->FindElementUnder(ActiveQuestTab,TEXT("PageListBox")); if(!List)return;
  const int32 MaxRows=FMath::Max(1,List->Height/20);
  while(QuestRows.Num()<MaxRows)QuestRows.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
  while(JournalRowElements.Num()<MaxRows)JournalRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x21000067,0x10000589));
  TArray<int32> Filtered;
  for(int32 I=0;I<JournalPages.Num();++I)
  {
   const auto& Page=JournalPages[I];
   if(JournalSearch.IsEmpty() || Page.Title.Contains(JournalSearch) || Page.Notes.Contains(JournalSearch) || Page.Label.Contains(JournalSearch)) Filtered.Add(I);
  }
  JournalFilteredCount=Filtered.Num();
  // Retail sorts a display copy. Stored page numbers and the current page are stable.
  Filtered.StableSort([&](int32 A,int32 B)
  {
   const auto& L=JournalPages[A]; const auto& R=JournalPages[B]; int32 Order=0;
   if(JournalSortColumn==TEXT("TitleSortButton"))Order=L.Title.Compare(R.Title,ESearchCase::IgnoreCase);
   else if(JournalSortColumn==TEXT("LabelSortButton"))Order=L.Label.Compare(R.Label,ESearchCase::IgnoreCase);
   else if(JournalSortColumn==TEXT("TimerSortButton"))
   {
    const bool LeftRunning=L.TimerEnd>0,RightRunning=R.TimerEnd>0;
    if(LeftRunning!=RightRunning)Order=LeftRunning?-1:1;
    else if(LeftRunning && L.TimerEnd!=R.TimerEnd)Order=L.TimerEnd<R.TimerEnd?-1:1;
   }
   if(Order==0)Order=A<B?-1:A>B?1:0;
   return bJournalSortDescending ? Order>0 : Order<0;
  });
  JournalScrollOffset=FMath::Clamp(JournalScrollOffset,0,FMath::Max(0,Filtered.Num()-MaxRows));
  if(auto Bar=Manager->FindElementUnder(ActiveQuestTab,TEXT("PageListBoxScrollbar")))
   SyncDatScrollbar(Bar,Filtered.Num()>MaxRows ? float(JournalScrollOffset)/(Filtered.Num()-MaxRows) : 0.f,
    Filtered.Num()>0 ? FMath::Min(1.f,float(MaxRows)/Filtered.Num()) : 1.f);
  QuestRowIds.Init(0,QuestRows.Num()); int32 Row=0;
  for(int32 N=JournalScrollOffset;N<Filtered.Num() && Row<MaxRows;++N)
  {
   const int32 I=Filtered[N]; const auto& Page=JournalPages[I];
   const auto Entry=JournalRowElements[Row]; if(!Entry)continue;
   if(Entry->Parent.Pin()!=List)List->AddChild(Entry);
   Entry->bVisible=true;Entry->Y=Row*20;Entry->Width=List->Width;
   Entry->bUseExplicitState=true;Entry->DefaultState=I==JournalPageIndex?6:1;
   const FString Timer=JournalTimerText(Page.TimerEnd);
   const FString Values[]={FString::FromInt(I+1),Page.Title,Timer,Page.Label};
   for(int32 Column=0;Column<Entry->Children.Num() && Column<4;++Column)
   {
    UTextBlock* Text=nullptr;
    if(Column==1)Text=QuestRows[Row];
    else
    {
     if(JournalLabels.Num()<=LabelIndex)JournalLabels.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
     Text=JournalLabels[LabelIndex++];
    }
    Text->SetClipping(EWidgetClipping::ClipToBounds);
    PlaceTextOnElement(Text,Entry->Children[Column],Values[Column],9,FLinearColor::White,100052);
   }
   QuestRowIds[Row++]=-I-1;
  }
 }
}
