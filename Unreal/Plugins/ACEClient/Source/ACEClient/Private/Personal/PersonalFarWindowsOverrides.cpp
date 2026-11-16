#include "Personal/PersonalFarWindowsOverrides.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** False for a word that is no kind; "rule" is a kind: no override (the rule decides), to undo a shipped line. */
	bool ParseKind(const FString& Word, PersonalFarWindowsOverrides::EOverride& Out)
	{
		if (Word.Equals(TEXT("door"), ESearchCase::IgnoreCase)) Out = PersonalFarWindowsOverrides::EOverride::Door;
		else if (Word.Equals(TEXT("open"), ESearchCase::IgnoreCase)) Out = PersonalFarWindowsOverrides::EOverride::Open;
		else if (Word.Equals(TEXT("rule"), ESearchCase::IgnoreCase)) Out = PersonalFarWindowsOverrides::EOverride::None;
		else return false;
		return true;
	}

	bool ParseHex(const FString& Word, uint32& Out)
	{
		FString Digits = Word;
		Digits.RemoveFromStart(TEXT("0x"), ESearchCase::IgnoreCase);
		if (Digits.IsEmpty() || Digits.Len() > 8)
		{
			return false;
		}
		for (const TCHAR C : Digits)
		{
			if (!FChar::IsHexDigit(C)) return false;
		}
		Out = FParse::HexNumber(*Digits);
		return true;
	}

	bool ParseInt(const FString& Word, int32& Out)
	{
		if (Word.IsEmpty() || !Word.IsNumeric())
		{
			return false;
		}
		Out = FCString::Atoi(*Word);
		return Out >= 0;
	}

	PersonalFarWindowsOverrides::FOverrides ReadFile(const FString& Path)
	{
		PersonalFarWindowsOverrides::FOverrides Overrides;
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			PersonalFarWindowsOverrides::Parse(Text, Overrides);
		}
		return Overrides;
	}

	struct FState
	{
		PersonalFarWindowsOverrides::FOverrides Shipped;
		PersonalFarWindowsOverrides::FOverrides Saved;
		PersonalFarWindowsOverrides::FOverrides Merged;
	};

	FState& Loaded()
	{
		static FState State;
		static bool bLoaded = false;
		if (!bLoaded)
		{
			bLoaded = true;
			State.Shipped = ReadFile(PersonalFarWindowsOverrides::ShippedPath());
			State.Saved = ReadFile(PersonalFarWindowsOverrides::FilePath());
			State.Merged = State.Shipped;
			PersonalFarWindowsOverrides::Merge(State.Saved, State.Merged);
			UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: overrides: %d shipped (%s), %d saved (%s), %d in use"), State.Shipped.Types.Num() + State.Shipped.Buildings.Num(),
				*PersonalFarWindowsOverrides::ShippedPath(), State.Saved.Types.Num() + State.Saved.Buildings.Num(), *PersonalFarWindowsOverrides::FilePath(),
				State.Merged.Types.Num() + State.Merged.Buildings.Num());
		}
		return State;
	}
}

void PersonalFarWindowsOverrides::Parse(const FString& Text, FOverrides& Out)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		TArray<FString> Words;
		Line.ParseIntoArrayWS(Words);
		uint32 Type = 0, Landblock = 0;
		int32 Order = 0, Building = 0;
		EOverride Kind = EOverride::None;
		if (Words.Num() == 3 && ParseHex(Words[0], Type) && ParseInt(Words[1], Order) && ParseKind(Words[2], Kind))
		{
			Out.Types.Add(TypeKey(Type, Order), Kind);
		}
		else if (Words.Num() == 5 && Words[0].Len() == 4 && ParseHex(Words[0], Landblock) && ParseInt(Words[1], Building) && ParseHex(Words[2], Type)
			&& ParseInt(Words[3], Order) && ParseKind(Words[4], Kind))
		{
			Out.Buildings.Add(BuildingKey(Landblock << 16, Building, Order), TPair<uint32, EOverride>(Type, Kind));
		}
	}
}

