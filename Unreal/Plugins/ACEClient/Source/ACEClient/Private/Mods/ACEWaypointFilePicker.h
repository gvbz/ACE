#pragma once

#include "CoreMinimal.h"

namespace ACEWaypointFilePicker
{
    using FComplete=TFunction<void(const FString& Path,const FString& Error,bool Cancelled)>;

    // Completion runs on the game thread. Returns false, without completion,
    // if another Waypoint picker is already open. Picking never imports a file.
    bool Open(const FString& InitialPath,FComplete Complete);
    bool IsOpen();

#if WITH_DEV_AUTOMATION_TESTS
    using FPicker=TFunction<void(const FString& InitialPath,FComplete Complete)>;
    void SetTestPicker(FPicker Picker);
#endif
}
