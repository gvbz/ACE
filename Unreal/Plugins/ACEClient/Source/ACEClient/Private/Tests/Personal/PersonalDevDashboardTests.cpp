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
	const TArray<FString> Names = {
		TEXT("personal.FarTerrain.Wireframe"), TEXT("personal.FarTerrain.Freeze"), TEXT("personal.FarWindows.Kinds"),
		TEXT("personal.FarScenery.Fade"), TEXT("personal.FarScenery.SizeFactor"), TEXT("personal.FarScenery.Detail"),
		TEXT("personal.FarScenery.MiddleRing"), TEXT("personal.FarScenery.SimplestRing"), TEXT("personal.FarScenery.Group"),
		TEXT("personal.FarScenery.BudgetMs"), TEXT("personal.FarScenery.Radius"), TEXT("personal.FarWindows.Rooms"),
		TEXT("personal.FarWindows.Draw"), TEXT("personal.FarWindows.RoomFogNear"), TEXT("personal.FarWindows.RoomFogFar"),
		TEXT("personal.FarWindows.RoomLift"), TEXT("personal.FarWindows.DoorDistance"), TEXT("personal.FarWindows.RoomsAlways")};
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
	auto Requests = [&]()
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
