#include "Personal/PersonalFarWindows.h"
#include "ACERuntimeOptions.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFarWindowsDraw(TEXT("personal.FarWindows.Draw"), -1,
		TEXT("-1 follows the Far Terrain Rooms check box, 0 off, 1 on: door panels and far rooms. Needs Far Terrain Objects."));
	TAutoConsoleVariable<int32> CVarFarWindowsKinds(TEXT("personal.FarWindows.Kinds"), 0,
		TEXT("1: a panel in every opening, near and far, doors red and every opening that gets a far room green, whatever the show rules. 0: back to normal."));
	TAutoConsoleVariable<int32> CVarFarWindowsLogRooms(TEXT("personal.FarWindows.LogRooms"), 0,
		TEXT("1 writes a log line per room behind a door panel: the frame its actor is first shown and the frame its mesh is first drawn."));
	TAutoConsoleVariable<float> CVarFarWindowsDoorDistance(TEXT("personal.FarWindows.DoorDistance"), 96.f,
		TEXT("A door panel stays until the player is within this many meters of it: the server sends the door object by distance."));
	TAutoConsoleVariable<FString> CVarFarWindowsDoorColor(TEXT("personal.FarWindows.DoorColor"), TEXT("55,31,16"),
		TEXT("The color of door panels by day, R,G,B."));

	FColor ParseColor(const FString& Text, FColor Fallback)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() != 3)
		{
			return Fallback;
		}
		return FColor(uint8(FMath::Clamp(FCString::Atoi(*Parts[0]), 0, 255)), uint8(FMath::Clamp(FCString::Atoi(*Parts[1]), 0, 255)),
			uint8(FMath::Clamp(FCString::Atoi(*Parts[2]), 0, 255)), 255);
	}
}

bool PersonalFarWindows::IsOn()
{
	const int32 Draw = CVarFarWindowsDraw.GetValueOnGameThread();
	return Draw != 0 && (Draw > 0 || ACERuntimeOptions::Get(OptionKey) > 0.5f) && PersonalFarScenery::IsOn();
}

bool PersonalFarWindows::ShowKinds()
{
	return CVarFarWindowsKinds.GetValueOnGameThread() != 0;
}

bool PersonalFarWindows::LogRooms()
{
	return CVarFarWindowsLogRooms.GetValueOnGameThread() != 0;
}

float PersonalFarWindows::DoorDistance()
{
	return FMath::Clamp(CVarFarWindowsDoorDistance.GetValueOnGameThread(), 0.f, 600.f);
}

FColor PersonalFarWindows::DoorColor()
{
	return ParseColor(CVarFarWindowsDoorColor.GetValueOnGameThread(), FColor(55, 31, 16));
}
