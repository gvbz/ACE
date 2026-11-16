#include "Personal/PersonalFarRooms.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFarWindowsRooms(TEXT("personal.FarWindows.Rooms"), -1,
		TEXT("Far rooms: -1 follows personal.FarWindows.Draw (the Far Terrain Rooms check box), 1 draws the rooms behind every opening of a building that is not a door, 0 off. Needs Far Terrain Objects."));
	TAutoConsoleVariable<float> CVarFarWindowsRoomsBudgetMs(TEXT("personal.FarWindows.RoomsBudgetMs"), 1.f,
		TEXT("Game thread time per update (10 per second) for making far room meshes, in ms."));
}

bool PersonalFarRooms::IsOn()
{
	const int32 Rooms = CVarFarWindowsRooms.GetValueOnGameThread();
	return Rooms != 0 && (Rooms > 0 || PersonalFarWindows::IsOn()) && PersonalFarScenery::IsOn();
}

float PersonalFarRooms::BudgetMs()
{
	return FMath::Clamp(CVarFarWindowsRoomsBudgetMs.GetValueOnGameThread(), 0.1f, 100.f);
}

namespace
{
	TAutoConsoleVariable<float> CVarFarWindowsRoomFogNear(TEXT("personal.FarWindows.RoomFogNear"), 200.f,
		TEXT("By day, far rooms take no fog out to this many meters from the camera."));
	TAutoConsoleVariable<float> CVarFarWindowsRoomFogFar(TEXT("personal.FarWindows.RoomFogFar"), 600.f,
		TEXT("By day, far rooms take all of the distance fog from this many meters on."));
}

namespace
{
	TAutoConsoleVariable<int32> CVarFarWindowsRoomsAlways(TEXT("personal.FarWindows.RoomsAlways"), 0,
		TEXT("1: far rooms are shown in rings 0 and 1 too, whatever rooms the client draws (to look for the cause of a flicker). 0: the show rule."));
	TAutoConsoleVariable<int32> CVarFarWindowsLogFlicker(TEXT("personal.FarWindows.LogFlicker"), 0,
		TEXT("1: a log line, with the frame number, for every change of a far-room component's visibility or of the client rooms behind it, within 600 m of the camera."));
}

namespace
{
	TAutoConsoleVariable<float> CVarFarWindowsRoomLift(TEXT("personal.FarWindows.RoomLift"), 3.f,
		TEXT("Far rooms stand this many centimeters higher than the client's rooms, so their floor does not fight the ground seen through a doorway. Changes live."));
}

float PersonalFarRooms::RoomLiftCm()
{
	return FMath::Clamp(CVarFarWindowsRoomLift.GetValueOnGameThread(), 0.f, 100.f);
}

bool PersonalFarRooms::RoomsAlways()
{
	return CVarFarWindowsRoomsAlways.GetValueOnGameThread() != 0;
}

bool PersonalFarRooms::LogFlicker()
{
	return CVarFarWindowsLogFlicker.GetValueOnGameThread() != 0;
}


float PersonalFarRooms::RoomFogNear()
{
	return FMath::Clamp(CVarFarWindowsRoomFogNear.GetValueOnGameThread(), 0.f, 5000.f);
}

float PersonalFarRooms::RoomFogFar()
{
	return FMath::Clamp(CVarFarWindowsRoomFogFar.GetValueOnGameThread(), 0.f, 5000.f);
}