FString PersonalFarWindowsOverrides::Write(const FOverrides& Overrides)
{
	FString Text = TEXT("# Far windows overrides (Far Terrain Rooms). The client reads the list it ships with first, then this file, which wins.\n")
		TEXT("# personal.FarWindows.Mark door|open|clear changes this file. Building type, opening (its order in the model, as the survey numbers them),\n")
		TEXT("# door, open, or rule (no override: the rule decides, also for a shipped line):\n")
		TEXT("#   0x010014C3 1 open\n")
		TEXT("# One building (landblock, building number, type, opening, kind) wins over its type:\n")
		TEXT("#   7E65 0 0x010014C3 1 door\n");
	TArray<uint64> Keys;
	Overrides.Types.GetKeys(Keys);
	Keys.Sort();
	for (const uint64 Key : Keys)
	{
		Text += FString::Printf(TEXT("0x%08X %u %s\n"), uint32(Key >> 32), uint32(Key), Name(Overrides.Types[Key]));
	}
	Keys.Reset();
	Overrides.Buildings.GetKeys(Keys);
	Keys.Sort();
	for (const uint64 Key : Keys)
	{
		const TPair<uint32, EOverride>& Entry = Overrides.Buildings[Key];
		Text += FString::Printf(TEXT("%04X %u 0x%08X %u %s\n"), uint32(Key >> 48), uint32((Key >> 32) & 0xFFFFu), Entry.Key, uint32(Key), Name(Entry.Value));
	}
	return Text;
}

void PersonalFarWindowsOverrides::Merge(const FOverrides& Over, FOverrides& InOut)
{
	InOut.Types.Append(Over.Types);
	InOut.Buildings.Append(Over.Buildings);
}

PersonalFarWindowsOverrides::EOverride PersonalFarWindowsOverrides::Find(const FOverrides& Overrides, uint32 ModelId, uint32 LandblockKey, int32 Building, int32 Order)
{
	if (const TPair<uint32, EOverride>* One = Overrides.Buildings.Find(BuildingKey(LandblockKey, Building, Order)))
	{
		if (One->Key == ModelId)
		{
			return One->Value;
		}
	}
	const EOverride* Type = Overrides.Types.Find(TypeKey(ModelId, Order));
	return Type ? *Type : EOverride::None;
}

void PersonalFarWindowsOverrides::Apply(const FOverrides& Overrides, uint32 ModelId, uint32 LandblockKey, int32 Building, TArray<PersonalFarWindows::EKind>& Kinds)
{
	if (Overrides.Types.Num() == 0 && Overrides.Buildings.Num() == 0)
	{
		return;
	}
	for (int32 Order = 0; Order < Kinds.Num(); ++Order)
	{
		const EOverride Override = Find(Overrides, ModelId, LandblockKey, Building, Order);
		if (Override == EOverride::Door)
		{
			Kinds[Order] = PersonalFarWindows::EKind::Door;
		}
		else if (Override == EOverride::Open && Kinds[Order] == PersonalFarWindows::EKind::Door)
		{
			Kinds[Order] = PersonalFarWindows::EKind::Entrance;
		}
	}
}

const PersonalFarWindowsOverrides::FOverrides& PersonalFarWindowsOverrides::Get()
{
	return Loaded().Merged;
}

bool PersonalFarWindowsOverrides::SetForType(uint32 ModelId, TConstArrayView<int32> Orders, EOverride Override)
{
	FState& State = Loaded();
	// The file as it is now, so hand edits made while the client runs are kept.
	if (FPaths::FileExists(FilePath()))
	{
		State.Saved = ReadFile(FilePath());
	}
	for (const int32 Order : Orders)
	{
		const uint64 Key = TypeKey(ModelId, Order);
		if (Override != EOverride::None)
		{
			State.Saved.Types.Add(Key, Override);
		}
		else if (State.Shipped.Types.Contains(Key))
		{
			// Clear of a shipped line: a rule line, which wins over it.
			State.Saved.Types.Add(Key, EOverride::None);
		}
		else
		{
			State.Saved.Types.Remove(Key);
		}
	}
	State.Merged = State.Shipped;
	Merge(State.Saved, State.Merged);
	return FFileHelper::SaveStringToFile(Write(State.Saved), *FilePath());
}

FString PersonalFarWindowsOverrides::FilePath()
{
	return FPaths::ProjectSavedDir() / TEXT("Personal/FarWindowsOverrides.txt");
}

FString PersonalFarWindowsOverrides::ShippedPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
	return Plugin ? Plugin->GetBaseDir() / TEXT("Data/Personal/FarWindowsOverrides.txt") : FString();
}

const TCHAR* PersonalFarWindowsOverrides::Name(EOverride Override)
{
	return Override == EOverride::Door ? TEXT("door") : (Override == EOverride::Open ? TEXT("open") : TEXT("rule"));
}
