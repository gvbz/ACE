#include "Personal/PersonalDevService.h"

#include "Mods/ACEPluginSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/StringOutputDevice.h"

namespace PersonalDevServicePrivate
{
	constexpr int32 MaxNames = 128;
	constexpr int32 MaxNameLength = 128;
	constexpr int32 MaxTextLength = 500;
	constexpr int32 MaxHelpLength = 1024;

	bool IsCleanText(const FString& Text)
	{
		for (const TCHAR Ch : Text)
		{
			if (FChar::IsControl(Ch))
			{
				return false;
			}
		}
		return true;
	}

	/** The personal. console object of this name, or null. Lookup is case-insensitive, as in the console. */
	IConsoleObject* FindPersonal(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > MaxNameLength || !Name.StartsWith(PersonalDevService::Prefix, ESearchCase::IgnoreCase) || !IsCleanText(Name))
		{
			return nullptr;
		}
		for (const TCHAR Ch : Name)
		{
			if (FChar::IsWhitespace(Ch))
			{
				return nullptr;
			}
		}
		IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Name, false);
		return Object && !Object->TestFlags(ECVF_Unregistered) ? Object : nullptr;
	}

	IConsoleVariable* FindVariable(const FString& Name, FString& Error)
	{
		IConsoleObject* Object = FindPersonal(Name);
		IConsoleVariable* Variable = Object ? Object->AsVariable() : nullptr;
		if (!Variable)
		{
			Error = TEXT("Not a personal. console variable: ") + Name.Left(MaxNameLength);
		}
		return Variable;
	}

	FString TypeName(IConsoleVariable& Variable)
	{
		return Variable.IsVariableBool() ? TEXT("bool") : Variable.IsVariableInt() ? TEXT("int") : Variable.IsVariableFloat() ? TEXT("float") : TEXT("string");
	}

	TSharedPtr<FJsonValue> Value(IConsoleVariable& Variable)
	{
		if (Variable.IsVariableBool())
		{
			return MakeShared<FJsonValueBoolean>(Variable.GetBool());
		}
		if (Variable.IsVariableInt())
		{
			return MakeShared<FJsonValueNumber>(Variable.GetInt());
		}
		if (Variable.IsVariableFloat())
		{
			return MakeShared<FJsonValueNumber>(Variable.GetFloat());
		}
		return MakeShared<FJsonValueString>(Variable.GetString());
	}

	/** A JSON value as the text the console would get; false if it does not fit the variable's type. */
	bool ConsoleText(IConsoleVariable& Variable, const TSharedPtr<FJsonValue>& In, FString& Out)
	{
		if (!In.IsValid())
		{
			return false;
		}
		if (Variable.IsVariableString())
		{
			if (In->Type != EJson::String || In->AsString().Len() > 1024 || !IsCleanText(In->AsString()))
			{
				return false;
			}
			Out = In->AsString();
			return true;
		}
		if (In->Type == EJson::Boolean)
		{
			Out = In->AsBool() ? TEXT("1") : TEXT("0");
			return true;
		}
		if (In->Type != EJson::Number || !FMath::IsFinite(In->AsNumber()))
		{
			return false;
		}
		const double Number = In->AsNumber();
		if (Variable.IsVariableFloat())
		{
			Out = FString::SanitizeFloat(Number);
			return true;
		}
		if (Number != FMath::FloorToDouble(Number) || Number < MIN_int32 || Number > MAX_int32 || (Variable.IsVariableBool() && Number != 0 && Number != 1))
		{
			return false;
		}
		Out = FString::Printf(TEXT("%d"), int32(Number));
		return true;
	}

	bool List(const TSharedPtr<FJsonObject>& Reply)
	{
		// The visitor runs under the console manager's lock: collect first, read afterwards.
		TArray<TPair<FString, IConsoleObject*>> Found;
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda([&Found](const TCHAR* Name, IConsoleObject* Object)
		{
			if (Object && !Object->TestFlags(ECVF_Unregistered) && FCString::Strnicmp(Name, PersonalDevService::Prefix, FCString::Strlen(PersonalDevService::Prefix)) == 0)
			{
				Found.Emplace(Name, Object);
			}
		}), PersonalDevService::Prefix);
		Found.Sort([](const TPair<FString, IConsoleObject*>& A, const TPair<FString, IConsoleObject*>& B) { return A.Key < B.Key; });

		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const TPair<FString, IConsoleObject*>& Pair : Found)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Pair.Key);
			Entry->SetStringField(TEXT("help"), FString(Pair.Value->GetHelp()).Left(MaxHelpLength));
			if (IConsoleVariable* Variable = Pair.Value->AsVariable())
			{
				Entry->SetStringField(TEXT("kind"), TEXT("variable"));
				Entry->SetStringField(TEXT("type"), TypeName(*Variable));
				Entry->SetField(TEXT("value"), Value(*Variable));
				Entry->SetStringField(TEXT("default"), Variable->GetDefaultValue());
				Entry->SetBoolField(TEXT("read_only"), Variable->TestFlags(ECVF_ReadOnly));
			}
			else
			{
				Entry->SetStringField(TEXT("kind"), TEXT("command"));
			}
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Reply->SetArrayField(TEXT("entries"), Entries);
		return true;
	}

	bool Get(const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& Reply, FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (!Request->TryGetArrayField(TEXT("names"), Names) || Names->IsEmpty() || Names->Num() > MaxNames)
		{
			Error = TEXT("get needs names: an array of 1 to 128 personal. variable names");
			return false;
		}
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& Name : *Names)
		{
			const FString Text = Name.IsValid() && Name->Type == EJson::String ? Name->AsString() : FString();
			IConsoleVariable* Variable = FindVariable(Text, Error);
			if (!Variable)
			{
				return false;
			}
			Values->SetField(Text, Value(*Variable));
		}
		Reply->SetObjectField(TEXT("values"), Values);
		return true;
	}

	bool Set(const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& Reply, FString& Error)
	{
		const TSharedPtr<FJsonObject>* In = nullptr;
		if (!Request->TryGetObjectField(TEXT("values"), In) || (*In)->Values.IsEmpty() || (*In)->Values.Num() > MaxNames)
		{
			Error = TEXT("set needs values: an object of 1 to 128 personal. variable names and values");
			return false;
		}
		// Check every name and value first, so a request sets all of its variables or none.
		TArray<TPair<FString, IConsoleVariable*>> Checked;
		TArray<FString> Texts;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*In)->Values)
		{
			IConsoleVariable* Variable = FindVariable(Pair.Key, Error);
			if (!Variable)
			{
				return false;
			}
			FString Text;
			if (Variable->TestFlags(ECVF_ReadOnly) || !ConsoleText(*Variable, Pair.Value, Text))
			{
				Error = TEXT("Invalid value for ") + Pair.Key;
				return false;
			}
			Checked.Emplace(Pair.Key, Variable);
			Texts.Add(Text);
		}
		// Set as the console does, so a later console line or command file still applies.
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < Checked.Num(); ++Index)
		{
			Checked[Index].Value->Set(*Texts[Index], ECVF_SetByConsole);
			Values->SetField(Checked[Index].Key, Value(*Checked[Index].Value));
		}
		Reply->SetObjectField(TEXT("values"), Values);
		return true;
	}

	bool Run(const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& Reply, FString& Error, UWorld* World)
	{
		FString Text;
		Request->TryGetStringField(TEXT("text"), Text);
		Text.TrimStartAndEndInline();
		TArray<FString> Words;
		if (Text.Len() <= MaxTextLength && IsCleanText(Text))
		{
			Text.ParseIntoArrayWS(Words);
		}
		IConsoleObject* Object = Words.IsEmpty() ? nullptr : FindPersonal(Words[0]);
		IConsoleCommand* Command = Object ? Object->AsCommand() : nullptr;
		if (!Command)
		{
			Error = TEXT("run needs text: one personal. console command and its arguments");
			return false;
		}
		Words.RemoveAt(0);
		FStringOutputDevice Output;
		Output.SetAutoEmitLineTerminator(true);
		if (!Command->Execute(Words, World, Output))
		{
			Error = TEXT("The command did not run: ") + Text;
			return false;
		}
		Reply->SetStringField(TEXT("command"), Text);
		Reply->SetStringField(TEXT("output"), FString(Output).Left(4096));
		return true;
	}
}

bool PersonalDevService::Handle(const FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& Reply, FString& Error, UWorld* World)
{
	namespace Dev = PersonalDevServicePrivate;
	if (!Plugin.Enabled || !Plugin.Permissions.Contains(Permission))
	{
		Error = TEXT("Missing permission: dev_tools");
		return false;
	}
	if (!Request.IsValid() || !Reply.IsValid())
	{
		Error = TEXT("Invalid dev request");
		return false;
	}
	FString Operation;
	Request->TryGetStringField(TEXT("operation"), Operation);
	if (Operation == TEXT("list"))
	{
		return Dev::List(Reply);
	}
	if (Operation == TEXT("get"))
	{
		return Dev::Get(Request, Reply, Error);
	}
	if (Operation == TEXT("set"))
	{
		return Dev::Set(Request, Reply, Error);
	}
	if (Operation == TEXT("run"))
	{
		return Dev::Run(Request, Reply, Error, World);
	}
	Error = TEXT("Unknown dev operation");
	return false;
}
