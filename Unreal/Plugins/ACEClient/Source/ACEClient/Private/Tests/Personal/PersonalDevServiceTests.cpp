#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Personal/PersonalDevService.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalDevServiceTest, "ACE.Personal.DevService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalDevServiceTest::RunTest(const FString& Parameters)
{
	auto Json = [](const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		return Object;
	};
	FACEClientPlugin Plugin;
	Plugin.Id = TEXT("devtest");
	Plugin.Enabled = true;
	Plugin.Running = true;
	Plugin.Permissions = {FString(TEXT("ui")), FString(TEXT("dev_tools"))};
	TSharedPtr<FJsonObject> Reply;
	FString Error;
	auto Call = [&](const TCHAR* Request)
	{
		Reply = MakeShared<FJsonObject>();
		Error.Reset();
		return PersonalDevService::Handle(Plugin, Json(Request), Reply, Error, nullptr);
	};

	// Two commands of this test only: one inside personal., one outside.
	int32 InsideRuns = 0, OutsideRuns = 0;
	FString InsideArgs;
	IConsoleObject* Inside = IConsoleManager::Get().RegisterConsoleCommand(TEXT("personal.DevServiceTest.Run"), TEXT("Test command."),
		FConsoleCommandWithArgsDelegate::CreateLambda([&](const TArray<FString>& Args) { ++InsideRuns; InsideArgs = FString::Join(Args, TEXT(" ")); }), ECVF_Default);
	IConsoleObject* Outside = IConsoleManager::Get().RegisterConsoleCommand(TEXT("DevServiceTest.Outside"), TEXT("Test command."),
		FConsoleCommandWithArgsDelegate::CreateLambda([&](const TArray<FString>&) { ++OutsideRuns; }), ECVF_Default);
	ON_SCOPE_EXIT
	{
		IConsoleManager::Get().UnregisterConsoleObject(Inside, false);
		IConsoleManager::Get().UnregisterConsoleObject(Outside, false);
	};

	IConsoleVariable* SizeFactor = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.SizeFactor"));
	IConsoleVariable* Freeze = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarTerrain.Freeze"));
	IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	if (!TestNotNull(TEXT("Size factor variable"), SizeFactor) || !TestNotNull(TEXT("Freeze variable"), Freeze) || !TestNotNull(TEXT("Screen percentage variable"), ScreenPercentage))
	{
		return false;
	}
	const float SavedSize = SizeFactor->GetFloat();
	const int32 SavedFreeze = Freeze->GetInt();
	const float SavedPercentage = ScreenPercentage->GetFloat();
	const uint32 SavedSizeFlags = uint32(SizeFactor->GetFlags()) & ECVF_SetByMask;
	const uint32 SavedFreezeFlags = uint32(Freeze->GetFlags()) & ECVF_SetByMask;
	// The service sets as the console does. Put back the values and their set-by priority, so later
	// tests can still set these variables from code.
	auto Restore = [](IConsoleVariable* Variable, const FString& Value, uint32 SetBy)
	{
		Variable->Set(*Value, ECVF_SetByConsole);
		Variable->SetFlags(EConsoleVariableFlags((uint32(Variable->GetFlags()) & ~uint32(ECVF_SetByMask)) | SetBy));
	};
	ON_SCOPE_EXIT
	{
		Restore(SizeFactor, FString::SanitizeFloat(SavedSize), SavedSizeFlags);
		Restore(Freeze, FString::FromInt(SavedFreeze), SavedFreezeFlags);
	};

	// list
	if (TestTrue(TEXT("list works"), Call(TEXT(R"({"operation":"list"})"))))
	{
		bool AllPersonal = true, SizeFound = false, CommandFound = false, OutsideListed = false;
		for (const TSharedPtr<FJsonValue>& Value : Reply->GetArrayField(TEXT("entries")))
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const FString Name = Entry->GetStringField(TEXT("name"));
			AllPersonal &= Name.StartsWith(TEXT("personal."), ESearchCase::IgnoreCase);
			OutsideListed |= Name.Contains(TEXT("Outside"));
			if (Name == TEXT("personal.FarScenery.SizeFactor"))
			{
				SizeFound = Entry->GetStringField(TEXT("kind")) == TEXT("variable") && Entry->GetStringField(TEXT("type")) == TEXT("float")
					&& Entry->HasTypedField<EJson::Number>(TEXT("value")) && !Entry->GetStringField(TEXT("help")).IsEmpty();
			}
			if (Name == TEXT("personal.FarScenery.LogFrameRate"))
			{
				CommandFound = Entry->GetStringField(TEXT("kind")) == TEXT("command") && !Entry->HasField(TEXT("value"));
			}
		}
		TestTrue(TEXT("list has only personal. names"), AllPersonal);
		TestFalse(TEXT("list leaves out a command outside personal."), OutsideListed);
		TestTrue(TEXT("list gives a variable with kind, type, value and help"), SizeFound);
		TestTrue(TEXT("list gives a command as a command"), CommandFound);
	}

	// Names outside personal. are refused.
	TestFalse(TEXT("get refuses a name outside personal."), Call(TEXT(R"({"operation":"get","names":["r.ScreenPercentage"]})")));
	TestTrue(TEXT("The refusal has an error"), !Error.IsEmpty());
	TestFalse(TEXT("get refuses a command"), Call(TEXT(R"({"operation":"get","names":["personal.FarScenery.LogFrameRate"]})")));
	TestFalse(TEXT("set refuses a name outside personal."), Call(TEXT(R"({"operation":"set","values":{"r.ScreenPercentage":10}})")));
	TestEqual(TEXT("A refused set changes nothing"), ScreenPercentage->GetFloat(), SavedPercentage);
	TestFalse(TEXT("set refuses a command"), Call(TEXT(R"({"operation":"set","values":{"personal.DevServiceTest.Run":1}})")));
	TestFalse(TEXT("set is all or nothing"), Call(TEXT(R"({"operation":"set","values":{"personal.FarScenery.SizeFactor":123,"r.ScreenPercentage":10}})")));
	TestEqual(TEXT("A refused set leaves the personal. variable too"), SizeFactor->GetFloat(), SavedSize);
	TestFalse(TEXT("set refuses a wrong type"), Call(TEXT(R"({"operation":"set","values":{"personal.FarTerrain.Freeze":"yes"}})")));
	TestFalse(TEXT("run refuses a command outside personal."), Call(TEXT(R"({"operation":"run","text":"DevServiceTest.Outside"})")));
	TestFalse(TEXT("run refuses a variable"), Call(TEXT(R"({"operation":"run","text":"personal.FarScenery.SizeFactor 5"})")));
	TestFalse(TEXT("run refuses a second line"), Call(TEXT(R"({"operation":"run","text":"personal.DevServiceTest.Run a\nDevServiceTest.Outside"})")));
	TestEqual(TEXT("No refused command ran"), InsideRuns + OutsideRuns, 0);

	// set, then get
	if (TestTrue(TEXT("set works"), Call(TEXT(R"({"operation":"set","values":{"personal.FarScenery.SizeFactor":250.5,"personal.FarTerrain.Freeze":true}})"))))
	{
		const TSharedPtr<FJsonObject> Values = Reply->GetObjectField(TEXT("values"));
		TestEqual(TEXT("set returns the new float value"), Values->GetNumberField(TEXT("personal.FarScenery.SizeFactor")), 250.5);
		TestEqual(TEXT("set returns the new int value"), Values->GetNumberField(TEXT("personal.FarTerrain.Freeze")), 1.);
	}
	if (TestTrue(TEXT("get works"), Call(TEXT(R"({"operation":"get","names":["personal.FarScenery.SizeFactor","personal.FarTerrain.Freeze"]})"))))
	{
		const TSharedPtr<FJsonObject> Values = Reply->GetObjectField(TEXT("values"));
		TestEqual(TEXT("get returns the value set"), Values->GetNumberField(TEXT("personal.FarScenery.SizeFactor")), 250.5);
		TestEqual(TEXT("get returns the second value set"), Values->GetNumberField(TEXT("personal.FarTerrain.Freeze")), 1.);
	}
	TestEqual(TEXT("The client uses the value set"), SizeFactor->GetFloat(), 250.5f);
	TestTrue(TEXT("set puts a value back"), Call(*FString::Printf(TEXT(R"({"operation":"set","values":{"personal.FarScenery.SizeFactor":%s,"personal.FarTerrain.Freeze":%d}})"),
		*FString::SanitizeFloat(SavedSize), SavedFreeze)));
	TestEqual(TEXT("The value is back"), SizeFactor->GetFloat(), SavedSize);

	// run
	TestTrue(TEXT("run runs a personal. command"), Call(TEXT(R"({"operation":"run","text":"personal.DevServiceTest.Run door now"})")));
	TestEqual(TEXT("The command ran once"), InsideRuns, 1);
	TestEqual(TEXT("The command got its arguments"), InsideArgs, FString(TEXT("door now")));
	TestEqual(TEXT("run returns the command"), Reply->GetStringField(TEXT("command")), FString(TEXT("personal.DevServiceTest.Run door now")));

	// Permission
	Plugin.Permissions = {FString(TEXT("ui"))};
	TestFalse(TEXT("A plugin without dev_tools cannot list"), Call(TEXT(R"({"operation":"list"})")));
	TestEqual(TEXT("The refusal names the permission"), Error, FString(TEXT("Missing permission: dev_tools")));
	TestFalse(TEXT("A plugin without dev_tools cannot set"), Call(TEXT(R"({"operation":"set","values":{"personal.FarScenery.SizeFactor":123}})")));
	TestFalse(TEXT("A plugin without dev_tools cannot run"), Call(TEXT(R"({"operation":"run","text":"personal.DevServiceTest.Run"})")));
	TestEqual(TEXT("Nothing changed without the permission"), SizeFactor->GetFloat(), SavedSize);
	TestEqual(TEXT("Nothing ran without the permission"), InsideRuns, 1);
	Plugin.Permissions = {FString(TEXT("ui")), FString(TEXT("dev_tools"))};
	Plugin.Enabled = false;
	TestFalse(TEXT("A disabled plugin cannot use the service"), Call(TEXT(R"({"operation":"list"})")));
	Plugin.Enabled = true;
	TestFalse(TEXT("An unknown operation is refused"), Call(TEXT(R"({"operation":"exec","text":"personal.DevServiceTest.Run"})")));

	// After the restore, code can set the variable again (the console priority is gone).
	Restore(SizeFactor, FString::SanitizeFloat(SavedSize), SavedSizeFlags);
	SizeFactor->Set(SavedSize + 1.f);
	TestEqual(TEXT("After the test, code can set the variable"), SizeFactor->GetFloat(), SavedSize + 1.f);
	Restore(SizeFactor, FString::SanitizeFloat(SavedSize), SavedSizeFlags);
	return true;
}
#endif
