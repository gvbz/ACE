#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "Interfaces/IPluginManager.h"
#include "Personal/PersonalDevService.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginStyle.h"
#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Tests/ACETestDatDirectory.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Dom/JsonObject.h"

// Draws the Development Dashboard in the real companion panel, offscreen, one image per tab,
// at Lua Plugin Text Scale 1 and 1.5, and checks that its controls line up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalDevDashboardLayoutTest, "ACE.Personal.DevDashboardLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalDevDashboardLayoutTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Needs a renderer (-RenderOffscreen); skipped."));
		return true;
	}
	const TSharedPtr<IPlugin> Client = IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
	FString Source, Error;
	if (!TestTrue(TEXT("ACEClient plugin"), Client.IsValid())
		|| !TestTrue(TEXT("Lua exists"), FFileHelper::LoadFileToString(Source, *(Client->GetBaseDir() / TEXT("ClientMods/devdash/main.lua")))))
	{
		return false;
	}

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	UGameInstance* GI = NewObject<UGameInstance>(GEngine);
	World->SetGameInstance(GI);
	Context.OwningGameInstance = GI;
	Context.SetCurrentWorld(World);
	GI->OnWorldChanged(nullptr, World);
	GI->Init();
	ON_SCOPE_EXIT
	{
		GI->Shutdown();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	UACEPluginSubsystem* Host = GI->GetSubsystem<UACEPluginSubsystem>();
	GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(ACETestDatDirectory());

	// The bundled plugin, shown as if it were running; put back at the end.
	TSharedPtr<FACEClientPlugin> Plugin;
	for (const TSharedPtr<FACEClientPlugin>& Candidate : Host->Plugins)
	{
		if (Candidate->Id == TEXT("devdash"))
		{
			Plugin = Candidate;
		}
	}
	const bool bAdded = !Plugin.IsValid();
	if (bAdded)
	{
		Plugin = MakeShared<FACEClientPlugin>();
		Plugin->Id = TEXT("devdash");
		Plugin->Api = 2;
		Plugin->Companion = true;
		Host->Plugins.Add(Plugin);
	}
	const bool bWasEnabled = Plugin->Enabled, bWasRunning = Plugin->Running;
	const TArray<FString> OldPermissions = Plugin->Permissions;
	Plugin->Enabled = Plugin->Running = true;
	Plugin->Permissions.AddUnique(TEXT("dev_tools"));
	Plugin->Permissions.AddUnique(TEXT("ui"));
	float OldScale = 1.f;
	const bool bHadScale = GConfig && GConfig->GetFloat(TEXT("ACE.Presentation"), TEXT("PluginTextScale"), OldScale, GGameUserSettingsIni);
	ON_SCOPE_EXIT
	{
		if (bHadScale)
		{
			ACERuntimeOptions::Set(TEXT("PluginTextScale"), OldScale);
		}
		else
		{
			ACERuntimeOptions::Set(TEXT("PluginTextScale"), 1.f);
			if (GConfig)
			{
				GConfig->RemoveKey(TEXT("ACE.Presentation"), TEXT("PluginTextScale"), GGameUserSettingsIni);
			}
		}
		Plugin->PanelRows.Reset();
		++Plugin->PanelRevision;
		Plugin->Enabled = bWasEnabled;
		Plugin->Running = bWasRunning;
		Plugin->Permissions = OldPermissions;
		if (bAdded)
		{
			Host->Plugins.Remove(Plugin);
		}
	};

	// The dashboard's UI after start: the client's values, one saved location, and "ar" typed in Place with the place list known.
	FACEPluginVM VM;
	if (!TestTrue(TEXT("Dashboard Lua loads"), VM.Load(Source, Error)))
	{
		AddError(Error);
		return false;
	}
	auto Parse = [](const FString& Text)
	{
		TSharedPtr<FJsonObject> J;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J);
		return J;
	};
	TSharedPtr<FJsonObject> Input = Parse(TEXT(R"({"player":1234,"time":0,"events":[],
		"state":{"locations":[{"name":"Test spot","cell":2847145946,"x":84.5,"y":-12.25,"z":42.005}]},
		"position":{"cell":2847145946,"x":84.5,"y":-12.25,"z":42.005}})"));
	TSharedPtr<FJsonObject> Profile = MakeShared<FJsonObject>(), Out;
	double Clock = 0;
	auto Step = [&](const TArray<TSharedPtr<FJsonValue>>& Events)
	{
		Input->SetNumberField(TEXT("time"), Clock += 1);
		Input->SetArrayField(TEXT("events"), Events);
		Out.Reset();
		if (!VM.Step(Input, Profile, Out, Error) || !Out.IsValid())
		{
			AddError(TEXT("Dashboard Lua: ") + Error);
			return false;
		}
		Input->SetArrayField(TEXT("chat_events"), {});
		return true;
	};
	if (!Step({}))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Replies;
	for (const TSharedPtr<FJsonValue>& Value : Out->GetArrayField(TEXT("requests")))
	{
		const TSharedPtr<FJsonObject> Request = Value->AsObject();
		TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
		FString Failure;
		const bool bOk = PersonalDevService::Handle(*Plugin, Request, Reply, Failure, nullptr);
		Reply->SetStringField(TEXT("type"), TEXT("result"));
		Reply->SetStringField(TEXT("id"), Request->GetStringField(TEXT("id")));
		Reply->SetBoolField(TEXT("ok"), bOk);
		Reply->SetStringField(TEXT("error"), Failure);
		Replies.Add(MakeShared<FJsonValueObject>(Reply));
	}
	if (!Step(Replies))
	{
		return false;
	}
	// The id of the Place input (it changes when a suggestion is picked).
	TFunction<FString(const TArray<TSharedPtr<FJsonValue>>&)> FindPlace = [&](const TArray<TSharedPtr<FJsonValue>>& Rows) -> FString
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			const TSharedPtr<FJsonObject> R = Row->AsObject();
			FString Id = R->GetStringField(TEXT("id")), Type;
			R->TryGetStringField(TEXT("type"), Type);
			if (Type == TEXT("input") && Id.StartsWith(TEXT("adm_place")))
			{
				return Id;
			}
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (R->TryGetArrayField(TEXT("children"), Children))
			{
				const FString Found = FindPlace(*Children);
				if (!Found.IsEmpty())
				{
					return Found;
				}
			}
		}
		return FString();
	};
	{
		const FString PlaceId = FindPlace(Out->GetArrayField(TEXT("ui")));
		TSharedPtr<FJsonObject> Chat = Parse(TEXT(R"({"serial":1,"time":1,"sender":"","sender_type":"server","chat_type":0,"color":0,
			"raw_text":"All POIs: Ahurenga, Al-Arqas, Arwic, Kara, Marketplace, Qalabar, Sanamar, Xarabydun, Yaraq",
			"text":"All POIs: Ahurenga, Al-Arqas, Arwic, Kara, Marketplace, Qalabar, Sanamar, Xarabydun, Yaraq"})"));
		Input->SetArrayField(TEXT("chat_events"), {MakeShared<FJsonValueObject>(Chat)});
		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("type"), TEXT("change"));
		Change->SetStringField(TEXT("id"), PlaceId);
		Change->SetStringField(TEXT("value"), TEXT("ar"));
		if (!Step({MakeShared<FJsonValueObject>(Change)}))
		{
			return false;
		}
	}
	const TArray<TSharedPtr<FJsonValue>> UI = Out->GetArrayField(TEXT("ui"));
	Plugin->PanelRows = UI;

	// Controls in the UI tree, and the tab pages.
	TMap<FString, TSharedPtr<FJsonObject>> Json;
	TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> Index = [&](const TArray<TSharedPtr<FJsonValue>>& Rows)
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			const TSharedPtr<FJsonObject> R = Row->AsObject();
			Json.Add(R->GetStringField(TEXT("id")), R);
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (R->TryGetArrayField(TEXT("children"), Children))
			{
				Index(*Children);
			}
		}
	};
	Index(UI);
	auto TypeOf = [](const TSharedPtr<FJsonObject>& R)
	{
		FString Type = TEXT("text");
		R->TryGetStringField(TEXT("type"), Type);
		return Type;
	};
	auto Children = [](const TSharedPtr<FJsonObject>& R)
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		return R->TryGetArrayField(TEXT("children"), List) ? *List : TArray<TSharedPtr<FJsonValue>>();
	};
	// Groups of controls that start a line, per container: the tab itself and each section in it.
	// A row stands for its first child; a section starts a line and is a container too.
	TFunction<void(const TArray<TSharedPtr<FJsonValue>>&, const FString&, TArray<TPair<FString, TArray<FString>>>&)> Columns =
		[&](const TArray<TSharedPtr<FJsonValue>>& Rows, const FString& Name, TArray<TPair<FString, TArray<FString>>>& Groups)
	{
		TArray<FString> Ids;
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			const TSharedPtr<FJsonObject> R = Row->AsObject();
			const FString Type = TypeOf(R);
			if (Type == TEXT("row"))
			{
				const TArray<TSharedPtr<FJsonValue>> Inner = Children(R);
				if (Inner.Num() > 0)
				{
					Ids.Add(Inner[0]->AsObject()->GetStringField(TEXT("id")));
				}
			}
			else
			{
				Ids.Add(R->GetStringField(TEXT("id")));
				if (Type == TEXT("section"))
				{
					Columns(Children(R), R->GetStringField(TEXT("id")), Groups);
				}
			}
			if (Type == TEXT("row"))
			{
				for (const TSharedPtr<FJsonValue>& Inner : Children(R))
				{
					if (TypeOf(Inner->AsObject()) == TEXT("section"))
					{
						Columns(Children(Inner->AsObject()), Inner->AsObject()->GetStringField(TEXT("id")), Groups);
					}
				}
			}
		}
		Groups.Insert(TPair<FString, TArray<FString>>(Name, Ids), 0);
	};
	TArray<FString> Pages;
	for (const TSharedPtr<FJsonValue>& Page : Children(Json[TEXT("tabs")]))
	{
		Pages.Add(Page->AsObject()->GetStringField(TEXT("id")));
	}

	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Automation/DevDashboard");
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FVector2D Size(710, 1160);
	for (const float Scale : {1.f, 1.5f})
	{
		ACERuntimeOptions::Set(TEXT("PluginTextScale"), Scale);
		const FString Suffix = Scale == 1.f ? TEXT("100") : TEXT("150");
		++Plugin->PanelRevision;
		TSharedRef<SWidget> Panel = Host->MakeCompanionPanel(Plugin->Id);
		TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>, FName)> Find = [&](TSharedRef<SWidget> Widget, FName Tag) -> TSharedPtr<SWidget>
		{
			if (Widget->GetTag() == Tag)
			{
				return Widget;
			}
			FChildren* List = Widget->GetChildren();
			for (int32 I = 0; I < List->Num(); ++I)
			{
				if (TSharedPtr<SWidget> Found = Find(List->GetChildAt(I), Tag))
				{
					return Found;
				}
			}
			return nullptr;
		};
		auto Box = [&](const FString& Id, FBox2D& Result)
		{
			const TSharedPtr<SWidget> Widget = Find(Panel, FName(*(TEXT("PluginControl.") + Id)));
			if (!Widget.IsValid())
			{
				return false;
			}
			const FGeometry& Geometry = Widget->GetPaintSpaceGeometry();
			const FVector2D Position = Geometry.GetAbsolutePosition(), Extent = Geometry.GetAbsoluteSize();
			Result = FBox2D(Position, Position + Extent);
			return Extent.X > 0 && Extent.Y > 0;
		};
		FWidgetRenderer Renderer(false, true);
		UTextureRenderTarget2D* Texture = FWidgetRenderer::CreateTargetFor(Size, TF_Bilinear, true);
		for (const FString& Page : Pages)
		{
			Renderer.DrawWidget(Texture, Panel, Size, 0);
			FlushRenderingCommands();
			const TSharedPtr<SACEPluginTab> Tab = StaticCastSharedPtr<SACEPluginTab>(Find(Panel, FName(*(TEXT("PluginTab.tabs.") + Page))));
			if (!TestTrue(*(TEXT("Tab button ") + Page), Tab.IsValid()))
			{
				continue;
			}
			Tab->SimulateClick();
			for (int32 I = 0; I < 3; ++I)
			{
				Renderer.DrawWidget(Texture, Panel, Size, 0);
				FlushRenderingCommands();
			}
			TArray<FColor> Pixels;
			FReadSurfaceDataFlags Flags;
			Flags.SetLinearToGamma(false);
			Texture->GameThread_GetRenderTargetResource()->ReadPixels(Pixels, Flags);
			TArray64<uint8> PNG;
			FImageUtils::PNGCompressImageArray(int32(Size.X), int32(Size.Y), Pixels, PNG);
			FFileHelper::SaveArrayToFile(PNG, *(Folder / (Page + TEXT("_") + Suffix + TEXT(".png"))));

			// Left edges: per container, every control that starts a line; Log frame rate with the tab's own.
			TArray<TPair<FString, TArray<FString>>> Groups;
			Columns(Children(Json[Page]), Page, Groups);
			Groups[0].Value.Add(TEXT("log_frame_rate"));
			for (const TPair<FString, TArray<FString>>& Group : Groups)
			{
				double Min = TNumericLimits<double>::Max(), Max = TNumericLimits<double>::Lowest();
				FString Line;
				for (const FString& Id : Group.Value)
				{
					FBox2D B;
					if (!Box(Id, B))
					{
						continue;
					}
					Min = FMath::Min(Min, B.Min.X);
					Max = FMath::Max(Max, B.Min.X);
					Line += FString::Printf(TEXT(" %s=%.1f"), *Id, B.Min.X);
				}
				AddInfo(FString::Printf(TEXT("Left edges, scale %s, %s / %s:%s"), *Suffix, *Page, *Group.Key, *Line));
				if (Max >= Min && Max - Min > 1.)
				{
					AddError(FString::Printf(TEXT("Left edges differ by %.1f px (scale %s, %s / %s)"), Max - Min, *Suffix, *Page, *Group.Key));
				}
			}
			// Reset tab is one line high: the same height as Log frame rate.
			FBox2D Reset, Frame;
			if (Box(TEXT("reset_") + Page, Reset) && Box(TEXT("log_frame_rate"), Frame))
			{
				AddInfo(FString::Printf(TEXT("Heights, scale %s, %s: Reset tab %.1f, Log frame rate %.1f"), *Suffix, *Page, Reset.GetSize().Y, Frame.GetSize().Y));
				TestTrue(*FString::Printf(TEXT("Reset tab is one line (scale %s, %s)"), *Suffix, *Page), FMath::Abs(Reset.GetSize().Y - Frame.GetSize().Y) <= 1.);
			}
			// Bottoms of the box and the buttons in the Location, Name and Place rows.
			for (const TCHAR* RowId : {TEXT("adm_location_row"), TEXT("adm_name_row"), TEXT("adm_place_row")})
			{
				const TSharedPtr<FJsonObject>* Row = Json.Find(RowId);
				if (Page != TEXT("admin") || !Row)
				{
					continue;
				}
				double Min = TNumericLimits<double>::Max(), Max = TNumericLimits<double>::Lowest();
				FString Line;
				for (const TSharedPtr<FJsonValue>& Child : Children(*Row))
				{
					const FString Id = Child->AsObject()->GetStringField(TEXT("id"));
					FBox2D B;
					if (!Box(Id, B))
					{
						continue;
					}
					Min = FMath::Min(Min, B.Max.Y);
					Max = FMath::Max(Max, B.Max.Y);
					Line += FString::Printf(TEXT(" %s bottom=%.1f height=%.1f"), *Id, B.Max.Y, B.GetSize().Y);
					// The box under a select's or input's label.
					TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>)> Inner = [&](TSharedRef<SWidget> Widget) -> TSharedPtr<SWidget>
					{
						const FString Type = Widget->GetType().ToString();
						if (Type.Contains(TEXT("ComboBox")) || Type.Contains(TEXT("EditableTextBox")))
						{
							return Widget;
						}
						FChildren* List = Widget->GetChildren();
						for (int32 I = 0; I < List->Num(); ++I)
						{
							if (TSharedPtr<SWidget> Found = Inner(List->GetChildAt(I)))
							{
								return Found;
							}
						}
						return nullptr;
					};
					if (const TSharedPtr<SWidget> Control = Find(Panel, FName(*(TEXT("PluginControl.") + Id))))
					{
						if (const TSharedPtr<SWidget> Field = Inner(Control.ToSharedRef()))
						{
							const FGeometry& G = Field->GetPaintSpaceGeometry();
							Line += FString::Printf(TEXT(" (box %.1f high, bottom %.1f)"), G.GetAbsoluteSize().Y, G.GetAbsolutePosition().Y + G.GetAbsoluteSize().Y);
						}
					}
				}
				AddInfo(FString::Printf(TEXT("Bottoms, scale %s, %s:%s"), *Suffix, RowId, *Line));
				if (Max >= Min && Max - Min > 1.)
				{
					AddError(FString::Printf(TEXT("Bottoms differ by %.1f px (scale %s, %s)"), Max - Min, *Suffix, RowId));
				}
			}
		}
		Texture->ReleaseResource();
	}

	// The row option valign: without it a row centers its children (as before); with "bottom" their bottoms line up.
	ACERuntimeOptions::Set(TEXT("PluginTextScale"), 1.f);
	{
		const TSharedPtr<FJsonObject> Rows = Parse(TEXT(R"({"ui":[
			{"id":"v_center","type":"row","children":[{"id":"vc_select","type":"select","text":"Label","options":["A"],"value":"A"},{"id":"vc_button","type":"button","text":"Button"}]},
			{"id":"v_bottom","type":"row","valign":"bottom","children":[{"id":"vb_select","type":"select","text":"Label","options":["A"],"value":"A"},{"id":"vb_button","type":"button","text":"Button"}]}]})"));
		Plugin->PanelRows = Rows->GetArrayField(TEXT("ui"));
		++Plugin->PanelRevision;
		TSharedRef<SWidget> Panel = Host->MakeCompanionPanel(Plugin->Id);
		TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>, FName)> Find = [&](TSharedRef<SWidget> Widget, FName Tag) -> TSharedPtr<SWidget>
		{
			if (Widget->GetTag() == Tag)
			{
				return Widget;
			}
			FChildren* List = Widget->GetChildren();
			for (int32 I = 0; I < List->Num(); ++I)
			{
				if (TSharedPtr<SWidget> Found = Find(List->GetChildAt(I), Tag))
				{
					return Found;
				}
			}
			return nullptr;
		};
		FWidgetRenderer Renderer(false, true);
		const FVector2D Small(400, 200);
		UTextureRenderTarget2D* Texture = FWidgetRenderer::CreateTargetFor(Small, TF_Bilinear, true);
		for (int32 I = 0; I < 3; ++I)
		{
			Renderer.DrawWidget(Texture, Panel, Small, 0);
			FlushRenderingCommands();
		}
		auto Geometry = [&](const TCHAR* Id)
		{
			const TSharedPtr<SWidget> Widget = Find(Panel, FName(*(FString(TEXT("PluginControl.")) + Id)));
			if (!Widget.IsValid())
			{
				return FBox2D(ForceInit);
			}
			const FGeometry& G = Widget->GetPaintSpaceGeometry();
			return FBox2D(G.GetAbsolutePosition(), G.GetAbsolutePosition() + G.GetAbsoluteSize());
		};
		const FBox2D CenterSelect = Geometry(TEXT("vc_select")), CenterButton = Geometry(TEXT("vc_button"));
		const FBox2D BottomSelect = Geometry(TEXT("vb_select")), BottomButton = Geometry(TEXT("vb_button"));
		AddInfo(FString::Printf(TEXT("Row without valign: select %.1f-%.1f, button %.1f-%.1f; valign bottom: select %.1f-%.1f, button %.1f-%.1f"),
			CenterSelect.Min.Y, CenterSelect.Max.Y, CenterButton.Min.Y, CenterButton.Max.Y, BottomSelect.Min.Y, BottomSelect.Max.Y, BottomButton.Min.Y, BottomButton.Max.Y));
		TestTrue(TEXT("Without valign the children are centered"), FMath::Abs(CenterSelect.GetCenter().Y - CenterButton.GetCenter().Y) <= 1.
			&& CenterButton.Max.Y < CenterSelect.Max.Y - 1.);
		TestTrue(TEXT("With valign bottom the bottoms line up"), FMath::Abs(BottomSelect.Max.Y - BottomButton.Max.Y) <= 1.);
		Texture->ReleaseResource();
	}
	return true;
}
#endif
