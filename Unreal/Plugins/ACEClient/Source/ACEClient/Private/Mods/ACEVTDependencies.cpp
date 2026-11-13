#include "ACEVTProfile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

namespace ACEVTProfile
{
TSharedPtr<FJsonObject> ConvertFile(const FString& Path,TArray<FString>& Issues)
{
    Issues.Reset();auto Library=MakeShared<FJsonObject>();TSet<FString> Seen;int64 TotalBytes=0;
    const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::GetPath(Path));
    TSharedPtr<FJsonObject> CraftData;bool TriedCraftData=false;
    auto AttachPeaRecipes=[&](const TSharedPtr<FJsonObject>& Document)
    {
        bool Needed=false,NeedMonsters=false,NeedGrenades=false;
        const TArray<TSharedPtr<FJsonValue>>* Tables=nullptr;if(!Document->TryGetArrayField(TEXT("tables"),Tables))return;
        for(const auto& Table:*Tables)
        {
            auto T=Table->AsObject();FString Name;if(!T->TryGetStringField(TEXT("name"),Name))continue;
            if(Name==TEXT("MyMonsters"))
            {
                const auto& Columns=T->GetArrayField(TEXT("columns"));int32 NameColumn=INDEX_NONE;
                for(int32 I=0;I<Columns.Num();++I)if(Columns[I]->AsString()==TEXT("MonsterName"))NameColumn=I;
                if(NameColumn!=INDEX_NONE)for(const auto& Row:T->GetArrayField(TEXT("rows")))if(Row->AsArray().IsValidIndex(NameColumn)&&Row->AsArray()[NameColumn]->Type==EJson::String)
                {const FString Text=Row->AsArray()[NameColumn]->AsString().ToLower();if(Text.Contains(TEXT("species"))||Text.Contains(TEXT("maxhp"))){Needed=true;NeedMonsters=true;}}
                continue;
            }
            if(Name!=TEXT("AssistItems"))continue;
            const auto& Columns=T->GetArrayField(TEXT("columns"));int32 TypeColumn=INDEX_NONE;
            for(int32 I=0;I<Columns.Num();++I)if(Columns[I]->AsString()==TEXT("Type"))TypeColumn=I;
            if(TypeColumn!=INDEX_NONE)for(const auto& Row:T->GetArrayField(TEXT("rows")))if(Row->AsArray().IsValidIndex(TypeColumn))
            {const auto V=Row->AsArray()[TypeColumn];if(V->Type==EJson::Number){if(V->AsNumber()==9||V->AsNumber()==11)Needed=true;if(V->AsNumber()==7){Needed=true;NeedGrenades=true;}}}
        }
        if(!Needed)return;
        if(!TriedCraftData)
        {
            TriedCraftData=true;FString Database=Folder/TEXT("gameinfodb.ugd"),Source,Error;
            if(!IFileManager::Get().FileExists(*Database))
            {TArray<FString> Files;IFileManager::Get().FindFiles(Files,*(Folder/TEXT("*")),true,false);for(const auto& File:Files)if(File.Equals(TEXT("gameinfodb.ugd"),ESearchCase::IgnoreCase)){Database=Folder/File;break;}}
            const int64 Bytes=IFileManager::Get().FileSize(*Database);TotalBytes+=FMath::Max<int64>(0,Bytes);
            if(Bytes>0&&Bytes<=4*1024*1024&&TotalBytes<=8*1024*1024&&FFileHelper::LoadFileToString(Source,*Database))
                CraftData=Read(Source,TEXT("usd"),Error); // UGD uses the same typed-table serialization.
        }
        TArray<TSharedPtr<FJsonValue>> Recipes;auto Monsters=MakeShared<FJsonObject>(),Grenades=MakeShared<FJsonObject>();bool HasMonsters=false;
        if(CraftData)for(const auto& Table:CraftData->GetArrayField(TEXT("tables")))
        {
            const auto T=Table->AsObject();const FString TableName=T->GetStringField(TEXT("name"));
            if(TableName!=TEXT("CraftInteractions")&&TableName!=TEXT("SpeciesMembers")&&TableName!=TEXT("GrenadeOptions"))continue;
            const auto& Columns=T->GetArrayField(TEXT("columns"));TMap<FString,int32> Index;
            for(int32 I=0;I<Columns.Num();++I)Index.Add(Columns[I]->AsString(),I);
            if(TableName==TEXT("GrenadeOptions"))
            {
                const TCHAR* Fields[]={TEXT("WieldReqType"),TEXT("WieldReqAttribute"),TEXT("WieldReqValue"),TEXT("Spell"),TEXT("Spellcraft")};
                bool Complete=Index.Contains(TEXT("GrenName"));for(const TCHAR* Field:Fields)Complete&=Index.Contains(Field);if(!Complete)continue;
                for(const auto& Row:T->GetArrayField(TEXT("rows")))
                {
                    const auto& Cells=Row->AsArray();if(Cells.Num()!=Columns.Num())continue;
                    const auto Name=Cells[Index[TEXT("GrenName")]];if(Name->Type!=EJson::String||Name->AsString().IsEmpty()||Name->AsString().Len()>256)continue;
                    auto G=MakeShared<FJsonObject>();bool Valid=true;
                    for(const TCHAR* Field:Fields)
                    {const auto V=Cells[Index[Field]];if(V->Type!=EJson::Number||V->AsNumber()<0||V->AsNumber()>MAX_int32||V->AsNumber()!=FMath::FloorToDouble(V->AsNumber())){Valid=false;break;}G->SetNumberField(Field,V->AsNumber());}
                    if(Valid&&Grenades->Values.Num()<512&&G->GetNumberField(TEXT("WieldReqType"))<=12&&G->GetNumberField(TEXT("Spell"))>0)Grenades->SetObjectField(Name->AsString(),G);
                }
                continue;
            }
            if(TableName==TEXT("SpeciesMembers"))
            {
                if(!Index.Contains(TEXT("Monster"))||!Index.Contains(TEXT("Species"))||!Index.Contains(TEXT("MaximumHealth")))continue;
                HasMonsters=true;
                for(const auto& Row:T->GetArrayField(TEXT("rows")))
                {
                    const auto& Cells=Row->AsArray();if(Cells.Num()!=Columns.Num())continue;
                    const auto Name=Cells[Index[TEXT("Monster")]],Species=Cells[Index[TEXT("Species")]],Health=Cells[Index[TEXT("MaximumHealth")]];
                    if(Name->Type!=EJson::String||Name->AsString().IsEmpty()||Name->AsString().Len()>256||Species->Type!=EJson::Number||Health->Type!=EJson::Number)continue;
                    if(Species->AsNumber()<0||Species->AsNumber()>MAX_int32||Health->AsNumber()<0||Health->AsNumber()>MAX_int32||FMath::FloorToDouble(Species->AsNumber())!=Species->AsNumber()||FMath::FloorToDouble(Health->AsNumber())!=Health->AsNumber())continue;
                    auto Monster=MakeShared<FJsonObject>();Monster->SetNumberField(TEXT("species"),Species->AsNumber());Monster->SetNumberField(TEXT("max_health"),Health->AsNumber());
                    Monsters->SetObjectField(Name->AsString(),Monster);
                }
                continue;
            }
            if(!Index.Contains(TEXT("UseItem1"))||!Index.Contains(TEXT("UseItem2"))||!Index.Contains(TEXT("ResultItem"))||!Index.Contains(TEXT("ResultCount")))continue;
            for(const auto& Row:T->GetArrayField(TEXT("rows")))
            {
                const auto& Cells=Row->AsArray();if(Cells.Num()!=Columns.Num())continue;
                const auto Tool=Cells[Index[TEXT("UseItem1")]],Input=Cells[Index[TEXT("UseItem2")]],Output=Cells[Index[TEXT("ResultItem")]],Count=Cells[Index[TEXT("ResultCount")]];
                if(Tool->Type!=EJson::String||Tool->AsString()!=TEXT("Splitting Tool")||Input->Type!=EJson::String||Output->Type!=EJson::String||Count->Type!=EJson::Number)continue;
                if(Input->AsString().IsEmpty()||Output->AsString().IsEmpty()||Count->AsNumber()<=0||Count->AsNumber()>10000||FMath::FloorToDouble(Count->AsNumber())!=Count->AsNumber())continue;
                auto Recipe=MakeShared<FJsonObject>();Recipe->SetStringField(TEXT("tool"),Tool->AsString());Recipe->SetStringField(TEXT("input"),Input->AsString());Recipe->SetStringField(TEXT("output"),Output->AsString());Recipe->SetNumberField(TEXT("count"),Count->AsNumber());
                Recipes.Add(MakeShared<FJsonValueObject>(Recipe));
            }
        }
        if(Recipes.Num()>512){Issues.Add(TEXT("Pea recipe catalog exceeds 512 recipes"));return;}
        Document->SetArrayField(TEXT("pea_recipes"),Recipes);
        if(NeedMonsters&&HasMonsters)Document->SetObjectField(TEXT("monster_catalog"),Monsters);
        if(NeedGrenades)Document->SetObjectField(TEXT("grenade_catalog"),Grenades);
    };
    TFunction<TSharedPtr<FJsonObject>(const FString&)> Visit;
    Visit=[&](const FString& File)->TSharedPtr<FJsonObject>
    {
        const FString Key=File.ToLower();
        if(Seen.Contains(Key))return Library->HasTypedField<EJson::Object>(Key)?Library->GetObjectField(Key):nullptr;
        if(Seen.Num()>=64){Issues.AddUnique(TEXT("Profile graph exceeds 64 files"));return nullptr;}
        Seen.Add(Key);
        // Never resolve an imported command as an arbitrary filesystem path.
        if(File!=FPaths::GetCleanFilename(File)||File.Contains(TEXT(".."))||File.Contains(TEXT(":")))
        {Issues.Add(TEXT("Invalid dependency filename: ")+File);return nullptr;}
        FString Full=Folder/File;
        if(!IFileManager::Get().FileExists(*Full))
        {
            TArray<FString> Names;IFileManager::Get().FindFiles(Names,*(Folder/TEXT("*")),true,false);
            for(const auto& Name:Names)if(Name.Equals(File,ESearchCase::IgnoreCase)){Full=Folder/Name;break;}
        }
        const int64 Bytes=IFileManager::Get().FileSize(*Full);TotalBytes+=FMath::Max<int64>(0,Bytes);
        FString Source,Error;
        if(Bytes<0||Bytes>4*1024*1024||TotalBytes>8*1024*1024||!FFileHelper::LoadFileToString(Source,*Full))
        {Issues.Add(TEXT("Missing or oversized dependency: ")+File);return nullptr;}
        auto Document=Read(Source,FPaths::GetExtension(File).ToLower(),Error);
        if(!Document){Issues.Add(File+TEXT(": ")+Error);return nullptr;}
        if(FPaths::GetExtension(File).Equals(TEXT("usd"),ESearchCase::IgnoreCase))AttachPeaRecipes(Document);
        TArray<FString> LocalIssues;auto Profile=Convert(Document,LocalIssues);
        for(const auto& Issue:LocalIssues)Issues.Add(File+TEXT(": ")+Issue);
        Library->SetObjectField(Key,Profile); // Register before recursion: cyclic state programs are valid.
        TFunction<void(const TSharedPtr<FJsonValue>&,int)> Walk;
        Walk=[&](const TSharedPtr<FJsonValue>& Value,int Depth)
        {
            if(Depth>64)return;
            if(Value->Type==EJson::Object)
            {
                const auto O=Value->AsObject();FString Op,Name;
                if(O->TryGetStringField(TEXT("op"),Op)&&Op==TEXT("load")&&O->TryGetStringField(TEXT("profile"),Name))Visit(Name);
                for(const auto& Pair:O->Values)Walk(Pair.Value,Depth+1);
            }
            else if(Value->Type==EJson::Array)for(const auto& Child:Value->AsArray())Walk(Child,Depth+1);
        };
        Walk(MakeShared<FJsonValueObject>(Profile),0);return Profile;
    };
    auto Root=Visit(FPaths::GetCleanFilename(Path));
    if(!Root)return MakeShared<FJsonObject>();
    auto Result=MakeShared<FJsonObject>(*Root);
    if(Library->Values.Num()>1)Result->SetObjectField(TEXT("vt_library"),Library);
    // Also include the root when it references itself.
    else {FString Serialized;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Serialized));if(Serialized.Contains(TEXT("\"load\"")))Result->SetObjectField(TEXT("vt_library"),Library);}
    return Result;
}
}
