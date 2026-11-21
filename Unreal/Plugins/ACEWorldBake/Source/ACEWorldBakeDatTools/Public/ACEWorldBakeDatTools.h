#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Engine/Texture.h"
#include "Modules/ModuleInterface.h"
#include "ACEWorldBakeDatTools.generated.h"

class UTexture2D;

ACEWORLDBAKEDATTOOLS_API DECLARE_LOG_CATEGORY_EXTERN(LogACEWorldBakeDatTools, All, All);

#define ACEWORLDBAKEDATTOOLS_LOG_PREFIX TEXT("ACEWorldBakeDatTools: ")
#define UE_LOG_ACEWORLDBAKEDATTOOLS(Verbosity, Format, ...) \
{ \
	UE_LOG(LogACEWorldBakeDatTools, Verbosity, TEXT("%s%s"), ACEWORLDBAKEDATTOOLS_LOG_PREFIX, *FString::Printf(Format, ##__VA_ARGS__)); \
}

const uint32 kSceneLandblockWidth = 16;

UENUM(BlueprintType)
namespace EACEWorldBakeDatFile
{
	enum Type
	{
		Cell,
		Highres,
		LocaleEnglish,
		Portal,
	};
}

UENUM(BlueprintType)
namespace EACEWorldBakeResource
{
	enum Type
	{
		Model,
		Setup,
		Animation,
		Palette,
		ImageTexture,
		ImageColor,
		Surface,
		MotionTable,
		Sound,
		Environment,
		Scene,
		Region,
		SoundTable,
		EnumMapper,
		ParticleEmitter,
		PhysicsScript,
		PhysicsScriptTable,
		Invalid
	};
}

struct ACEWORLDBAKEDATTOOLS_API FACEWorldBakeResourceLocation
{
	FACEWorldBakeResourceLocation();

	FACEWorldBakeResourceLocation(const FACEWorldBakeResourceLocation& Other);

	FACEWorldBakeResourceLocation& operator=(const FACEWorldBakeResourceLocation& Other);

	//
	float OffsetX;
	//
	float OffsetY;
	//
	float OffsetZ;
	//
	float HeadingW;
	//
	float HeadingA;
	//
	float HeadingB;
	//
	float HeadingC;
	//
};

typedef TArray<FACEWorldBakeResourceLocation> TResourceLocationArray;

typedef TMap<uint32, TSharedPtr<TResourceLocationArray>> TResourceLocationsMap;

struct ACEWORLDBAKEDATTOOLS_API FACEWorldBakeScaledResourceLocation : public FACEWorldBakeResourceLocation
{
	FACEWorldBakeScaledResourceLocation();

	FACEWorldBakeScaledResourceLocation(const FACEWorldBakeResourceLocation& InResourceLocation, const FVector3f& InScale);

	//
	FVector3f Scale;
};

typedef TArray<FACEWorldBakeScaledResourceLocation> TScaledResourceLocationArray;

typedef TMap<uint32, TSharedPtr<TScaledResourceLocationArray>> TScaledResourceLocationsMap;

struct ACEWORLDBAKEDATTOOLS_API FACEWorldBakeResourceInfo
{
	FACEWorldBakeResourceInfo();

	FACEWorldBakeResourceInfo(uint32 InResourceId, const FACEWorldBakeResourceLocation& InLocation);

	//
	uint32 ResourceId;
	//
	FACEWorldBakeResourceLocation Location;
};

typedef TArray<FACEWorldBakeResourceInfo> TResourceInfoArray;

struct ACEWORLDBAKEDATTOOLS_API FACEWorldBakeScaledResourceInfo : public FACEWorldBakeResourceInfo
{
	FACEWorldBakeScaledResourceInfo();

	FACEWorldBakeScaledResourceInfo(const FACEWorldBakeResourceInfo& Resourceinfo, const FVector3f& InScale = FVector3f::OneVector);

	FACEWorldBakeScaledResourceInfo(uint32 InResourceId, const FACEWorldBakeResourceLocation& InLocation, const FVector3f& InScale);

	FACEWorldBakeScaledResourceInfo(uint32 InResourceId, const FACEWorldBakeScaledResourceLocation& InLocation);

	//
	FVector3f Scale;
};

typedef TArray<FACEWorldBakeScaledResourceInfo> TScaledResourceInfoArray;

typedef TArray<uint8> TByteArray;
typedef TSharedPtr<TByteArray, ESPMode::ThreadSafe> TByteArrayPtr;
typedef TSharedPtr<class FACEWorldBakeDatFile, ESPMode::ThreadSafe> FACEWorldBakeDatFilePtr;

class ACEWORLDBAKEDATTOOLS_API IACEWorldBakeDatToolsModule : public IModuleInterface
{
public:
	virtual void GetDatFileDirectory(FString& OutPath, bool& OutHasCell, bool& OutHasHighRes, bool& OutHasLocale, bool& OutHasPortal) = 0;

	virtual void SetDatFileDirectory(const FString& Path) = 0;

	virtual void DumpDatFileStatsToLog(EACEWorldBakeDatFile::Type DatFileType, bool bDumpFileEntries) = 0;

	virtual FACEWorldBakeDatFilePtr GetDatFile(EACEWorldBakeDatFile::Type DatFileType) = 0;

	virtual void ExportLandblocks() = 0;

	virtual void ExportLandblockInfo(uint32 LandblockX, uint32 LandblockY) = 0;

	virtual bool ExportResource(EACEWorldBakeResource::Type ResourceType, uint32 Identifier) = 0;

	virtual int32 ExportResources(EACEWorldBakeResource::Type ResourceType, uint32 IdentifierMask) = 0;

	virtual bool CorrectPortalImageTextureSettings(UTexture2D* Texture, uint32 Identifier = 0) = 0;

	// todo: apis for retrieving name-standards from ini settings
	// i.e. intermediate save locations, file patterns, scaling, etc.
};

UCLASS()
class UACEWorldBakeExportResourcesCommandlet : public UCommandlet
{
	GENERATED_BODY()

	/** Parsed commandline tokens */
	TArray<FString> Tokens;

	/** Parsed commandline switches */
	TArray<FString> Switches;

public:
	UACEWorldBakeExportResourcesCommandlet();

public:
	virtual int32 Main(const FString& Params) override;
};

bool SaveImageToFile(const TArray<FColor>& ImageColors, int32 Width, int32 Height, const FString& FileName, const TCHAR* SavedSubDir = TEXT("/Images/"));

template<typename INT_TYPE>
void ArrayToString(const TArray<INT_TYPE>& Array, FString& String)
{
	const int32 Count = Array.Num();
	for (int32 i = 0; i < Count; ++i)
	{
		if (i > 0)
		{
			String += TEXT(",");
		}
		String += FString::Printf(TEXT("%d"), Array[i]);
	}
}

template<typename INT_TYPE>
void ArrayToHexString(const TArray<INT_TYPE>& Array, FString& String)
{
	const int32 Count = Array.Num();
	for (int32 i = 0; i < Count; ++i)
	{
		if (i > 0)
		{
			String += TEXT(",");
		}
		String += FString::Printf(TEXT("0x%08X"), Array[i]);
	}
}

void ArrayToString(const TArray<FString>& Array, FString& String);
void ArrayToString(const TArray<float>& Array, FString& String);

FACEWorldBakeResourceLocation SumResourceLocation(const FACEWorldBakeResourceLocation& Left, const FACEWorldBakeResourceLocation& Right);
