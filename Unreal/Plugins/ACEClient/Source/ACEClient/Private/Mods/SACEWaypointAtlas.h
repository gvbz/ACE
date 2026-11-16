#pragma once
#include "CoreMinimal.h"
class SWidget;
class UACEPluginSubsystem;

TSharedRef<SWidget> MakeWaypointAtlas(UACEPluginSubsystem* Host,FString ImportStatus,FSimpleDelegate Back);
