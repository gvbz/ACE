#pragma once

#include "CoreMinimal.h"
#include "Engine/GameEngine.h"
#include "ACEGameEngine.generated.h"

/** Temporary front-end pacing; never changes the saved gameplay frame limit. */
namespace ACEFrontendFrameRate
{
    ACECLIENT_API float GetLimit(const UEngine& Engine,float GameplayLimit,bool Editor);
}

/** Used by editor previews to retain the same background behavior as the game. */
namespace ACEBackgroundActivity
{
    ACECLIENT_API bool HasClientWorld(const UEngine& Engine);
}

UCLASS()
class ACECLIENT_API UACEGameEngine : public UGameEngine
{
    GENERATED_BODY()
public:
    virtual void Init(IEngineLoop* InEngineLoop) override;
    virtual float GetMaxTickRate(float DeltaTime,bool bAllowFrameRateSmoothing=true) const override;
};
