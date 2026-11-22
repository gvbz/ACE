#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Personal/PersonalDevService.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Runs the bundled Development Dashboard (ClientMods/devdash) in the plugin VM against the dev service.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalDevDashboardTest, "ACE.Personal.DevDashboard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalDevDashboardTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Client = IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
	if (!TestTrue(TEXT("ACEClient plugin"), Client.IsValid()))
	{
		return false;
	}
	const FString Folder = Client->GetBaseDir() / TEXT("ClientMods/devdash");
	FString ManifestText, Source, Error;
	if (!TestTrue(TEXT("Manifest exists"), FFileHelper::LoadFileToString(ManifestText, *(Folder / TEXT("plugin.json"))))
		|| !TestTrue(TEXT("Lua exists"), FFileHelper::LoadFileToString(Source, *(Folder / TEXT("main.lua")))))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Manifest;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), Manifest);
	if (!TestTrue(TEXT("Manifest is JSON"), Manifest.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Manifest id"), Manifest->GetStringField(TEXT("id")), FString(TEXT("devdash")));
	TestEqual(TEXT("Manifest mode"), Manifest->GetStringField(TEXT("mode")), FString(TEXT("companion")));
	TArray<FString> Permissions;
	Manifest->TryGetStringArrayField(TEXT("permissions"), Permissions);
	TestEqual(TEXT("Permissions are ui, dev_tools, say, state and chat"), Permissions, TArray<FString>{TEXT("ui"), TEXT("dev_tools"), TEXT("say"), TEXT("state"), TEXT("chat")});
	TArray<FString> Fields;
	Manifest->TryGetStringArrayField(TEXT("snapshot_fields"), Fields);
	TestEqual(TEXT("Snapshot fields position and chat_events"), Fields, TArray<FString>{TEXT("position"), TEXT("chat_events")});
	const TArray<TSharedPtr<FJsonValue>>* Subscriptions = nullptr;
	if (TestTrue(TEXT("One chat subscription"), Manifest->TryGetArrayField(TEXT("chat_subscriptions"), Subscriptions) && Subscriptions->Num() == 1))
	{
		const TSharedPtr<FJsonObject> Rule = (*Subscriptions)[0]->AsObject();
		TestEqual(TEXT("It matches the place list"), Rule->GetStringField(TEXT("prefix")), FString(TEXT("All POIs: ")));
		TestEqual(TEXT("From the server"), Rule->GetStringField(TEXT("sender_type")), FString(TEXT("server")));
		TestEqual(TEXT("As a Broadcast line"), Rule->GetNumberField(TEXT("chat_type")), 0.);
	}

	// Every variable the dashboard can set: values and set-by priority are put back at the end.
	const TArray<FString> FarNames = {
		TEXT("personal.FarTerrain.Wireframe"), TEXT("personal.FarTerrain.Freeze"), TEXT("personal.FarWindows.Kinds"),
		TEXT("personal.FarScenery.Fade"), TEXT("personal.FarScenery.SizeFactor"), TEXT("personal.FarScenery.Detail"),
		TEXT("personal.FarScenery.MiddleRing"), TEXT("personal.FarScenery.SimplestRing"), TEXT("personal.FarScenery.Group"),
		TEXT("personal.FarScenery.BudgetMs"), TEXT("personal.FarScenery.Radius"), TEXT("personal.FarWindows.Rooms"),
		TEXT("personal.FarWindows.Draw"), TEXT("personal.FarWindows.RoomFogNear"), TEXT("personal.FarWindows.RoomFogFar"),
		TEXT("personal.FarWindows.RoomLift"), TEXT("personal.FarWindows.DoorDistance"), TEXT("personal.FarWindows.RoomsAlways")};
	// The Fog tab's 49 variables (personal.FogColor.*), after the 18 above.
	TArray<FString> FogNames = {TEXT("personal.FogColor.Draw"), TEXT("personal.FogColor.Strength"), TEXT("personal.FogColor.Saturation"),
		TEXT("personal.FogColor.Window"), TEXT("personal.FogColor.StormFade"), TEXT("personal.FogColor.Variation"), TEXT("personal.FogColor.DayOffset"),
		TEXT("personal.FogColor.Weather"), TEXT("personal.FogColor.Set"), TEXT("personal.FogColor.Column"),
		TEXT("personal.FogColor.TableBrightness"), TEXT("personal.FogColor.SplitHaze"), TEXT("personal.FogColor.HazeColumn")};
	for (const TCHAR* Weather : {TEXT("Clear"), TEXT("Sunny"), TEXT("Cloudy"), TEXT("Rainy")})
	{
		for (const TCHAR* Field : {TEXT("Table"), TEXT("SunrisePalette"), TEXT("SunsetPalette"), TEXT("SunriseHue"), TEXT("SunriseSat"),
			TEXT("SunsetHue"), TEXT("SunsetSat"), TEXT("SunriseBright"), TEXT("SunsetBright")})
		{
			FogNames.Add(FString::Printf(TEXT("personal.FogColor.%s.%s"), Weather, Field));
		}
	}
	// The Lighting tab's three variables (personal.LightDir.*).
	const TArray<FString> LightNames = {TEXT("personal.LightDir.Smooth"), TEXT("personal.LightDir.HorizonFade"), TEXT("personal.LightDir.NightTurn")};
	TArray<FString> Names = FarNames;
	Names.Append(FogNames);
	Names.Append(LightNames);
	struct FSaved { IConsoleVariable* Variable; FString Value; uint32 SetBy; };
	TArray<FSaved> Saved;
	for (const FString& Name : Names)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!TestNotNull(*(TEXT("Variable ") + Name), Variable))
		{
			return false;
		}
		Saved.Add({Variable, Variable->GetString(), uint32(Variable->GetFlags()) & ECVF_SetByMask});
	}
	ON_SCOPE_EXIT
	{
		for (const FSaved& S : Saved)
		{
			S.Variable->Set(*S.Value, ECVF_SetByConsole);
			S.Variable->SetFlags(EConsoleVariableFlags((uint32(S.Variable->GetFlags()) & ~uint32(ECVF_SetByMask)) | S.SetBy));
		}
	};
	// The Fog and Lighting variables start at their defaults, as in a fresh client.
	TArray<FString> FreshNames = FogNames;
	FreshNames.Append(LightNames);
	for (const FString& Name : FreshNames)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
		Variable->Set(*Variable->GetDefaultValue(), ECVF_SetByConsole);
	}
	// Current as the client writes it with Saturation following the option at 0.35 (read only: set by code, put back at the end).
	IConsoleVariable* CurrentVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FogColor.Current"));
	if (!TestNotNull(TEXT("Variable personal.FogColor.Current"), CurrentVariable))
	{
		return false;
	}
	const FString SavedCurrent = CurrentVariable->GetString();
	ON_SCOPE_EXIT { CurrentVariable->Set(*SavedCurrent, ECVF_SetByCode); };
	CurrentVariable->Set(TEXT("On. Clear (server), day 77 (+0). Atmospheric tables, Horizon, Clear sky, band Mid-sky; saturation 0.35 (option). Variation 0.50: outside the windows. Weights: sunrise 0.00, sunset 0.00; rain 0.00, overcast 0.00; amount 0.00."), ECVF_SetByCode);
	// The Lighting tab's personal.LightDir.Current, the same way.
	IConsoleVariable* LightCurrentVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.LightDir.Current"));
	if (!TestNotNull(TEXT("Variable personal.LightDir.Current"), LightCurrentVariable))
	{
		return false;
	}
	const FString SavedLightCurrent = LightCurrentVariable->GetString();
	ON_SCOPE_EXIT { LightCurrentVariable->Set(*SavedLightCurrent, ECVF_SetByCode); };
	LightCurrentVariable->Set(TEXT("On. Moon 0.42, sun 0.00. Light 8.3° up, bearing 12°."), ECVF_SetByCode);

	FACEClientPlugin Plugin;
	Plugin.Id = TEXT("devdash");
	Plugin.Enabled = true;
	Plugin.Running = true;
	Plugin.Permissions = {FString(TEXT("ui")), FString(TEXT("dev_tools"))};

	FACEPluginVM VM;
	if (!TestTrue(TEXT("Dashboard Lua loads"), VM.Load(Source, Error)))
	{
		AddError(Error);
		return false;
	}
	FACEPluginVM* Active = &VM;
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>(), Profile = MakeShared<FJsonObject>(), Out;
	Input->SetNumberField(TEXT("player"), 1234);
	Input->SetObjectField(TEXT("state"), MakeShared<FJsonObject>());
	double Clock = 0;
	// One callback, Delta seconds after the last. A returned state is kept, as the host does.
	auto Step = [&](const TArray<TSharedPtr<FJsonValue>>& Events, double Delta = 1.)
	{
		Input->SetNumberField(TEXT("time"), Clock += Delta);
		Input->SetArrayField(TEXT("events"), Events);
		Out.Reset();
		if (!Active->Step(Input, Profile, Out, Error) || !Out.IsValid())
		{
			AddError(TEXT("Dashboard Lua: ") + Error);
			return false;
		}
		const TSharedPtr<FJsonObject>* State = nullptr;
		if (Out->TryGetObjectField(TEXT("state"), State))
		{
			Input->SetObjectField(TEXT("state"), *State);
		}
		return true;
	};
	// The Fog and Lighting tabs read personal.FogColor.Current and personal.LightDir.Current every two seconds, in one get;
	// Requests leaves those gets out, AllRequests has them.
	const FString CurrentName = TEXT("personal.FogColor.Current");
	const FString LightCurrentName = TEXT("personal.LightDir.Current");
	auto IsPoll = [&](const TSharedPtr<FJsonObject>& R)
	{
		const TArray<TSharedPtr<FJsonValue>>* PollNames = nullptr;
		if (R->GetStringField(TEXT("operation")) != TEXT("get") || !R->TryGetArrayField(TEXT("names"), PollNames) || PollNames->Num() == 0)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& PollName : *PollNames)
		{
			if (PollName->AsString() != CurrentName && PollName->AsString() != LightCurrentName)
			{
				return false;
			}
		}
		return true;
	};
	auto AllRequests = [&]()
	{
		TArray<TSharedPtr<FJsonObject>> List;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Out->TryGetArrayField(TEXT("requests"), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				List.Add(Value->AsObject());
			}
		}
		return List;
	};
	auto Requests = [&]()
	{
		TArray<TSharedPtr<FJsonObject>> List = AllRequests();
		List.RemoveAll([&](const TSharedPtr<FJsonObject>& R) { return IsPoll(R); });
		return List;
	};
	// Runs a request through the dev service and makes the result event the host would queue.
	auto Serve = [&](const TSharedPtr<FJsonObject>& Request) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
		FString Failure;
		const bool bOk = PersonalDevService::Handle(Plugin, Request, Reply, Failure, nullptr);
		Reply->SetStringField(TEXT("type"), TEXT("result"));
		Reply->SetStringField(TEXT("id"), Request->GetStringField(TEXT("id")));
		Reply->SetBoolField(TEXT("ok"), bOk);
		Reply->SetStringField(TEXT("error"), Failure);
		return MakeShared<FJsonValueObject>(Reply);
	};
	auto Event = [](const TCHAR* Type, const TCHAR* Id, const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), Type);
		E->SetStringField(TEXT("id"), Id);
		if (Value.IsValid())
		{
			E->SetField(TEXT("value"), Value);
		}
		return MakeShared<FJsonValueObject>(E);
	};
	// The UI as the host checks it: unique IDs, at most 256 controls, eight levels, known types.
	TMap<FString, TSharedPtr<FJsonObject>> Controls;
	int32 Count = 0;
	auto ReadUI = [&]()
	{
		Controls.Reset();
		Count = 0;
		bool bValid = true;
		TFunction<void(const TArray<TSharedPtr<FJsonValue>>&, int32)> Visit = [&](const TArray<TSharedPtr<FJsonValue>>& Rows, int32 Depth)
		{
			bValid &= Depth <= 8;
			for (const TSharedPtr<FJsonValue>& Row : Rows)
			{
				const TSharedPtr<FJsonObject> R = Row->AsObject();
				const FString Id = R->GetStringField(TEXT("id"));
				FString Type = TEXT("text");
				R->TryGetStringField(TEXT("type"), Type);
				bValid &= ++Count <= 256 && !Id.IsEmpty() && Id.Len() <= 64 && !Controls.Contains(Id)
					&& TArray<FString>{TEXT("text"), TEXT("button"), TEXT("checkbox"), TEXT("slider"), TEXT("select"), TEXT("input"), TEXT("row"), TEXT("column"), TEXT("section"), TEXT("tabs")}.Contains(Type);
				Controls.Add(Id, R);
				const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
				if (R->TryGetArrayField(TEXT("children"), Children))
				{
					Visit(*Children, Depth + 1);
				}
			}
		};
		const TArray<TSharedPtr<FJsonValue>>* UI = nullptr;
		if (!Out->TryGetArrayField(TEXT("ui"), UI))
		{
			return false;
		}
		Visit(*UI, 0);
		return bValid;
	};
	auto Enabled = [&](const TCHAR* Id)
	{
		const TSharedPtr<FJsonObject>* C = Controls.Find(Id);
		bool bEnabled = true;
		return C && (*C)->TryGetBoolField(TEXT("enabled"), bEnabled) ? bEnabled : (C != nullptr);
	};

	// Start: one list request, the controls disabled until it is answered.
	if (!Step({}))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonObject>> Sent = Requests();
	if (!TestEqual(TEXT("Start sends one request"), Sent.Num(), 1) || !TestEqual(TEXT("It is list"), Sent[0]->GetStringField(TEXT("operation")), FString(TEXT("list"))))
	{
		return false;
	}
	TestEqual(TEXT("It goes to the dev service"), Sent[0]->GetStringField(TEXT("service")), FString(TEXT("dev")));
	TestTrue(TEXT("The UI is valid"), ReadUI());
	TestFalse(TEXT("Controls wait for the client's values"), Enabled(TEXT("fs_size")));
	for (const TCHAR* Id : {TEXT("status"), TEXT("tabs"), TEXT("far_terrain"), TEXT("far_scenery"), TEXT("far_rooms"), TEXT("fog"), TEXT("lighting"), TEXT("shadows"), TEXT("performance"), TEXT("log_frame_rate")})
	{
		TestTrue(*FString::Printf(TEXT("Control %s"), Id), Controls.Contains(Id));
	}

	// The list reply, with personal.FarTerrain.Freeze left out: it is shown disabled.
	TSharedPtr<FJsonValue> ListReply = Serve(Sent[0]);
	TArray<TSharedPtr<FJsonValue>> Entries = ListReply->AsObject()->GetArrayField(TEXT("entries"));
	Entries.RemoveAll([](const TSharedPtr<FJsonValue>& V) { return V->AsObject()->GetStringField(TEXT("name")) == TEXT("personal.FarTerrain.Freeze"); });
	ListReply->AsObject()->SetArrayField(TEXT("entries"), Entries);
	if (!Step({ListReply}))
	{
		return false;
	}
	TestTrue(TEXT("The UI is valid after list"), ReadUI());
	TestEqual(TEXT("Nothing is sent without a change"), Requests().Num(), 0);
	TestTrue(TEXT("Size factor is enabled"), Enabled(TEXT("fs_size")));
	TestFalse(TEXT("A variable the client does not have is disabled"), Enabled(TEXT("ft_freeze")));
	TestTrue(TEXT("The status names the missing variable"), Controls[TEXT("status")]->GetStringField(TEXT("text")).Contains(TEXT("personal.FarTerrain.Freeze")));
	IConsoleVariable* SizeFactor = Saved[4].Variable;
	TestEqual(TEXT("Size factor shows the client's value"), Controls[TEXT("fs_size")]->GetNumberField(TEXT("value")), double(FMath::Clamp(SizeFactor->GetFloat(), 1.f, 2000.f)));
	TestEqual(TEXT("Tooltip is the help text"), Controls[TEXT("fs_size")]->GetStringField(TEXT("tooltip")), FString(SizeFactor->GetHelp()));
	TestEqual(TEXT("Rooms is a select"), Controls[TEXT("fr_rooms")]->GetStringField(TEXT("type")), FString(TEXT("select")));
	TestTrue(TEXT("Mark door is enabled"), Enabled(TEXT("fr_mark_door")));

	// Changes in one callback go in one set.
	if (!Step({Event(TEXT("change"), TEXT("fs_size"), MakeShared<FJsonValueNumber>(250)),
		Event(TEXT("change"), TEXT("fs_fade"), MakeShared<FJsonValueBoolean>(false)),
		Event(TEXT("change"), TEXT("fr_rooms"), MakeShared<FJsonValueString>(TEXT("On"))),
		Event(TEXT("change"), TEXT("ft_freeze"), MakeShared<FJsonValueBoolean>(true))}))
	{
		return false;
	}
	Sent = Requests();
	if (!TestEqual(TEXT("One request for the changes"), Sent.Num(), 1) || !TestEqual(TEXT("It is set"), Sent[0]->GetStringField(TEXT("operation")), FString(TEXT("set"))))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> SetValues = Sent[0]->GetObjectField(TEXT("values"));
	TestEqual(TEXT("Three values, none for the missing variable"), SetValues->Values.Num(), 3);
	TestEqual(TEXT("Size factor value"), SetValues->GetNumberField(TEXT("personal.FarScenery.SizeFactor")), 250.);
	TestEqual(TEXT("Fade value"), SetValues->GetNumberField(TEXT("personal.FarScenery.Fade")), 0.);
	TestEqual(TEXT("Rooms On is 1"), SetValues->GetNumberField(TEXT("personal.FarWindows.Rooms")), 1.);

	// A rate-limited set is sent again.
	{
		TSharedPtr<FJsonObject> Limited = MakeShared<FJsonObject>();
		Limited->SetStringField(TEXT("type"), TEXT("result"));
		Limited->SetStringField(TEXT("id"), Sent[0]->GetStringField(TEXT("id")));
		Limited->SetBoolField(TEXT("ok"), false);
		Limited->SetStringField(TEXT("error"), TEXT("Service rate limit; retry after 0.5 seconds"));
		if (!Step({MakeShared<FJsonValueObject>(Limited)}))
		{
			return false;
		}
		Sent = Requests();
		if (!TestEqual(TEXT("The limited set is sent again"), Sent.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("With the same three values"), Sent[0]->GetObjectField(TEXT("values"))->Values.Num(), 3);
	}
	const TSharedPtr<FJsonValue> SetReply = Serve(Sent[0]);
	TestTrue(TEXT("The set is accepted"), SetReply->AsObject()->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("The client uses the new size factor"), SizeFactor->GetFloat(), 250.f);
	TestEqual(TEXT("The client uses the new fade"), Saved[3].Variable->GetInt(), 0);
	TestEqual(TEXT("The client uses the new rooms"), Saved[11].Variable->GetInt(), 1);
	if (!Step({SetReply}))
	{
		return false;
	}
	ReadUI();
	TestEqual(TEXT("The slider shows the new value"), Controls[TEXT("fs_size")]->GetNumberField(TEXT("value")), 250.);
	TestEqual(TEXT("The select shows On"), Controls[TEXT("fr_rooms")]->GetStringField(TEXT("value")), FString(TEXT("On")));

	// Buttons run their commands (not run here: the test has no world).
	if (!Step({Event(TEXT("click"), TEXT("fr_mark_door"), nullptr), Event(TEXT("click"), TEXT("log_frame_rate"), nullptr)}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("Two run requests"), Sent.Num(), 2))
	{
		TestEqual(TEXT("Mark door"), Sent[0]->GetStringField(TEXT("text")), FString(TEXT("personal.FarWindows.Mark door")));
		TestEqual(TEXT("Log frame rate"), Sent[1]->GetStringField(TEXT("text")), FString(TEXT("personal.FarScenery.LogFrameRate")));
	}

	// Reset tab puts the tab's variables back to the defaults the client reports.
	if (!Step({Event(TEXT("click"), TEXT("reset_far_scenery"), nullptr), Event(TEXT("click"), TEXT("reset_far_rooms"), nullptr)}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("Reset sends one set"), Sent.Num(), 1))
	{
		TestEqual(TEXT("Every variable of the two tabs"), Sent[0]->GetObjectField(TEXT("values"))->Values.Num(), 16);
		TestTrue(TEXT("The reset is accepted"), Serve(Sent[0])->AsObject()->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("Size factor is back to 400"), SizeFactor->GetFloat(), 400.f);
		TestEqual(TEXT("Fade is back to 1"), Saved[3].Variable->GetInt(), 1);
		TestEqual(TEXT("Rooms is back to -1"), Saved[11].Variable->GetInt(), -1);
		TestEqual(TEXT("Room fog far is back to 600"), Saved[14].Variable->GetFloat(), 600.f);
	}

	// Fog tab: the status line from personal.FogColor.Current, read every two seconds.
	auto FogVariable = [&](const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name); };
	{
		TSharedPtr<FJsonObject> Poll;
		for (int32 I = 0; I < 3 && !Poll.IsValid(); ++I)
		{
			if (!Step({}))
			{
				return false;
			}
			for (const TSharedPtr<FJsonObject>& R : AllRequests())
			{
				if (IsPoll(R))
				{
					Poll = R;
				}
			}
		}
		if (TestTrue(TEXT("The Fog tab reads Current"), Poll.IsValid()))
		{
			TArray<FString> PollNames;
			for (const TSharedPtr<FJsonValue>& PollName : Poll->GetArrayField(TEXT("names")))
			{
				PollNames.Add(PollName->AsString());
			}
			TestEqual(TEXT("One get for both status lines"), PollNames, TArray<FString>{CurrentName, LightCurrentName});
			if (!Step({Serve(Poll)}))
			{
				return false;
			}
			ReadUI();
			TestEqual(TEXT("Status line shows Current"), Controls[TEXT("fc_current")]->GetStringField(TEXT("text")),
				TEXT("Now: ") + FogVariable(*CurrentName)->GetString());
			if (TestTrue(TEXT("Lighting status line"), Controls.Contains(TEXT("ld_current"))))
			{
				TestEqual(TEXT("Lighting status line shows its Current"), Controls[TEXT("ld_current")]->GetStringField(TEXT("text")),
					FString(TEXT("Now: On. Moon 0.42, sun 0.00. Light 8.3° up, bearing 12°.")));
			}
		}
	}
	ReadUI();
	// A fresh client: the atmospheric tables, Clear, the horizon band on. Table controls and Band column shown;
	// no palettes, no other weather.
	for (const TCHAR* Id : {TEXT("fc_current"), TEXT("fc_draw"), TEXT("fc_strength"), TEXT("fc_saturation"), TEXT("fc_window"), TEXT("fc_weather"),
		TEXT("fc_variation"), TEXT("fc_day_offset"), TEXT("fc_set"), TEXT("fc_column"), TEXT("fc_table_brightness"), TEXT("fc_split_haze"),
		TEXT("fc_haze_column"), TEXT("fc_edit_weather"), TEXT("fc_clear_table"), TEXT("fc_clear_sunrise_bright"), TEXT("fc_clear_sunset_bright"), TEXT("reset_fog")})
	{
		TestTrue(*FString::Printf(TEXT("Fog control %s"), Id), Controls.Contains(Id));
	}
	for (const TCHAR* Id : {TEXT("fc_clear_sunrise_palette"), TEXT("fc_clear_sunset_palette"), TEXT("fc_sunny_table"),
		TEXT("fc_clear_sunrise_hue"), TEXT("fc_clear_sunset_sat")})
	{
		TestFalse(*FString::Printf(TEXT("Fog control %s is hidden"), Id), Controls.Contains(Id));
	}
	TestEqual(TEXT("Fog colors label"), Controls[TEXT("fc_draw")]->GetStringField(TEXT("text")), FString(TEXT("Fog colors")));
	// The defaults chosen in game on 2026-11-22. Saturation follows the option: the slider shows the value in use (from Current).
	TestEqual(TEXT("Saturation shows the option's 0.35"), Controls[TEXT("fc_saturation")]->GetNumberField(TEXT("value")), .35, 1e-6);
	TestEqual(TEXT("Window starts at 1.5"), Controls[TEXT("fc_window")]->GetNumberField(TEXT("value")), 1.5, 1e-6);
	TestEqual(TEXT("Color set starts at Atmospheric tables"), Controls[TEXT("fc_set")]->GetStringField(TEXT("value")), FString(TEXT("Atmospheric tables")));
	TestEqual(TEXT("Column starts at Horizon"), Controls[TEXT("fc_column")]->GetStringField(TEXT("value")), FString(TEXT("Horizon")));
	TestEqual(TEXT("Clear table is Clear sky"), Controls[TEXT("fc_clear_table")]->GetStringField(TEXT("value")), FString(TEXT("Clear sky")));
	TestEqual(TEXT("Clear sunrise brightness 0.85"), Controls[TEXT("fc_clear_sunrise_bright")]->GetNumberField(TEXT("value")), .85, 1e-6);
	TestEqual(TEXT("Clear sunset brightness 0.8"), Controls[TEXT("fc_clear_sunset_bright")]->GetNumberField(TEXT("value")), .8, 1e-6);
	TArray<FString> Ids;
	auto At = [&](const TCHAR* Id) { return Ids.IndexOfByKey(FString(Id)); };
	Controls.GenerateKeyArray(Ids);
	// Storm fade: a slider right after Window, 0 to 1 in steps of 0.05, starting at 0.35.
	if (TestTrue(TEXT("Fog control fc_storm_fade"), Controls.Contains(TEXT("fc_storm_fade"))))
	{
		const TSharedPtr<FJsonObject> StormFade = Controls[TEXT("fc_storm_fade")];
		TestEqual(TEXT("Storm fade label"), StormFade->GetStringField(TEXT("text")), FString(TEXT("Storm fade")));
		TestEqual(TEXT("Storm fade is a slider"), StormFade->GetStringField(TEXT("type")), FString(TEXT("slider")));
		TestEqual(TEXT("Storm fade min"), StormFade->GetNumberField(TEXT("min")), 0.);
		TestEqual(TEXT("Storm fade max"), StormFade->GetNumberField(TEXT("max")), 1.);
		TestEqual(TEXT("Storm fade step"), StormFade->GetNumberField(TEXT("step")), .05, 1e-6);
		TestEqual(TEXT("Storm fade starts at 0.35"), StormFade->GetNumberField(TEXT("value")), .35, 1e-6);
		TestTrue(TEXT("Storm fade comes right after Window"), At(TEXT("fc_window")) != INDEX_NONE && At(TEXT("fc_window")) < At(TEXT("fc_storm_fade")));
	}
	// Day variation (0 to 1, step 0.05, 0.5) and Preview day (-10 to 10, step 1, 0), below Storm fade and before Weather.
	struct FSliderCheck { const TCHAR* Id; const TCHAR* Text; double Min, Max, Step, Value; };
	for (const FSliderCheck& S : {FSliderCheck{TEXT("fc_variation"), TEXT("Day variation"), 0., 1., .05, .5},
		FSliderCheck{TEXT("fc_day_offset"), TEXT("Preview day"), -10., 10., 1., 0.}})
	{
		if (TestTrue(*FString::Printf(TEXT("Fog control %s"), S.Id), Controls.Contains(S.Id)))
		{
			const TSharedPtr<FJsonObject> C = Controls[S.Id];
			TestEqual(*FString::Printf(TEXT("%s label"), S.Id), C->GetStringField(TEXT("text")), FString(S.Text));
			TestEqual(*FString::Printf(TEXT("%s is a slider"), S.Id), C->GetStringField(TEXT("type")), FString(TEXT("slider")));
			TestEqual(*FString::Printf(TEXT("%s min"), S.Id), C->GetNumberField(TEXT("min")), S.Min, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s max"), S.Id), C->GetNumberField(TEXT("max")), S.Max, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s step"), S.Id), C->GetNumberField(TEXT("step")), S.Step, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s starts at its default"), S.Id), C->GetNumberField(TEXT("value")), S.Value, 1e-6);
		}
	}
	TestTrue(TEXT("Storm fade, Day variation, Preview day, Weather in this order"), At(TEXT("fc_storm_fade")) < At(TEXT("fc_variation"))
		&& At(TEXT("fc_variation")) < At(TEXT("fc_day_offset")) && At(TEXT("fc_day_offset")) < At(TEXT("fc_weather")));
	TestEqual(TEXT("Weather follows the server"), Controls[TEXT("fc_weather")]->GetStringField(TEXT("value")), FString(TEXT("Follow server")));
	// Separate horizon band: a check box right after Use table brightness, on; Band column right after it, Mid-sky.
	if (TestTrue(TEXT("Tables: fc_split_haze shown"), Controls.Contains(TEXT("fc_split_haze"))))
	{
		const TSharedPtr<FJsonObject> Split = Controls[TEXT("fc_split_haze")];
		TestEqual(TEXT("Separate horizon band label"), Split->GetStringField(TEXT("text")), FString(TEXT("Separate horizon band")));
		TestEqual(TEXT("Separate horizon band is a check box"), Split->GetStringField(TEXT("type")), FString(TEXT("checkbox")));
		TestTrue(TEXT("Separate horizon band is on"), Split->GetBoolField(TEXT("value")));
		TestTrue(TEXT("Separate horizon band comes right after Use table brightness"), At(TEXT("fc_table_brightness")) != INDEX_NONE
			&& At(TEXT("fc_table_brightness")) < At(TEXT("fc_split_haze")) && At(TEXT("fc_split_haze")) < At(TEXT("fc_edit_weather")));
	}
	if (TestTrue(TEXT("Separate horizon band on: Band column shown"), Controls.Contains(TEXT("fc_haze_column"))))
	{
		const TSharedPtr<FJsonObject> BandColumn = Controls[TEXT("fc_haze_column")];
		TestEqual(TEXT("Band column label"), BandColumn->GetStringField(TEXT("text")), FString(TEXT("Band column")));
		TestEqual(TEXT("Band column is a select"), BandColumn->GetStringField(TEXT("type")), FString(TEXT("select")));
		TArray<FString> Options;
		for (const TSharedPtr<FJsonValue>& O : BandColumn->GetArrayField(TEXT("options")))
		{
			Options.Add(O->AsString());
		}
		TestEqual(TEXT("Band column options"), Options, TArray<FString>{TEXT("Horizon"), TEXT("Mid-sky"), TEXT("Zenith")});
		TestEqual(TEXT("Band column starts at Mid-sky"), BandColumn->GetStringField(TEXT("value")), FString(TEXT("Mid-sky")));
		TestTrue(TEXT("Band column comes right after Separate horizon band"), At(TEXT("fc_split_haze")) < At(TEXT("fc_haze_column"))
			&& At(TEXT("fc_haze_column")) < At(TEXT("fc_edit_weather")));
	}
	// Saturation follows the option: a new Current with another option value moves the slider; moving it sets the override.
	CurrentVariable->Set(TEXT("On. Clear (server), day 77 (+0). Atmospheric tables, Horizon, Clear sky, band Mid-sky; saturation 0.65 (option). Variation 0.50: outside the windows. Weights: sunrise 0.00, sunset 0.00; rain 0.00, overcast 0.00; amount 0.00."), ECVF_SetByCode);
	{
		TSharedPtr<FJsonObject> Poll;
		for (int32 I = 0; I < 4 && !Poll.IsValid(); ++I)
		{
			if (!Step({}))
			{
				return false;
			}
			for (const TSharedPtr<FJsonObject>& R : AllRequests())
			{
				if (IsPoll(R))
				{
					Poll = R;
				}
			}
		}
		if (TestTrue(TEXT("Current read again"), Poll.IsValid()) && !Step({Serve(Poll)}))
		{
			return false;
		}
	}
	ReadUI();
	TestEqual(TEXT("Saturation shows the option's new 0.65"), Controls[TEXT("fc_saturation")]->GetNumberField(TEXT("value")), .65, 1e-6);
	TestEqual(TEXT("The client still follows the option"), FogVariable(TEXT("personal.FogColor.Saturation"))->GetFloat(), -1.f);
	if (!Step({Event(TEXT("change"), TEXT("fc_saturation"), MakeShared<FJsonValueNumber>(.45))}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("One set for Saturation"), Sent.Num(), 1))
	{
		TestEqual(TEXT("Saturation override 0.45"), Sent[0]->GetObjectField(TEXT("values"))->GetNumberField(TEXT("personal.FogColor.Saturation")), .45, 1e-6);
		if (!Step({Serve(Sent[0])}))
		{
			return false;
		}
		TestEqual(TEXT("The client uses the override"), FogVariable(TEXT("personal.FogColor.Saturation"))->GetFloat(), .45f, 1e-6f);
	}
	ReadUI();
	TestEqual(TEXT("Saturation shows the override"), Controls[TEXT("fc_saturation")]->GetNumberField(TEXT("value")), .45, 1e-6);
	// Rainy, Separate horizon band off, Day variation 1, Preview day +3.
	if (!Step({Event(TEXT("change"), TEXT("fc_edit_weather"), MakeShared<FJsonValueString>(TEXT("Rainy"))),
		Event(TEXT("change"), TEXT("fc_split_haze"), MakeShared<FJsonValueBoolean>(false)),
		Event(TEXT("change"), TEXT("fc_variation"), MakeShared<FJsonValueNumber>(1.)),
		Event(TEXT("change"), TEXT("fc_day_offset"), MakeShared<FJsonValueNumber>(3.))}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("One set for the band and the day"), Sent.Num(), 1))
	{
		const TSharedPtr<FJsonObject> DayValues = Sent[0]->GetObjectField(TEXT("values"));
		TestEqual(TEXT("Three values"), DayValues->Values.Num(), 3);
		TestEqual(TEXT("SplitHaze 0"), DayValues->GetNumberField(TEXT("personal.FogColor.SplitHaze")), 0.);
		TestEqual(TEXT("Variation 1"), DayValues->GetNumberField(TEXT("personal.FogColor.Variation")), 1.);
		TestEqual(TEXT("DayOffset 3"), DayValues->GetNumberField(TEXT("personal.FogColor.DayOffset")), 3.);
		if (!Step({Serve(Sent[0])}))
		{
			return false;
		}
		TestEqual(TEXT("The client previews day +3"), FogVariable(TEXT("personal.FogColor.DayOffset"))->GetInt(), 3);
	}
	ReadUI();
	for (const TCHAR* Id : {TEXT("fc_column"), TEXT("fc_table_brightness"), TEXT("fc_rainy_table"), TEXT("fc_rainy_sunrise_bright"), TEXT("fc_rainy_sunset_bright")})
	{
		TestTrue(*FString::Printf(TEXT("Tables, Rainy: %s shown"), Id), Controls.Contains(Id));
	}
	for (const TCHAR* Id : {TEXT("fc_rainy_sunrise_palette"), TEXT("fc_clear_table"), TEXT("fc_clear_sunrise_bright")})
	{
		TestFalse(*FString::Printf(TEXT("Tables, Rainy: %s hidden"), Id), Controls.Contains(Id));
	}
	TestEqual(TEXT("Rainy table is Overcast and rainy"), Controls[TEXT("fc_rainy_table")]->GetStringField(TEXT("value")), FString(TEXT("Overcast and rainy")));
	TestEqual(TEXT("Rainy sunrise brightness 0.75"), Controls[TEXT("fc_rainy_sunrise_bright")]->GetNumberField(TEXT("value")), .75, 1e-6);
	TestEqual(TEXT("Rainy sunset brightness 0.75"), Controls[TEXT("fc_rainy_sunset_bright")]->GetNumberField(TEXT("value")), .75, 1e-6);
	TestFalse(TEXT("Separate horizon band off: Band column hidden"), Controls.Contains(TEXT("fc_haze_column")));
	TestEqual(TEXT("Preview day shows +3"), Controls[TEXT("fc_day_offset")]->GetNumberField(TEXT("value")), 3., 1e-6);
	// Palettes and Clear; Clear's sunset palette to Custom: the Custom sunset shows its hue and saturation.
	if (!Step({Event(TEXT("change"), TEXT("fc_set"), MakeShared<FJsonValueString>(TEXT("Palettes"))),
		Event(TEXT("change"), TEXT("fc_edit_weather"), MakeShared<FJsonValueString>(TEXT("Clear"))),
		Event(TEXT("change"), TEXT("fc_clear_sunset_palette"), MakeShared<FJsonValueString>(TEXT("Custom")))}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("One set for the Fog changes"), Sent.Num(), 1))
	{
		const TSharedPtr<FJsonObject> FogValues = Sent[0]->GetObjectField(TEXT("values"));
		TestEqual(TEXT("Two values"), FogValues->Values.Num(), 2);
		TestEqual(TEXT("Color set 0"), FogValues->GetNumberField(TEXT("personal.FogColor.Set")), 0.);
		TestEqual(TEXT("Custom is 5"), FogValues->GetNumberField(TEXT("personal.FogColor.Clear.SunsetPalette")), 5.);
		if (!Step({Serve(Sent[0])}))
		{
			return false;
		}
		TestEqual(TEXT("The client uses the palettes"), FogVariable(TEXT("personal.FogColor.Set"))->GetInt(), 0);
	}
	ReadUI();
	TestTrue(TEXT("Palettes: Clear palettes shown"), Controls.Contains(TEXT("fc_clear_sunrise_palette")) && Controls.Contains(TEXT("fc_clear_sunset_palette")));
	if (Controls.Contains(TEXT("fc_clear_sunrise_palette")))
	{
		TestEqual(TEXT("Clear sunrise palette is From Dusk to Dawn"), Controls[TEXT("fc_clear_sunrise_palette")]->GetStringField(TEXT("value")), FString(TEXT("From Dusk to Dawn")));
		TestEqual(TEXT("Six presets"), Controls[TEXT("fc_clear_sunrise_palette")]->GetArrayField(TEXT("options")).Num(), 6);
	}
	TestTrue(TEXT("Custom sunset hue shown"), Controls.Contains(TEXT("fc_clear_sunset_hue")) && Controls.Contains(TEXT("fc_clear_sunset_sat")));
	TestFalse(TEXT("Sunrise hue still hidden"), Controls.Contains(TEXT("fc_clear_sunrise_hue")));
	for (const TCHAR* Id : {TEXT("fc_column"), TEXT("fc_table_brightness"), TEXT("fc_clear_table"), TEXT("fc_split_haze"), TEXT("fc_haze_column")})
	{
		TestFalse(*FString::Printf(TEXT("Palettes: %s hidden"), Id), Controls.Contains(Id));
	}
	TestTrue(TEXT("Palettes: Day variation and Preview day still shown"), Controls.Contains(TEXT("fc_variation")) && Controls.Contains(TEXT("fc_day_offset")));
	// Reset tab puts every Fog variable back, shown or not, to the defaults; Saturation follows the option again.
	if (!Step({Event(TEXT("click"), TEXT("reset_fog"), nullptr)}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("Fog reset sends one set"), Sent.Num(), 1))
	{
		const TSharedPtr<FJsonObject> ResetValues = Sent[0]->GetObjectField(TEXT("values"));
		TestEqual(TEXT("Every Fog variable (49)"), ResetValues->Values.Num(), 49);
		for (const TCHAR* Name : {TEXT("personal.FogColor.StormFade"), TEXT("personal.FogColor.SplitHaze"), TEXT("personal.FogColor.HazeColumn"),
			TEXT("personal.FogColor.Variation"), TEXT("personal.FogColor.DayOffset"), TEXT("personal.FogColor.Saturation")})
		{
			TestTrue(*FString::Printf(TEXT("%s is reset"), Name), ResetValues->HasField(Name));
		}
		TestEqual(TEXT("Saturation reset to -1"), ResetValues->GetNumberField(TEXT("personal.FogColor.Saturation")), -1., 1e-6);
		TestTrue(TEXT("The Fog reset is accepted"), Serve(Sent[0])->AsObject()->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("Clear sunset palette back to 1"), FogVariable(TEXT("personal.FogColor.Clear.SunsetPalette"))->GetInt(), 1);
		TestEqual(TEXT("Color set back to the tables"), FogVariable(TEXT("personal.FogColor.Set"))->GetInt(), 1);
		TestEqual(TEXT("Separate horizon band back to on"), FogVariable(TEXT("personal.FogColor.SplitHaze"))->GetInt(), 1);
		TestEqual(TEXT("Saturation back to the option"), FogVariable(TEXT("personal.FogColor.Saturation"))->GetFloat(), -1.f);
		TestEqual(TEXT("Window back to 1.5"), FogVariable(TEXT("personal.FogColor.Window"))->GetFloat(), 1.5f, 1e-6f);
		TestEqual(TEXT("Storm fade back to 0.35"), FogVariable(TEXT("personal.FogColor.StormFade"))->GetFloat(), .35f, 1e-6f);
		TestEqual(TEXT("Day variation back to 0.5"), FogVariable(TEXT("personal.FogColor.Variation"))->GetFloat(), .5f, 1e-6f);
		TestEqual(TEXT("Preview day back to 0"), FogVariable(TEXT("personal.FogColor.DayOffset"))->GetInt(), 0);
		TestEqual(TEXT("Clear sunset brightness back to 0.8"), FogVariable(TEXT("personal.FogColor.Clear.SunsetBright"))->GetFloat(), .8f, 1e-6f);
	}
	ReadUI();
	TestEqual(TEXT("After Reset tab, Saturation shows the option's value again"), Controls[TEXT("fc_saturation")]->GetNumberField(TEXT("value")), .65, 1e-6);

	// Lighting tab: the status line, Smooth light direction (on), Horizon fade (5 to 45, step 1, 20), Night turn
	// (0.01 to 0.25, step 0.01, 0.1), in this order, then Reset tab.
	{
		TArray<FString> LightIds;
		for (const TSharedPtr<FJsonValue>& Row : Controls[TEXT("lighting")]->GetArrayField(TEXT("children")))
		{
			for (const TSharedPtr<FJsonValue>& Child : Row->AsObject()->GetArrayField(TEXT("children")))
			{
				LightIds.Add(Child->AsObject()->GetStringField(TEXT("id")));
			}
		}
		TestEqual(TEXT("Lighting tab controls in order"), LightIds, TArray<FString>{TEXT("ld_current"), TEXT("ld_smooth"), TEXT("ld_horizon_fade"),
			TEXT("ld_night_turn"), TEXT("reset_lighting"), TEXT("lighting_reset_row_fill2"), TEXT("lighting_reset_row_fill3")});
	}
	if (TestTrue(TEXT("Lighting control ld_smooth"), Controls.Contains(TEXT("ld_smooth"))))
	{
		const TSharedPtr<FJsonObject> Smooth = Controls[TEXT("ld_smooth")];
		TestEqual(TEXT("Smooth light direction label"), Smooth->GetStringField(TEXT("text")), FString(TEXT("Smooth light direction")));
		TestEqual(TEXT("Smooth light direction is a check box"), Smooth->GetStringField(TEXT("type")), FString(TEXT("checkbox")));
		TestTrue(TEXT("Smooth light direction is on"), Smooth->GetBoolField(TEXT("value")));
		TestTrue(TEXT("Smooth light direction is enabled"), Enabled(TEXT("ld_smooth")));
	}
	for (const FSliderCheck& S : {FSliderCheck{TEXT("ld_horizon_fade"), TEXT("Horizon fade"), 5., 45., 1., 20.},
		FSliderCheck{TEXT("ld_night_turn"), TEXT("Night turn"), .01, .25, .01, .1}})
	{
		if (TestTrue(*FString::Printf(TEXT("Lighting control %s"), S.Id), Controls.Contains(S.Id)))
		{
			const TSharedPtr<FJsonObject> C = Controls[S.Id];
			TestEqual(*FString::Printf(TEXT("%s label"), S.Id), C->GetStringField(TEXT("text")), FString(S.Text));
			TestEqual(*FString::Printf(TEXT("%s is a slider"), S.Id), C->GetStringField(TEXT("type")), FString(TEXT("slider")));
			TestEqual(*FString::Printf(TEXT("%s min"), S.Id), C->GetNumberField(TEXT("min")), S.Min, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s max"), S.Id), C->GetNumberField(TEXT("max")), S.Max, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s step"), S.Id), C->GetNumberField(TEXT("step")), S.Step, 1e-6);
			TestEqual(*FString::Printf(TEXT("%s starts at its default"), S.Id), C->GetNumberField(TEXT("value")), S.Value, 1e-6);
		}
	}
	auto LightVariable = [&](const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name); };
	if (!Step({Event(TEXT("change"), TEXT("ld_smooth"), MakeShared<FJsonValueBoolean>(false)),
		Event(TEXT("change"), TEXT("ld_horizon_fade"), MakeShared<FJsonValueNumber>(15.)),
		Event(TEXT("change"), TEXT("ld_night_turn"), MakeShared<FJsonValueNumber>(.05))}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("One set for the Lighting changes"), Sent.Num(), 1))
	{
		const TSharedPtr<FJsonObject> LightValues = Sent[0]->GetObjectField(TEXT("values"));
		TestEqual(TEXT("Three Lighting values"), LightValues->Values.Num(), 3);
		TestEqual(TEXT("Smooth 0"), LightValues->GetNumberField(TEXT("personal.LightDir.Smooth")), 0.);
		TestEqual(TEXT("HorizonFade 15"), LightValues->GetNumberField(TEXT("personal.LightDir.HorizonFade")), 15., 1e-6);
		TestEqual(TEXT("NightTurn 0.05"), LightValues->GetNumberField(TEXT("personal.LightDir.NightTurn")), .05, 1e-6);
		if (!Step({Serve(Sent[0])}))
		{
			return false;
		}
		TestEqual(TEXT("The client's light is no longer smoothed"), LightVariable(TEXT("personal.LightDir.Smooth"))->GetInt(), 0);
		TestEqual(TEXT("The client uses Horizon fade 15"), LightVariable(TEXT("personal.LightDir.HorizonFade"))->GetFloat(), 15.f, 1e-6f);
		TestEqual(TEXT("The client uses Night turn 0.05"), LightVariable(TEXT("personal.LightDir.NightTurn"))->GetFloat(), .05f, 1e-6f);
	}
	ReadUI();
	TestFalse(TEXT("Smooth light direction shows off"), Controls[TEXT("ld_smooth")]->GetBoolField(TEXT("value")));
	if (!Step({Event(TEXT("click"), TEXT("reset_lighting"), nullptr)}))
	{
		return false;
	}
	Sent = Requests();
	if (TestEqual(TEXT("Lighting reset sends one set"), Sent.Num(), 1))
	{
		TestEqual(TEXT("Every Lighting variable (3)"), Sent[0]->GetObjectField(TEXT("values"))->Values.Num(), 3);
		TestTrue(TEXT("The Lighting reset is accepted"), Serve(Sent[0])->AsObject()->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("Smooth back to 1"), LightVariable(TEXT("personal.LightDir.Smooth"))->GetInt(), 1);
		TestEqual(TEXT("Horizon fade back to 20"), LightVariable(TEXT("personal.LightDir.HorizonFade"))->GetFloat(), 20.f, 1e-6f);
		TestEqual(TEXT("Night turn back to 0.1"), LightVariable(TEXT("personal.LightDir.NightTurn"))->GetFloat(), .1f, 1e-6f);
	}

	// Admin tab: commands go to the chat service, one per callback, at least one second apart.
	auto ChatSent = [&]()
	{
		TArray<FString> Texts;
		for (const TSharedPtr<FJsonObject>& R : Requests())
		{
			if (R->GetStringField(TEXT("service")) == TEXT("chat"))
			{
				TestEqual(TEXT("Chat operation"), R->GetStringField(TEXT("operation")), FString(TEXT("send")));
				Texts.Add(R->GetStringField(TEXT("text")));
			}
		}
		return Texts;
	};
	auto ChatRequest = [&]() -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonObject>& R : Requests())
		{
			if (R->GetStringField(TEXT("service")) == TEXT("chat"))
			{
				return R;
			}
		}
		return nullptr;
	};
	auto Status = [&]() { return Controls[TEXT("status")]->GetStringField(TEXT("text")); };
	if (!Step({}))
	{
		return false;
	}
	TestTrue(TEXT("The UI is valid with the Admin tab (256 controls, eight levels)"), ReadUI());
	AddInfo(FString::Printf(TEXT("Dashboard window: %d controls"), Count));
	const TArray<TSharedPtr<FJsonValue>>& TabList = Controls[TEXT("tabs")]->GetArrayField(TEXT("children"));
	if (TestTrue(TEXT("Admin tab exists"), Controls.Contains(TEXT("admin"))))
	{
		TestEqual(TEXT("Admin is the last tab"), TabList.Last()->AsObject()->GetStringField(TEXT("id")), FString(TEXT("admin")));
		TestEqual(TEXT("Admin tab title"), Controls[TEXT("admin")]->GetStringField(TEXT("text")), FString(TEXT("Admin")));
		TestEqual(TEXT("Admin note"), Controls[TEXT("adm_note")]->GetStringField(TEXT("text")), FString(TEXT("Admin commands work only for an admin character (local server).")));
	}
	// Reset tab: no wrap=false (it clips the border); a third of the row, like Log frame rate.
	TestFalse(TEXT("Reset tab is not clipped"), Controls[TEXT("reset_far_scenery")]->HasField(TEXT("wrap")));
	TestEqual(TEXT("Reset tab row has three columns"), Controls[TEXT("far_scenery_reset_row")]->GetArrayField(TEXT("children")).Num(), 3);
	TestEqual(TEXT("Log frame rate row has three columns"), Controls[TEXT("shared")]->GetArrayField(TEXT("children")).Num(), 3);
	for (const TCHAR* Id : {TEXT("adm_location_row"), TEXT("adm_name_row"), TEXT("adm_place_row")})
	{
		FString VAlign;
		TestTrue(*FString::Printf(TEXT("%s is bottom-aligned"), Id), Controls.Contains(Id) && Controls[Id]->TryGetStringField(TEXT("valign"), VAlign) && VAlign == TEXT("bottom"));
	}
	TestFalse(TEXT("Set weather is gone"), Controls.Contains(TEXT("adm_set_weather")));
	TestFalse(TEXT("Set time is gone"), Controls.Contains(TEXT("adm_set_time")));
	TestEqual(TEXT("Five weather buttons"), Controls[TEXT("adm_weather_row")]->GetArrayField(TEXT("children")).Num(), 5);
	TestEqual(TEXT("Storm label"), Controls[TEXT("adm_w_storm")]->GetStringField(TEXT("text")), FString(TEXT("Storm")));
	TestEqual(TEXT("Four time buttons"), Controls[TEXT("adm_time_row")]->GetArrayField(TEXT("children")).Num(), 4);
	TestFalse(TEXT("No weather Default button"), Controls.Contains(TEXT("adm_w_default")));
	TestFalse(TEXT("No time Default button"), Controls.Contains(TEXT("adm_t_default")));
	if (TestTrue(TEXT("Reset sky is still there"), Controls.Contains(TEXT("adm_reset_sky"))))
	{
		TestEqual(TEXT("Reset sky label"), Controls[TEXT("adm_reset_sky")]->GetStringField(TEXT("text")), FString(TEXT("Reset sky")));
		TestEqual(TEXT("Reset sky tooltip"), Controls[TEXT("adm_reset_sky")]->GetStringField(TEXT("tooltip")), FString(TEXT("@weather reset, @time reset, @timelapse reset")));
	}

	// A button sends its command with @, which the ACE server runs as a command.
	if (!Step({Event(TEXT("click"), TEXT("adm_heal"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Heal sends @heal"), ChatSent(), TArray<FString>{TEXT("@heal")});
	ReadUI();
	TestEqual(TEXT("The status shows the command"), Status(), FString(TEXT("Sent: @heal")));

	// Two buttons in one callback: two sends, one second apart.
	if (!Step({Event(TEXT("click"), TEXT("adm_god"), nullptr), Event(TEXT("click"), TEXT("adm_buff"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("God mode first"), ChatSent(), TArray<FString>{TEXT("@god")});
	const double FirstSend = Clock;
	if (!Step({}, .5))
	{
		return false;
	}
	TestEqual(TEXT("Nothing within the second"), ChatSent().Num(), 0);
	if (!Step({}, .5))
	{
		return false;
	}
	TSharedPtr<FJsonObject> BuffRequest = ChatRequest();
	TestEqual(TEXT("Buff one second later"), ChatSent(), TArray<FString>{TEXT("@buff")});
	TestEqual(TEXT("One second apart"), Clock - FirstSend, 1.);

	// A refused send is shown on the status line and sent once more.
	if (BuffRequest.IsValid())
	{
		TSharedPtr<FJsonObject> Refused = MakeShared<FJsonObject>();
		Refused->SetStringField(TEXT("type"), TEXT("result"));
		Refused->SetStringField(TEXT("id"), BuffRequest->GetStringField(TEXT("id")));
		Refused->SetBoolField(TEXT("ok"), false);
		Refused->SetStringField(TEXT("error"), TEXT("Chat requires one line, 1-500 characters, at most one send per second"));
		if (!Step({MakeShared<FJsonValueObject>(Refused)}, .5))
		{
			return false;
		}
		ReadUI();
		TestTrue(TEXT("The refusal is on the status line"), Status().StartsWith(TEXT("Refused: @buff")));
		if (!Step({}, .5))
		{
			return false;
		}
		TestEqual(TEXT("The refused command is sent again"), ChatSent(), TArray<FString>{TEXT("@buff")});
	}

	// Weather and time buttons send at once.
	if (!Step({Event(TEXT("click"), TEXT("adm_w_storm"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Storm sends passing"), ChatSent(), TArray<FString>{TEXT("@weather passing")});
	if (!Step({Event(TEXT("click"), TEXT("adm_w_clear"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Clear"), ChatSent(), TArray<FString>{TEXT("@weather clear")});
	if (!Step({Event(TEXT("click"), TEXT("adm_t_dusk"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Dusk"), ChatSent(), TArray<FString>{TEXT("@time set dusk")});

	// The first typing in Place with no place list asks for it once, then Go to place.
	if (!Step({Event(TEXT("change"), TEXT("adm_place_0"), MakeShared<FJsonValueString>(TEXT(" Arwic "))),
		Event(TEXT("click"), TEXT("adm_go_place"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("The place list is asked for"), ChatSent(), TArray<FString>{TEXT("@telepoi list")});
	ReadUI();
	TestTrue(TEXT("Status says so"), Status().StartsWith(TEXT("Getting the list of places.")));
	if (!Step({}))
	{
		return false;
	}
	TestEqual(TEXT("Go to place"), ChatSent(), TArray<FString>{TEXT("@telepoi Arwic")});

	// The server's list arrives in chat; typing "ar" suggests names starting with it, then containing it.
	{
		TSharedPtr<FJsonObject> Chat = MakeShared<FJsonObject>();
		Chat->SetStringField(TEXT("raw_text"), TEXT("All POIs: Ahurenga, Al-Arqas, Arwic, Holtburg, Kara, Marketplace, Qalabar, Sanamar, Xarabydun, Yaraq"));
		Chat->SetStringField(TEXT("sender"), TEXT(""));
		Chat->SetStringField(TEXT("sender_type"), TEXT("server"));
		Chat->SetNumberField(TEXT("chat_type"), 0);
		Input->SetArrayField(TEXT("chat_events"), {MakeShared<FJsonValueObject>(Chat)});
	}
	const bool bTyped = Step({Event(TEXT("change"), TEXT("adm_place_0"), MakeShared<FJsonValueString>(TEXT("ar")))});
	Input->SetArrayField(TEXT("chat_events"), {});
	if (!bTyped)
	{
		return false;
	}
	TestEqual(TEXT("The list is not asked for again"), ChatSent().Num(), 0);
	ReadUI();
	const TArray<FString> Expected = {TEXT("Arwic"), TEXT("Al-Arqas"), TEXT("Kara"), TEXT("Marketplace"), TEXT("Qalabar"), TEXT("Sanamar")};
	for (int32 I = 0; I < Expected.Num(); ++I)
	{
		const FString Id = FString::Printf(TEXT("adm_sugg_%d"), I + 1);
		TestTrue(*(TEXT("Suggestion ") + Expected[I]), Controls.Contains(Id) && Controls[Id]->GetStringField(TEXT("text")) == Expected[I]);
	}
	TestFalse(TEXT("At most six"), Controls.Contains(TEXT("adm_sugg_7")));
	TestEqual(TEXT("Rows of three"), Controls[TEXT("adm_sugg_row1")]->GetArrayField(TEXT("children")).Num(), 3);
	if (!Step({Event(TEXT("click"), TEXT("adm_sugg_2"), nullptr)}))
	{
		return false;
	}
	ReadUI();
	TestFalse(TEXT("The old Place input is gone"), Controls.Contains(TEXT("adm_place_0")));
	TestTrue(TEXT("A new Place input holds the name"), Controls.Contains(TEXT("adm_place_1")) && Controls[TEXT("adm_place_1")]->GetStringField(TEXT("value")) == TEXT("Al-Arqas"));
	TestFalse(TEXT("No suggestions for an exact name"), Controls.Contains(TEXT("adm_sugg_1")));

	// Coordinates go to @tele.
	if (!Step({Event(TEXT("change"), TEXT("adm_place_1"), MakeShared<FJsonValueString>(TEXT("83.0S, 93.2E"))),
		Event(TEXT("click"), TEXT("adm_go_place"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Coordinates"), ChatSent(), TArray<FString>{TEXT("@tele 83.0S 93.2E")});
	ReadUI();
	TestFalse(TEXT("No suggestions for coordinates"), Controls.Contains(TEXT("adm_sugg_1")));
	if (!Step({Event(TEXT("change"), TEXT("adm_place_1"), MakeShared<FJsonValueString>(TEXT("12.5n,40w"))),
		Event(TEXT("click"), TEXT("adm_go_place"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Coordinates without a space"), ChatSent(), TArray<FString>{TEXT("@tele 12.5N 40W")});
	if (!Step({Event(TEXT("change"), TEXT("adm_attackable"), MakeShared<FJsonValueBoolean>(true))}))
	{
		return false;
	}
	TestEqual(TEXT("Attackable on"), ChatSent(), TArray<FString>{TEXT("@attackable on")});

	// Saved locations: an empty name is refused; a save stores the position and clears the name.
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("cell"), double(0xA9B4001Au));
	Position->SetNumberField(TEXT("x"), 84.5);
	Position->SetNumberField(TEXT("y"), -12.25);
	Position->SetNumberField(TEXT("z"), 42.005);
	Input->SetObjectField(TEXT("position"), Position);
	if (!Step({Event(TEXT("click"), TEXT("adm_save"), nullptr)}))
	{
		return false;
	}
	ReadUI();
	TestTrue(TEXT("An empty name is refused"), Status().StartsWith(TEXT("Type a name first")));
	TestFalse(TEXT("Nothing saved"), Out->HasField(TEXT("state")));
	TestFalse(TEXT("Go waits for a location"), Enabled(TEXT("adm_go")));
	if (!Step({Event(TEXT("change"), TEXT("adm_name_0"), MakeShared<FJsonValueString>(TEXT("Test spot"))),
		Event(TEXT("click"), TEXT("adm_save"), nullptr)}))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SavedState = nullptr;
	if (TestTrue(TEXT("The save returns state"), Out->TryGetObjectField(TEXT("state"), SavedState)))
	{
		const TArray<TSharedPtr<FJsonValue>>& List = (*SavedState)->GetArrayField(TEXT("locations"));
		if (TestEqual(TEXT("One saved location"), List.Num(), 1))
		{
			const TSharedPtr<FJsonObject> L = List[0]->AsObject();
			TestEqual(TEXT("Saved name"), L->GetStringField(TEXT("name")), FString(TEXT("Test spot")));
			TestEqual(TEXT("Saved cell"), L->GetNumberField(TEXT("cell")), double(0xA9B4001Au));
			TestEqual(TEXT("Saved z"), L->GetNumberField(TEXT("z")), 42.005);
		}
	}
	ReadUI();
	TestFalse(TEXT("The old Name input is gone"), Controls.Contains(TEXT("adm_name_0")));
	TestTrue(TEXT("A new, empty Name input"), Controls.Contains(TEXT("adm_name_1")) && Controls[TEXT("adm_name_1")]->GetStringField(TEXT("value")).IsEmpty());
	TestEqual(TEXT("The location is selected"), Controls[TEXT("adm_location")]->GetStringField(TEXT("value")), FString(TEXT("Test spot")));
	const FString Teleloc = TEXT("@teleloc 0xA9B4001A 84.500000 -12.250000 42.005000");
	if (!Step({Event(TEXT("click"), TEXT("adm_go"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Go sends teleloc"), ChatSent(), TArray<FString>{Teleloc});

	// The saved locations come back from state after a restart of the plugin.
	FACEPluginVM Restarted;
	if (!TestTrue(TEXT("Dashboard Lua loads again"), Restarted.Load(Source, Error)))
	{
		AddError(Error);
		return false;
	}
	Active = &Restarted;
	if (!Step({}))
	{
		return false;
	}
	ReadUI();
	TestEqual(TEXT("Restart lists the saved location"), Controls[TEXT("adm_location")]->GetStringField(TEXT("value")), FString(TEXT("Test spot")));
	TestTrue(TEXT("Go is enabled after the restart"), Enabled(TEXT("adm_go")));
	TestFalse(TEXT("Attackable starts unchecked"), Controls[TEXT("adm_attackable")]->GetBoolField(TEXT("value")));
	if (!Step({Event(TEXT("click"), TEXT("adm_go"), nullptr)}))
	{
		return false;
	}
	TestEqual(TEXT("Go after the restart"), ChatSent(), TArray<FString>{Teleloc});

	// Delete removes it from state.
	if (!Step({Event(TEXT("click"), TEXT("adm_delete"), nullptr)}))
	{
		return false;
	}
	ReadUI();
	TestEqual(TEXT("No saved locations"), Controls[TEXT("adm_location")]->GetStringField(TEXT("value")), FString(TEXT("No saved locations")));
	// An empty Lua table reaches the host as an empty object or array.
	const TSharedPtr<FJsonValue> Emptied = Input->GetObjectField(TEXT("state"))->TryGetField(TEXT("locations"));
	TestTrue(TEXT("State has no locations"), Emptied.IsValid()
		&& ((Emptied->Type == EJson::Object && Emptied->AsObject()->Values.IsEmpty()) || (Emptied->Type == EJson::Array && Emptied->AsArray().IsEmpty())));
	return true;
}
#endif
