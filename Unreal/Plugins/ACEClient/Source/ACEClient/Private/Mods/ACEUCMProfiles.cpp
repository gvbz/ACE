#include "Mods/ACEUCMProfiles.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace ACEUCMProfiles
{
namespace
{
    bool Equal(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
    { return A && B && FJsonValue::CompareEqual(FJsonValueObject(A), FJsonValueObject(B)); }
    FString Encode(const TSharedPtr<FJsonObject>& Object)
    {
        FString Text;
        if (Object) FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
        return Text;
    }
    TSharedPtr<FJsonObject> Read(const FString& Path)
    {
        FString Text; TSharedPtr<FJsonObject> Object;
        if (IFileManager::Get().FileSize(*Path) > 8 * 1024 * 1024) return nullptr;
        if (FFileHelper::LoadFileToString(Text, *Path)) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
        return Object;
    }
    bool Write(const FString& Path, const TSharedPtr<FJsonObject>& Object, bool Exclusive = false)
    {
        if (Exclusive && IFileManager::Get().FileExists(*Path)) return false;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        if (Exclusive) return FFileHelper::SaveStringToFile(Encode(Object), *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_NoReplaceExisting);
        const FString Temp = Path + TEXT(".tmp");
        return FFileHelper::SaveStringToFile(Encode(Object), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*Path, *Temp, true, true);
    }
    FString Path(const FString& Root, const FString& Section, const FString& Name)
    { return Section == TEXT("ucm") && Name == ActiveName() ? Root / TEXT("ucm-selection.json")
        : Root / TEXT("Profiles") / Section / (Name + TEXT(".json")); }
    TSharedPtr<FJsonObject> ReadSection(const FString& Root, const FString& Section, const FString& Name, FString& Error)
    {
        auto Part = SafeName(Name) ? Read(Path(Root, Section, Name)) : nullptr;
        if (!Part) { Error = TEXT("Cannot load ") + Section + TEXT(" profile: ") + Name; return nullptr; }
        if(Section==TEXT("Metas")){Part->RemoveField(TEXT("vt_revision"));Part->RemoveField(TEXT("vt_loaded_kind"));}
        for (const auto& Pair : Part->Values) if (SectionFor(FString(Pair.Key)) != Section)
        { Error = Section + TEXT(" profile contains a setting from another section: ") + FString(Pair.Key); return nullptr; }
        NormalizeDamageTypes(Part,Section);
        return Part;
    }
    void Replace(TSharedPtr<FJsonObject>& Profile, const FString& Section, const TSharedPtr<FJsonObject>& Part)
    {
        TArray<FString> Keys; for (const auto& Pair : Profile->Values) Keys.Add(FString(Pair.Key));
        for (const auto& Key : Keys) if (SectionFor(Key) == Section) Profile->RemoveField(Key);
        for (const auto& Pair : Part->Values) Profile->Values.Add(Pair.Key, Pair.Value);
    }
}
const TArray<FString>& Sections()
{
    static const TArray<FString> Names = {TEXT("Character"), TEXT("Routes"), TEXT("Loot"), TEXT("Monsters"), TEXT("Metas"), TEXT("Vendors")};
    return Names;
}
bool SafeName(const FString& Name)
{
    if (Name.IsEmpty() || Name.Len() > 64 || Name != Name.TrimStartAndEnd()) return false;
    for (TCHAR C : Name) if (!(FChar::IsAlnum(C) || C == ' ' || C == '_' || C == '-')) return false;
    // These names are devices on Windows even with a .json extension. Use the
    // same portable filename rules when creating, listing and loading profiles.
    const FString Upper=Name.ToUpper();
    if (Upper==TEXT("CON") || Upper==TEXT("PRN") || Upper==TEXT("AUX") || Upper==TEXT("NUL")) return false;
    if (Upper.Len()==4 && (Upper.StartsWith(TEXT("COM")) || Upper.StartsWith(TEXT("LPT")))
        && Upper[3]>='1' && Upper[3]<='9') return false;
    return true;
}
FString SectionFor(const FString& Key)
{
    // Run activity switches stay local to the setup; shared rules do not toggle
    // another hunt's activities when their settings are edited.
    static const TSet<FString> Run = {TEXT("buffing"), TEXT("combat"), TEXT("recovery"), TEXT("navigation"),
        TEXT("looting"), TEXT("auto_craft"), TEXT("auto_fellowship"), TEXT("buff_others"), TEXT("vendor_restock"), TEXT("meta_enabled"), TEXT("idle_peace"),
        TEXT("import_source"), TEXT("ucm_format_version"), TEXT("loot_revision"), TEXT("vt_revision"), TEXT("vt_loaded_kind")};
    if (Run.Contains(Key)) return TEXT("ucm");
    if (Key == TEXT("monsters") || Key == TEXT("monster_catalog") || Key == TEXT("monster_damage_version")) return TEXT("Monsters");
    if (Key.StartsWith(TEXT("vendor_"))) return TEXT("Vendors");
    if (Key == TEXT("states") || Key == TEXT("initial_state") || Key.StartsWith(TEXT("vt_"))
        || Key.StartsWith(TEXT("meta_")) || Key == TEXT("met_source")) return TEXT("Metas");
    static const TSet<FString> Route = {TEXT("route"), TEXT("navigation_mode"), TEXT("loop_route"), TEXT("reverse_route"),
        TEXT("waypoint_radius"), TEXT("open_doors"), TEXT("nav_priority"), TEXT("nav_source")};
    if (Route.Contains(Key) || Key.StartsWith(TEXT("route_")) || Key.StartsWith(TEXT("follow_"))) return TEXT("Routes");
    static const TSet<FString> Loot = {TEXT("salvage_policy"), TEXT("salvage"), TEXT("autostack"), TEXT("autocram"), TEXT("utl_source"),
        TEXT("learn_scrolls"), TEXT("read_unknown_scrolls"), TEXT("move_loose_items"), TEXT("combine_salvage")};
    if (Loot.Contains(Key) || Key.StartsWith(TEXT("loot_")) || Key.StartsWith(TEXT("salvage_"))) return TEXT("Loot");
    return TEXT("Character"); // Includes future/unknown character options without dropping data.
}
TSharedPtr<FJsonObject> Copy(const TSharedPtr<FJsonObject>& Object)
{
    if(!Object)return nullptr;
    TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&)> Clone;
    Clone=[&](const TSharedPtr<FJsonValue>& Value)->TSharedPtr<FJsonValue>
    {
        if(Value->Type==EJson::Object)return MakeShared<FJsonValueObject>(Copy(Value->AsObject()));
        if(Value->Type==EJson::Array){TArray<TSharedPtr<FJsonValue>> Values;Values.Reserve(Value->AsArray().Num());for(const auto& Item:Value->AsArray())Values.Add(Clone(Item));return MakeShared<FJsonValueArray>(Values);}
        // Scalar JSON values are immutable; only objects/arrays need ownership.
        return Value;
    };
    auto Result=MakeShared<FJsonObject>();
    for(const auto& Pair:Object->Values)Result->Values.Add(Pair.Key,Clone(Pair.Value));
    return Result;
}
void NormalizeDamageTypes(const TSharedPtr<FJsonObject>& Profile,const FString& Section)
{
    // Through v144 the UI/importer encoded Fire=8 and Cold=16. Preserve the
    // chosen labels while adopting the retail bits. Version each shared section
    // independently; loading a different Monsters file must migrate that file too.
    if(!Profile)return;
    auto Swap=[](const TSharedPtr<FJsonObject>& O,const TCHAR* Key)
    {double V=0;if(O->TryGetNumberField(Key,V)&&(V==8||V==16))O->SetNumberField(Key,V==8?16:8);};
    double Version=0;
    if((Section.IsEmpty()||Section==TEXT("Character"))
        &&(!Profile->TryGetNumberField(TEXT("character_damage_version"),Version)||Version<2))
    {Swap(Profile,TEXT("damage_type"));Profile->SetNumberField(TEXT("character_damage_version"),2);}
    if((Section.IsEmpty()||Section==TEXT("Monsters"))
        &&(!Profile->TryGetNumberField(TEXT("monster_damage_version"),Version)||Version<2))
    {
        const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;
        if(Profile->TryGetArrayField(TEXT("monsters"),Rules))for(const auto& Rule:*Rules)
            if(Rule->Type==EJson::Object){Swap(Rule->AsObject(),TEXT("damage_type"));Swap(Rule->AsObject(),TEXT("secondary_vuln"));}
        Profile->SetNumberField(TEXT("monster_damage_version"),2);
    }
}
TSharedPtr<FJsonObject> Extract(const TSharedPtr<FJsonObject>& Profile, const FString& Section)
{
    auto Result = MakeShared<FJsonObject>();
    if (Profile) for (const auto& Pair : Profile->Values) if (SectionFor(FString(Pair.Key)) == Section) Result->Values.Add(Pair.Key, Pair.Value);
    return Result;
}
TArray<FString> List(const FString& Root, const FString& Section)
{
    TArray<FString> Files;
    if (Section != TEXT("ucm") && !Sections().Contains(Section)) return Files;
    IFileManager::Get().FindFiles(Files, *(Root / TEXT("Profiles") / Section / TEXT("*.json")), true, false);
    for (auto& File : Files) File = FPaths::GetBaseFilename(File);
    Files.RemoveAll([](const FString& Name) { return !SafeName(Name); }); Files.Sort();
    return Files;
}
FString UniqueName(const FString& Root, const FString& Section, const FString& Name)
{
    FString Stem;
    for (TCHAR C : Name.Left(48)) Stem.AppendChar(FChar::IsAlnum(C) || C == '_' || C == '-' ? C : '_');
    if (Stem.IsEmpty()) Stem = TEXT("Imported");
    for (int32 N = 1; N < 10000; ++N)
    {
        FString Candidate = Stem + (N == 1 ? FString() : FString::Printf(TEXT("_%d"), N));
        if (!IFileManager::Get().FileExists(*Path(Root, Section, Candidate))) return Candidate;
    }
    return FString();
}
bool Load(const FString& Root, const FString& Name, TSharedPtr<FJsonObject>& Profile, TSharedPtr<FJsonObject>& Links, FString& Error)
{
    auto Manifest = SafeName(Name) ? Read(Path(Root, TEXT("ucm"), Name)) : nullptr;
    if (!Manifest) { Error = TEXT("Cannot read UCM setup: ") + Name; return false; }
    const TSharedPtr<FJsonObject>* Refs = nullptr;
    if (!Manifest->HasField(TEXT("profile_refs")))
    {
        // The host validates legacy data before calling Save to migrate it.
        NormalizeDamageTypes(Manifest);
        Profile = Manifest; Links = MakeShared<FJsonObject>(); return true;
    }
    double Version = 0;
    if (!Manifest->TryGetNumberField(TEXT("ucm_setup_version"), Version) || Version != 2
        || !Manifest->TryGetObjectField(TEXT("profile_refs"), Refs) || (*Refs)->Values.Num() != Sections().Num())
    { Error = TEXT("Invalid linked UCM setup: ") + Name; return false; }
    const TSharedPtr<FJsonObject>* Run = nullptr;
    if (!Manifest->TryGetObjectField(TEXT("run"), Run)) { Error = TEXT("Missing UCM run settings"); return false; }
    auto Resolved = Copy(*Run);
    for (const auto& Pair : Resolved->Values) if (SectionFor(FString(Pair.Key)) != TEXT("ucm"))
    { Error = TEXT("Invalid UCM run setting: ") + FString(Pair.Key); return false; }
    for (const auto& Section : Sections())
    {
        FString PartName;
        if (!(*Refs)->TryGetStringField(Section, PartName)) { Error = TEXT("Missing profile reference: ") + Section; return false; }
        auto Part = ReadSection(Root, Section, PartName, Error); if (!Part) return false;
        Replace(Resolved, Section, Part);
    }
    if (Encode(Resolved).Len() > 8 * 1024 * 1024) { Error = TEXT("Resolved UCM setup exceeds 8 MiB"); return false; }
    Profile = Resolved; Links = Copy(*Refs); return true;
}
bool Save(const FString& Root, const FString& Name, TSharedPtr<FJsonObject>& Profile, TSharedPtr<FJsonObject>& Links,
    const TSharedPtr<FJsonObject>& Baseline, FString& Error, TFunction<bool(const TSharedPtr<FJsonObject>&)> Validate)
{
    if (!SafeName(Name) || !Profile) { Error = TEXT("Invalid UCM setup name or data"); return false; }
    auto NextLinks = Links ? Copy(Links) : MakeShared<FJsonObject>();
    // Only top-level section replacement happens below. Nested edits already
    // belong to the candidate; cloning every loot condition again is unnecessary.
    TSharedPtr<FJsonObject> Resolved = MakeShared<FJsonObject>();
    Resolved->Values = Profile->Values;
    // Native edits already use current retail damage bits. A replacement profile
    // may omit metadata; persist it so the next read cannot migrate new values
    // or look like an external edit. Legacy files are migrated by Load/ReadSection.
    for (const TCHAR* Key : {TEXT("character_damage_version"), TEXT("monster_damage_version")})
        if (!Resolved->HasField(Key)) Resolved->SetNumberField(Key, 2);
    TMap<FString, TSharedPtr<FJsonObject>> Writes, Previous;
    for (const auto& Section : Sections())
    {
        FString PartName; const bool Linked = NextLinks->TryGetStringField(Section, PartName);
        auto Part = Extract(Resolved, Section);
        auto Before = Extract(Baseline, Section);
        if (Baseline) for (const TCHAR* Key : {TEXT("character_damage_version"), TEXT("monster_damage_version")})
            if (SectionFor(Key) == Section && !Before->HasField(Key)) Before->SetNumberField(Key, 2);
        if (!Linked)
        {
            PartName = UniqueName(Root, Section, Name);
            if (PartName.IsEmpty()) { Error = TEXT("Cannot allocate profile name"); return false; }
            NextLinks->SetStringField(Section, PartName);
        }
        auto Disk = Linked ? ReadSection(Root, Section, PartName, Error) : nullptr;
        if (Linked && !Disk) return false;
        // Only changed sections are written. A different setup may have updated
        // an unchanged shared section since this setup was loaded.
        if (Linked && Baseline && Equal(Part, Before))
        { Replace(Resolved, Section, Disk); continue; }
        if (Linked && Baseline && !Equal(Disk, Before) && !Equal(Disk, Part))
        { Error = TEXT("Shared ") + Section + TEXT(" profile changed on disk. Reload linked profiles before editing."); return false; }
        const FString Target = Path(Root, Section, PartName);
        Writes.Add(Target, Part); Previous.Add(Target, Disk);
    }
    if (Encode(Resolved).Len() > 8 * 1024 * 1024 || (Validate && !Validate(Resolved)))
    { Error = TEXT("Invalid linked profile data; no changes were saved. Correct the file and reload."); return false; }
    const FString SetupPath = Path(Root, TEXT("ucm"), Name);
    auto OldSetup = Read(SetupPath);
    if (OldSetup && !OldSetup->HasField(TEXT("profile_refs")))
    {
        const FString BackupName = UniqueName(Root, TEXT("Legacy/ucm"), Name);
        if (BackupName.IsEmpty() || !Write(Path(Root, TEXT("Legacy/ucm"), BackupName), OldSetup, true))
        { Error = TEXT("Cannot back up the original UCM setup; migration was not applied"); return false; }
    }
    auto Manifest = MakeShared<FJsonObject>();
    Manifest->SetNumberField(TEXT("ucm_setup_version"), 2);
    Manifest->SetObjectField(TEXT("profile_refs"), NextLinks);
    Manifest->SetObjectField(TEXT("run"), Extract(Resolved, TEXT("ucm")));
    TArray<FString> Applied;
    auto Rollback = [&]()
    {
        for (const auto& Target : Applied)
            if (Previous[Target]) Write(Target, Previous[Target]); else IFileManager::Get().Delete(*Target);
    };
    for (const auto& Entry : Writes)
    {
        if (!Write(Entry.Key, Entry.Value, !Previous[Entry.Key]))
        { Rollback(); Error = TEXT("Could not save shared profile: ") + Entry.Key; return false; }
        Applied.Add(Entry.Key);
    }
    if (!Write(SetupPath, Manifest)) { Rollback(); Error = TEXT("Could not save UCM setup"); return false; }
    Profile = Resolved; Links = NextLinks; return true;
}
bool Select(const FString& Root, const FString& Section, const FString& Name, TSharedPtr<FJsonObject>& Profile,
    TSharedPtr<FJsonObject>& Links, FString& Error)
{
    if (!Sections().Contains(Section)) { Error = TEXT("Unknown profile section"); return false; }
    auto Part = ReadSection(Root, Section, Name, Error); if (!Part) return false;
    Profile = Copy(Profile); Replace(Profile, Section, Part);
    Links = Links ? Copy(Links) : MakeShared<FJsonObject>(); Links->SetStringField(Section, Name); return true;
}
bool SaveSectionAs(const FString& Root, const FString& Section, const FString& Name,
    const TSharedPtr<FJsonObject>& Profile, FString& Error)
{
    if (!SafeName(Name) || !Sections().Contains(Section)) { Error = TEXT("Use up to 64 letters, numbers, spaces, _ or -. Avoid reserved filenames such as CON."); return false; }
    const FString File=Path(Root, Section, Name);
    if (IFileManager::Get().FileExists(*File))
    { Error = TEXT("That profile already exists. Select it from the dropdown to edit it, or choose a different name."); return false; }
    auto Part = Extract(Profile, Section);
    if (Section == TEXT("Loot")) { Part->SetStringField(TEXT("loot_profile"), Name); Part->RemoveField(TEXT("loot_modified")); }
    if (!Write(File, Part, true))
    { Error = TEXT("Could not create ") + File + TEXT(". Check that the folder is writable and has free disk space."); return false; }
    return true;
}
bool EnsureDefaults(const FString& Root, const TSharedPtr<FJsonObject>& Defaults, FString& Error)
{
    if (!Defaults) { Error = TEXT("Cannot read bundled UCM defaults"); return false; }
    for (const auto& Section : Sections())
    {
        if (IFileManager::Get().FileExists(*Path(Root, Section, TEXT("Default"))))
        { if (!ReadSection(Root, Section, TEXT("Default"), Error)) return false; }
        else if (!SaveSectionAs(Root, Section, TEXT("Default"), Defaults, Error)) return false;
    }
    return true;
}
bool SaveSelection(const FString& Root, const TSharedPtr<FJsonObject>& Profile,
    const TSharedPtr<FJsonObject>& Links, FString& Error)
{
    if (!Profile || !Links || Links->Values.Num() != Sections().Num()) { Error=TEXT("Incomplete profile selection"); return false; }
    for (const auto& Section:Sections())
    {
        FString Name;
        if (!Links->TryGetStringField(Section,Name) || !ReadSection(Root,Section,Name,Error)) return false;
    }
    auto Selection=MakeShared<FJsonObject>();
    Selection->SetNumberField(TEXT("ucm_setup_version"),2); // Retain the validated legacy serialization version.
    Selection->SetObjectField(TEXT("profile_refs"),Copy(Links));
    Selection->SetObjectField(TEXT("run"),Extract(Profile,TEXT("ucm")));
    if (!Write(Path(Root,TEXT("ucm"),ActiveName()),Selection)) { Error=TEXT("Cannot save current profile selection"); return false; }
    return true;
}
static bool RelocateSection(const FString& Root, const FString& Section, const FString& Name, const FString& Replacement, bool Rename, FString& Error)
{
    if (!Sections().Contains(Section) || !SafeName(Name) || Name.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
    { Error = TEXT("The Default profile cannot be deleted. Choose a named profile."); return false; }
    if (!SafeName(Replacement)||(!Rename&&!ReadSection(Root, Section, Replacement, Error)) || !ReadSection(Root, Section, Name, Error)) return false;
    if(Rename&&IFileManager::Get().FileExists(*Path(Root,Section,Replacement))){Error=TEXT("Destination profile exists");return false;}
    TMap<FString, TSharedPtr<FJsonObject>> Before, After;
    auto Selections=List(Root,TEXT("ucm"));
    if (IFileManager::Get().FileExists(*Path(Root,TEXT("ucm"),ActiveName()))) Selections.Add(ActiveName());
    for (const auto& Setup : Selections)
    {
        const FString File = Path(Root, TEXT("ucm"), Setup); auto Manifest = Read(File);
        if (!Manifest) { Error = TEXT("Cannot inspect setup before deleting shared profile: ") + Setup; return false; }
        const TSharedPtr<FJsonObject>* Refs = nullptr;
        if (!Manifest->HasField(TEXT("profile_refs"))) continue; // Legacy setup embeds its own data.
        if (!Manifest->TryGetObjectField(TEXT("profile_refs"), Refs)) { Error = TEXT("Invalid setup: ") + Setup; return false; }
        FString Linked; if (!(*Refs)->TryGetStringField(Section, Linked) || !Linked.Equals(Name, ESearchCase::IgnoreCase)) continue;
        Before.Add(File, Manifest); auto Updated = Copy(Manifest);
        Updated->GetObjectField(TEXT("profile_refs"))->SetStringField(Section, Replacement); After.Add(File, Updated);
    }
    // Meta dependencies are references too. Otherwise reloading a meta would
    // recreate a route/profile that the user had just deliberately deleted.
    for(const auto& MetaName:List(Root,TEXT("Metas")))
    {
        if(!Rename&&Section==TEXT("Metas")&&MetaName.Equals(Name,ESearchCase::IgnoreCase))continue;
        const FString File=Path(Root,TEXT("Metas"),MetaName);auto Original=ReadSection(Root,TEXT("Metas"),MetaName,Error);if(!Original)return false;
        auto Updated=Copy(Original);bool Changed=false;
        const TSharedPtr<FJsonObject>* Refs=nullptr;
        if(Updated->TryGetObjectField(TEXT("meta_profile_refs"),Refs))for(const auto& Pair:(*Refs)->Values)
        {
            if(Pair.Value->Type!=EJson::Object)continue;
            auto Ref=Pair.Value->AsObject();FString Linked;
            if(Ref->TryGetStringField(Section,Linked)&&Linked.Equals(Name,ESearchCase::IgnoreCase))
            {Ref->SetStringField(Section,Replacement);Changed=true;}
        }
        if(Section==TEXT("Routes"))
        {
            TFunction<void(const TSharedPtr<FJsonValue>&,int32)> Visit;
            Visit=[&](const TSharedPtr<FJsonValue>& Value,int32 Depth){
                if(Depth>64)return;
                if(Value->Type==EJson::Array){for(const auto& Child:Value->AsArray())Visit(Child,Depth+1);}
                else if(Value->Type==EJson::Object){auto Object=Value->AsObject();FString Linked;
                    if(Object->TryGetStringField(TEXT("route_profile"),Linked)&&Linked.Equals(Name,ESearchCase::IgnoreCase))
                    {Object->SetStringField(TEXT("route_profile"),Replacement);Changed=true;}
                    for(const auto& Pair:Object->Values)Visit(Pair.Value,Depth+1);}
            };
            Visit(MakeShared<FJsonValueObject>(Updated),0);
        }
        if(Changed){Before.Add(File,Original);After.Add(File,Updated);}
    }
    const FString IndexPath=Root/TEXT("import-index.json");
    if(Rename&&Section==TEXT("Loot"))
    {const FString File=Path(Root,Section,Name);auto Original=ReadSection(Root,Section,Name,Error);auto Updated=Copy(Original);
     Updated->SetStringField(TEXT("loot_profile"),Replacement);Before.Add(File,Original);After.Add(File,Updated);}
    if(auto Index=Read(IndexPath))
    {
        auto Updated=Copy(Index);bool Changed=false;
        for(auto& Pair:Updated->Values)if(Pair.Value->Type==EJson::Object)
        {auto Ref=Pair.Value->AsObject();FString Linked;if(Ref->TryGetStringField(Section,Linked)&&Linked==Name)
            {if(Rename)Ref->SetStringField(Section,Replacement);else Ref->RemoveField(Section);Changed=true;}}
        if(Changed){Before.Add(IndexPath,Index);After.Add(IndexPath,Updated);}
    }
    TArray<FString> Applied;
    auto Rollback = [&]() { for (const auto& File : Applied) Write(File, Before[File]); };
    for (const auto& Entry : After)
    {
        if (!Write(Entry.Key, Entry.Value)) { Rollback(); Error = TEXT("Could not update linked setups; profile retained."); return false; }
        Applied.Add(Entry.Key);
    }
    const FString Backup = Rename?Path(Root,Section,Replacement):Path(Root, TEXT("Deleted") / Section, Name + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Backup), true);
    if (!IFileManager::Get().Move(*Backup, *Path(Root, Section, Name), false, false))
    { Rollback(); Error = TEXT("Could not archive deleted profile; linked setups were restored."); return false; }
    return true;
}
bool DeleteSection(const FString& Root,const FString& Section,const FString& Name,FString& Error)
{return RelocateSection(Root,Section,Name,TEXT("Default"),false,Error);}
bool MigrateAnonymousImports(const FString& Root,FString& Error)
{
    for(const auto& Section:Sections())for(const auto& Name:List(Root,Section))
    {
        if(Name!=TEXT("Imported")&&!(Name.StartsWith(TEXT("Imported_"))&&Name.Mid(9).IsNumeric()))continue;
        auto Part=ReadSection(Root,Section,Name,Error),Default=ReadSection(Root,Section,TEXT("Default"),Error);
        if(!Part||!Default)return false;
        auto Content=Copy(Part),DefaultContent=Copy(Default);
        Content->RemoveField(TEXT("loot_profile"));DefaultContent->RemoveField(TEXT("loot_profile"));
        const bool Unchanged=Equal(Content,DefaultContent);
        FString Stem;for(const TCHAR* Key:{TEXT("usd_source"),TEXT("nav_source"),TEXT("utl_source"),TEXT("met_source")})
        {FString Source;if(Part->TryGetStringField(Key,Source)&&!Source.IsEmpty()){Stem=FPaths::GetBaseFilename(Source);break;}}
        if(Stem.IsEmpty())Stem=TEXT("Recovered_")+Section;
        if(!RelocateSection(Root,Section,Name,Unchanged?TEXT("Default"):UniqueName(Root,Section,Stem),!Unchanged,Error))return false;
    }
    return true;
}
}
