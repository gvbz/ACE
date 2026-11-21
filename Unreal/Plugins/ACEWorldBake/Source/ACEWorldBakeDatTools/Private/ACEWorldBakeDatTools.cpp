#include "ACEWorldBakeDatTools.h"
#include "ACEWorldBakeDatFile.h"
#include "ACEWorldBakeCellFile.h"
#include "ACEWorldBakeCsvWriter.h"
#include "ACEWorldBakePortalFile.h"
#include "ACEWorldBakeDatEnvironment.h"
#include "ACEWorldBakeDatImageTexture.h"
#include "ACEWorldBakeDatModel.h"
#include "ACEWorldBakeDatPalette.h"
#include "ACEWorldBakeDatRegion.h"
#include "ACEWorldBakeDatScene.h"
#include "ACEWorldBakeDatSetup.h"
#include "ACEWorldBakeDatSound.h"
#include "ACEWorldBakeDatSurface.h"
#include "ACEWorldBakeFbxWriter.h"
#include "ACEWorldBakeLandscape.h"
#include "ACEWorldBakeT3dWriter.h"
#include "Audio.h"
#include "CoreGlobals.h"
#include "DDSFile.h"
#include "Engine/Texture2D.h"
#include "Exporters/Exporter.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogACEWorldBakeDatTools);

#define LOCTEXT_NAMESPACE "ACEWorldBakeDatTools"

extern "C" void DecompressBlockBC1(uint32 x, uint32 y, uint32 stride, const uint8* blockStorage, uint8* image);
extern "C" void DecompressBlockBC2(uint32 x, uint32 y, uint32 stride, const uint8* blockStorage, uint8* image);
extern "C" void DecompressBlockBC3(uint32 x, uint32 y, uint32 stride, const uint8* blockStorage, uint8* image);

#define ACUNREAL_MAKEFOURCC(ch0, ch1, ch2, ch3)\
	((uint32)(uint8)(ch0) | ((uint32)(uint8)(ch1) << 8) |\
	((uint32)(uint8)(ch2) << 16) | ((uint32)(uint8)(ch3) << 24 ))

#define ACUNREAL_mmioFOURCC(ch0, ch1, ch2, ch3)\
	ACUNREAL_MAKEFOURCC(ch0, ch1, ch2, ch3)

#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(push, 2)
#endif

struct FACEWorldBakeRiffWaveHeaderChunk
{
	uint32	rID;			// Contains 'RIFF'
	uint32	ChunkLen;		// Remaining length of the entire riff chunk (= file).
	uint32	wID;			// Form type. Contains 'WAVE' for .wav files.
};

struct FACEWorldBakeRiffChunk
{
	uint32	ChunkID;		  // General data chunk ID like 'data', or 'fmt '
	uint32	ChunkLen;		  // Length of the rest of this chunk in bytes.
};

struct FACEWorldBakeRiffFormatChunk
{
	uint16   wFormatTag;        // Format type: 1 = PCM
	uint16   nChannels;         // Number of channels (i.e. mono, stereo...).
	uint32   nSamplesPerSec;    // Sample rate. 44100 or 22050 or 11025  Hz.
	uint32   nAvgBytesPerSec;   // For buffer estimation  = sample rate * BlockAlign.
	uint16   nBlockAlign;       // Block size of data = Channels times BYTES per sample.
	uint16   wBitsPerSample;    // Number of bits per sample of mono data.
	uint16   cbSize;            // The count in bytes of the size of extra information (after cbSize).
};

struct FACEWorldBakeRiffWave
{
	FACEWorldBakeRiffWaveHeaderChunk Header;

	FACEWorldBakeRiffChunk HeaderChunkFormat;

	FACEWorldBakeRiffFormatChunk ChunkFormat;

	FACEWorldBakeRiffChunk HeaderChunkData;

	uint8* ChunkData;

	FACEWorldBakeRiffWave(const FACEWorldBakePortalSoundWave& PortalSoundWave)
	{
		Header.rID = ACUNREAL_mmioFOURCC('R', 'I', 'F', 'F');
		Header.ChunkLen = PortalSoundWave.DataSize;
		Header.wID = ACUNREAL_mmioFOURCC('W', 'A', 'V', 'E');

		HeaderChunkFormat.ChunkID = ACUNREAL_mmioFOURCC('f', 'm', 't', ' ');
		HeaderChunkFormat.ChunkLen = sizeof(FACEWorldBakeRiffFormatChunk);

		ChunkFormat.wFormatTag = PortalSoundWave.FormatTag;
		ChunkFormat.nChannels = PortalSoundWave.Channels;
		ChunkFormat.nSamplesPerSec = PortalSoundWave.SamplesPerSec;
		ChunkFormat.nAvgBytesPerSec = PortalSoundWave.AvgBytesPerSec;
		ChunkFormat.nBlockAlign = PortalSoundWave.BlockAlign;
		ChunkFormat.wBitsPerSample = PortalSoundWave.BitsPerSample;
		ChunkFormat.cbSize = PortalSoundWave.BytesEx;

		HeaderChunkData.ChunkID = ACUNREAL_mmioFOURCC('d', 'a', 't', 'a');
		HeaderChunkData.ChunkLen = PortalSoundWave.DataSize;

		ChunkData = PortalSoundWave.WaveData;
	}
};

#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(pop)
#endif

enum EACEWorldBakeDDSFlags
{
	DDSF_Caps			= 0x00000001,
	DDSF_Height			= 0x00000002,
	DDSF_Width			= 0x00000004,
	DDSF_PixelFormat	= 0x00001000
};

enum EACEWorldBakeDDSCaps
{
	DDSC_CubeMap			= 0x00000200,
	DDSC_CubeMap_AllFaces	= 0x00000400 | 0x00000800 | 0x00001000 | 0x00002000 | 0x00004000 | 0x00008000,
	DDSC_Volume				= 0x00200000,
};

enum EACEWorldBakeDDSPixelFormat
{
	DDSPF_FourCC = 0x00000004,
	DDSPF_RGB = 0x00000040,
	DDSPF_DXT1 = ACUNREAL_MAKEFOURCC('D', 'X', 'T', '1'),
	DDSPF_DXT3 = ACUNREAL_MAKEFOURCC('D', 'X', 'T', '3'),
	DDSPF_DXT5 = ACUNREAL_MAKEFOURCC('D', 'X', 'T', '5'),
	DDSPF_DX10 = ACUNREAL_MAKEFOURCC('D', 'X', '1', '0')
};

// .DDS subheader.
#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(push,1)
#endif
struct FACEWorldBakeDDSPixelFormatHeader
{
	uint32 dwSize;
	uint32 dwFlags;
	uint32 dwFourCC;
	uint32 dwRGBBitCount;
	uint32 dwRBitMask;
	uint32 dwGBitMask;
	uint32 dwBBitMask;
	uint32 dwABitMask;
};
#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(pop)
#endif

// .DDS header.
#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(push,1)
#endif
struct FACEWorldBakeDDSFileHeader
{
	uint32 dwSize;
	uint32 dwFlags;
	uint32 dwHeight;
	uint32 dwWidth;
	uint32 dwLinearSize;
	uint32 dwDepth;
	uint32 dwMipMapCount;
	uint32 dwReserved1[11];
	FACEWorldBakeDDSPixelFormatHeader ddpf;
	uint32 dwCaps;
	uint32 dwCaps2;
	uint32 dwCaps3;
	uint32 dwCaps4;
	uint32 dwReserved2;
};
#if PLATFORM_SUPPORTS_PRAGMA_PACK
#pragma pack(pop)
#endif

const TCHAR* kACEWorldBakeIniDatToolsSection = TEXT("ACEWorldBakeDatTools");
const TCHAR* kACEWorldBakeIniDatFileDirectory = TEXT("DatFileDirectory");

struct FACEWorldBakeImageResourceSettings
{
	FACEWorldBakeImageResourceSettings()
	{
		CompressionSettings = TC_Default;
		Filter = TF_Default;
		MipGenSettings = TMGS_FromTextureGroup;
		SRGB = true;
	}

	friend bool operator==(const FACEWorldBakeImageResourceSettings& LHS, const UTexture2D& RHS);
	friend bool operator!=(const FACEWorldBakeImageResourceSettings& LHS, const UTexture2D& RHS);

	TextureCompressionSettings CompressionSettings;

	TextureFilter Filter;

	TextureMipGenSettings MipGenSettings;

	bool SRGB;
};

bool operator==(const FACEWorldBakeImageResourceSettings& LHS, const UTexture2D& RHS)
{
	const bool bCore = (LHS.CompressionSettings == RHS.CompressionSettings) && (LHS.Filter == RHS.Filter);
#if WITH_EDITORONLY_DATA
	return bCore && (LHS.MipGenSettings == RHS.MipGenSettings);
#else
	return bCore;
#endif
}

bool operator!=(const FACEWorldBakeImageResourceSettings& LHS, const UTexture2D& RHS)
{
	return !(LHS == RHS);
}

const EACEWorldBakeResource::Type kResourceTypes[] =
{
	EACEWorldBakeResource::Model,
	EACEWorldBakeResource::Setup,
	EACEWorldBakeResource::Animation,
	EACEWorldBakeResource::Palette,
	EACEWorldBakeResource::ImageTexture,
	EACEWorldBakeResource::ImageColor,
	EACEWorldBakeResource::Surface,
	EACEWorldBakeResource::MotionTable,
	EACEWorldBakeResource::Sound,
	EACEWorldBakeResource::Environment,
	EACEWorldBakeResource::Scene,
	EACEWorldBakeResource::Region,
	EACEWorldBakeResource::SoundTable,
	EACEWorldBakeResource::EnumMapper,
	EACEWorldBakeResource::ParticleEmitter,
	EACEWorldBakeResource::PhysicsScript,
	EACEWorldBakeResource::PhysicsScriptTable
};

static EACEWorldBakeResource_Native::Type ToNative(EACEWorldBakeResource::Type Type)
{
	switch (Type)
	{
	case EACEWorldBakeResource::Model: return EACEWorldBakeResource_Native::kModel;
	case EACEWorldBakeResource::Setup: return EACEWorldBakeResource_Native::kSetup;
	case EACEWorldBakeResource::Animation: return EACEWorldBakeResource_Native::kAnimation;
	case EACEWorldBakeResource::Palette: return EACEWorldBakeResource_Native::kPalette;
	case EACEWorldBakeResource::ImageTexture: return EACEWorldBakeResource_Native::kImgTex;
	case EACEWorldBakeResource::ImageColor: return EACEWorldBakeResource_Native::kImgColor;
	case EACEWorldBakeResource::Surface: return EACEWorldBakeResource_Native::kSurface;
	case EACEWorldBakeResource::MotionTable: return EACEWorldBakeResource_Native::kMotionTable;
	case EACEWorldBakeResource::Sound: return EACEWorldBakeResource_Native::kSound;
	case EACEWorldBakeResource::Environment: return EACEWorldBakeResource_Native::kEnvironment;
	case EACEWorldBakeResource::Scene: return EACEWorldBakeResource_Native::kScene;
	case EACEWorldBakeResource::Region: return EACEWorldBakeResource_Native::kRegion;
	case EACEWorldBakeResource::SoundTable: return EACEWorldBakeResource_Native::kSoundTable;
	case EACEWorldBakeResource::EnumMapper: return EACEWorldBakeResource_Native::kEnumMapper;
	case EACEWorldBakeResource::ParticleEmitter: return EACEWorldBakeResource_Native::kParticleEmitter;
	case EACEWorldBakeResource::PhysicsScript: return EACEWorldBakeResource_Native::kPhysicsScript;
	case EACEWorldBakeResource::PhysicsScriptTable: return EACEWorldBakeResource_Native::kPhysicsScriptTable;
	}

	return EACEWorldBakeResource_Native::kNone;
}

static EACEWorldBakeResource::Type ToACEWorldBake(EACEWorldBakeResource_Native::Type Type)
{
	switch (Type)
	{
	case EACEWorldBakeResource_Native::kModel: return EACEWorldBakeResource::Model;
	case EACEWorldBakeResource_Native::kSetup: return EACEWorldBakeResource::Setup;
	case EACEWorldBakeResource_Native::kAnimation: return EACEWorldBakeResource::Animation;
	case EACEWorldBakeResource_Native::kPalette: return EACEWorldBakeResource::Palette;
	case EACEWorldBakeResource_Native::kImgTex: return EACEWorldBakeResource::ImageTexture;
	case EACEWorldBakeResource_Native::kImgColor: return EACEWorldBakeResource::ImageColor;
	case EACEWorldBakeResource_Native::kSurface: return EACEWorldBakeResource::Surface;
	case EACEWorldBakeResource_Native::kMotionTable: return EACEWorldBakeResource::MotionTable;
	case EACEWorldBakeResource_Native::kSound: return EACEWorldBakeResource::Sound;
	case EACEWorldBakeResource_Native::kEnvironment: return EACEWorldBakeResource::Environment;
	case EACEWorldBakeResource_Native::kScene: return EACEWorldBakeResource::Scene;
	case EACEWorldBakeResource_Native::kRegion: return EACEWorldBakeResource::Region;
	case EACEWorldBakeResource_Native::kSoundTable: return EACEWorldBakeResource::SoundTable;
	case EACEWorldBakeResource_Native::kEnumMapper: return EACEWorldBakeResource::EnumMapper;
	case EACEWorldBakeResource_Native::kParticleEmitter: return EACEWorldBakeResource::ParticleEmitter;
	case EACEWorldBakeResource_Native::kPhysicsScript: return EACEWorldBakeResource::PhysicsScript;
	case EACEWorldBakeResource_Native::kPhysicsScriptTable: return EACEWorldBakeResource::PhysicsScriptTable;
	}

	return EACEWorldBakeResource::Invalid;
}

static FString ToString(EACEWorldBakeResource::Type Type)
{
	switch (Type)
	{
	case EACEWorldBakeResource::Model: return TEXT("Model");
	case EACEWorldBakeResource::Setup: return TEXT("Setup");
	case EACEWorldBakeResource::Animation: return TEXT("Animation");
	case EACEWorldBakeResource::Palette: return TEXT("Palette");
	case EACEWorldBakeResource::ImageTexture: return TEXT("ImageTexture");
	case EACEWorldBakeResource::ImageColor: return TEXT("ImageColor");
	case EACEWorldBakeResource::Surface: return TEXT("Surface");
	case EACEWorldBakeResource::MotionTable: return TEXT("MotionTable");
	case EACEWorldBakeResource::Sound: return TEXT("Sound");
	case EACEWorldBakeResource::Environment: return TEXT("Environment");
	case EACEWorldBakeResource::Scene: return TEXT("Scene");
	case EACEWorldBakeResource::Region: return TEXT("Region");
	case EACEWorldBakeResource::SoundTable: return TEXT("SoundTable");
	case EACEWorldBakeResource::EnumMapper: return TEXT("EnumMapper");
	case EACEWorldBakeResource::ParticleEmitter: return TEXT("ParticleEmitter");
	case EACEWorldBakeResource::PhysicsScript: return TEXT("PhysicsScript");
	case EACEWorldBakeResource::PhysicsScriptTable: return TEXT("PhysicsScriptTable");
	}

	return TEXT("");
}

static FString GetDatFilePath(const FString& DirectoryPath, EACEWorldBakeDatFile::Type DatFileType)
{
	FString FileName;
	switch (DatFileType)
	{
	case EACEWorldBakeDatFile::Cell:
		FileName = TEXT("client_cell_1.dat");
		break;

	case EACEWorldBakeDatFile::Highres:
		FileName = TEXT("client_highres.dat");
		break;

	case EACEWorldBakeDatFile::LocaleEnglish:
		FileName = TEXT("client_local_english.dat");
		break;

	case EACEWorldBakeDatFile::Portal:
		FileName = TEXT("client_portal.dat");
		break;
	}

	return FPaths::Combine(DirectoryPath, FileName);
}

FACEWorldBakeResourceLocation::FACEWorldBakeResourceLocation()
: OffsetX(0.0f)
, OffsetY(0.0f)
, OffsetZ(0.0f)
, HeadingW(0.0f)
, HeadingA(0.0f)
, HeadingB(0.0f)
, HeadingC(0.0f)
{
}

FACEWorldBakeResourceLocation::FACEWorldBakeResourceLocation(const FACEWorldBakeResourceLocation& Other)
: OffsetX(Other.OffsetX)
, OffsetY(Other.OffsetY)
, OffsetZ(Other.OffsetZ)
, HeadingW(Other.HeadingW)
, HeadingA(Other.HeadingA)
, HeadingB(Other.HeadingB)
, HeadingC(Other.HeadingC)
{
}

FACEWorldBakeResourceLocation& FACEWorldBakeResourceLocation::operator=(const FACEWorldBakeResourceLocation& Other)
{
	OffsetX = Other.OffsetX;
	OffsetY = Other.OffsetY;
	OffsetZ = Other.OffsetZ;
	HeadingW = Other.HeadingW;
	HeadingA = Other.HeadingA;
	HeadingB = Other.HeadingB;
	HeadingC = Other.HeadingC;
	return *this;
}

FACEWorldBakeScaledResourceLocation::FACEWorldBakeScaledResourceLocation()
: FACEWorldBakeResourceLocation()
, Scale(FVector3f::OneVector)
{
}

FACEWorldBakeScaledResourceLocation::FACEWorldBakeScaledResourceLocation(const FACEWorldBakeResourceLocation& InResourceLocation, const FVector3f& InScale)
: FACEWorldBakeResourceLocation(InResourceLocation)
, Scale(InScale)
{

}

FACEWorldBakeResourceInfo::FACEWorldBakeResourceInfo()
: ResourceId(0)
{
}

FACEWorldBakeResourceInfo::FACEWorldBakeResourceInfo(uint32 InResourceId, const FACEWorldBakeResourceLocation& InLocation)
: ResourceId(InResourceId)
, Location(InLocation)
{
}

FACEWorldBakeScaledResourceInfo::FACEWorldBakeScaledResourceInfo()
: FACEWorldBakeResourceInfo()
, Scale(FVector3f::OneVector)
{
}

FACEWorldBakeScaledResourceInfo::FACEWorldBakeScaledResourceInfo(const FACEWorldBakeResourceInfo& Resourceinfo, const FVector3f& InScale)
: FACEWorldBakeResourceInfo(Resourceinfo)
, Scale(InScale)
{
}

FACEWorldBakeScaledResourceInfo::FACEWorldBakeScaledResourceInfo(uint32 InResourceId, const FACEWorldBakeResourceLocation& InLocation, const FVector3f& InScale)
: FACEWorldBakeResourceInfo(InResourceId, InLocation)
, Scale(InScale)
{
}

FACEWorldBakeScaledResourceInfo::FACEWorldBakeScaledResourceInfo(uint32 InResourceId, const FACEWorldBakeScaledResourceLocation& InLocation)
: FACEWorldBakeResourceInfo(InResourceId, InLocation)
, Scale(InLocation.Scale)
{
}

class FACEWorldBakeDatToolsModule : public IACEWorldBakeDatToolsModule
{
public:
	virtual void StartupModule() override
	{
		if (GConfig)
		{
			GConfig->GetString(kACEWorldBakeIniDatToolsSection, kACEWorldBakeIniDatFileDirectory, DatFileDirectory, GEditorPerProjectIni);
		}
	}

	virtual bool IsGameModule() const override
	{
		return true;
	}

	virtual void GetDatFileDirectory(FString& OutPath, bool& OutHasCell, bool& OutHasHighRes, bool& OutHasLocale, bool& OutHasPortal) override
	{
		OutPath = DatFileDirectory;
		OutHasCell = FPaths::FileExists(GetDatFilePath(DatFileDirectory, EACEWorldBakeDatFile::Cell));
		OutHasHighRes = FPaths::FileExists(GetDatFilePath(DatFileDirectory, EACEWorldBakeDatFile::Highres));
		OutHasLocale = FPaths::FileExists(GetDatFilePath(DatFileDirectory, EACEWorldBakeDatFile::LocaleEnglish));
		OutHasPortal = FPaths::FileExists(GetDatFilePath(DatFileDirectory, EACEWorldBakeDatFile::Portal));
	}

	virtual void SetDatFileDirectory(const FString& Path) override
	{
		DatFileDirectory = Path;

		if (GConfig)
		{
			GConfig->SetString(kACEWorldBakeIniDatToolsSection, kACEWorldBakeIniDatFileDirectory, *DatFileDirectory, GEditorPerProjectIni);
		}
	}

	virtual void DumpDatFileStatsToLog(EACEWorldBakeDatFile::Type DatFileType, bool bDumpFileEntries) override
	{
		FACEWorldBakeDatFilePtr DatFile = GetDatFile(DatFileType);
		if (DatFile.IsValid())
		{
			DatFile->DumpStatsToLog(bDumpFileEntries);
		}
	}

	virtual FACEWorldBakeDatFilePtr GetDatFile(EACEWorldBakeDatFile::Type DatFileType) override
	{
		FACEWorldBakeDatFilePtr LoadedFile;
		{
			FACEWorldBakeDatFilePtr* AlreadyLoadedFile = OpenedDatFiles.Find(DatFileType);
			if (AlreadyLoadedFile && AlreadyLoadedFile->IsValid())
			{
				LoadedFile = *AlreadyLoadedFile;
			}
		}

		if (!LoadedFile.IsValid())
		{
			LoadedFile = FACEWorldBakeDatFile::LoadFromFile(GetDatFilePath(DatFileDirectory, DatFileType));
			if (LoadedFile.IsValid())
			{
				OpenedDatFiles.Add(DatFileType, LoadedFile);
			}
		}

		return LoadedFile;
	}

	virtual void ExportLandblocks() override
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportLandblocks()"));

		FACEWorldBakeDatFilePtr CellDatFile = GetDatFile(EACEWorldBakeDatFile::Cell);
		FACEWorldBakeDatFilePtr PortalDatFile = GetDatFile(EACEWorldBakeDatFile::Portal);
		if (CellDatFile.IsValid() && PortalDatFile.IsValid())
		{
			FACEWorldBakePortalRegion PortalRegion;
			if (PortalDatFile->GetResource(EACEWorldBakeResource_Native::kRegion, PortalRegion))
			{
				FACEWorldBakeCellFile CellFile(CellDatFile);
				FACEWorldBakeLandscapeWriter::SaveLandblockHeightmaps(CellFile, PortalRegion, TEXT("Tile"));

				{
					int32 WorkItems_SaveLandblockInfo = FACEWorldBakeCellFile::kHeightInLandblocks * FACEWorldBakeCellFile::kWidthInLandblocks;
					FScopedSlowTask SlowTask_SaveLandblockInfo(static_cast<float>(WorkItems_SaveLandblockInfo), LOCTEXT("SaveLandblockInfo", "Saving Landblock Infos"));
					SlowTask_SaveLandblockInfo.MakeDialog();

					for (uint32 LandblockY = 0; LandblockY < FACEWorldBakeCellFile::kHeightInLandblocks; LandblockY++)
					{
						for (uint32 LandblockX = 0; LandblockX < FACEWorldBakeCellFile::kWidthInLandblocks; LandblockX++)
						{
							SlowTask_SaveLandblockInfo.EnterProgressFrame();

							if (const FACEWorldBakeCellLandblock * const LandBlock = CellFile.GetLandblock(LandblockX, LandblockY))
							{
								if (LandBlock->HasLandblockObject)
								{
									SaveLandblockInfo(CellFile, LandblockX, LandblockY);
								}
							}
						}
					}
				}

				{
					float WorkPerEnumeration = static_cast<float>(FACEWorldBakeCellFile::kHeightInLandblocks) / static_cast<float>(kSceneLandblockWidth);
					float WorkItems_SaveLandblockScenes = WorkPerEnumeration * WorkPerEnumeration;
					FScopedSlowTask SlowTask_SaveLandblockScenes(static_cast<float>(WorkItems_SaveLandblockScenes), LOCTEXT("SaveLandblockScenes", "Saving Landblock Scenes"));
					SlowTask_SaveLandblockScenes.MakeDialog();

					for (uint32 LandblockY = 0; LandblockY < FACEWorldBakeCellFile::kHeightInLandblocks; LandblockY += kSceneLandblockWidth)
					{
						for (uint32 LandblockX = 0; LandblockX < FACEWorldBakeCellFile::kWidthInLandblocks; LandblockX += kSceneLandblockWidth)
						{
							SlowTask_SaveLandblockScenes.EnterProgressFrame();

							if (const FACEWorldBakeCellLandblock* const LandBlock = CellFile.GetLandblock(LandblockX, LandblockY))
							{
								SaveLandblockScenes(CellFile, LandblockX, LandblockY);
							}
						}
					}
				}
			}
		}
	}

	virtual void ExportLandblockInfo(uint32 LandblockX, uint32 LandblockY) override
	{
		FACEWorldBakeDatFilePtr CellDatFile = GetDatFile(EACEWorldBakeDatFile::Cell);
		if (CellDatFile.IsValid())
		{
			FACEWorldBakeCellFile CellFile(CellDatFile);
			SaveLandblockInfo(CellFile, LandblockX, LandblockY);
		}
	}

	virtual bool ExportResource(EACEWorldBakeResource::Type ResourceType, uint32 Identifier) override
	{
		FACEWorldBakeDatFilePtr PortalDatFile = GetDatFile(EACEWorldBakeDatFile::Portal);
		if (!PortalDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakeDatFilePtr HighresDatFile = GetDatFile(EACEWorldBakeDatFile::Highres);
		if (!HighresDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakePortalFile PortalFile(PortalDatFile);
		FACEWorldBakePortalFile HighresFile(HighresDatFile);

		switch (ResourceType)
		{
			case EACEWorldBakeResource::Model:
			{
				FACEWorldBakePortalModel Model;
				const bool modelOk = PortalFile.GetResource(Identifier, Model);
				if (modelOk)
				{
					return ExportModel(&Model, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			case EACEWorldBakeResource::Setup:
			{
				FACEWorldBakePortalSetup Setup;
				const bool SetupOk = PortalFile.GetResource(Identifier, Setup);
				if (SetupOk)
				{
					return ExportSetup(&Setup, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			//case EACEWorldBakeResource::Animation:

			case EACEWorldBakeResource::Palette:
			{
				FACEWorldBakePortalPalette PortalPalette;
				if (HighresFile.GetResource(Identifier, PortalPalette) || PortalFile.GetResource(Identifier, PortalPalette))
				{
					return SaveImageToFile(PortalPalette.Colors, PortalPalette.NumColors, 1, FString::Printf(TEXT("0x%08X"), Identifier), TEXT("/Palettes/"));
				}
			}
			break;

			case EACEWorldBakeResource::ImageTexture:
			{
				FACEWorldBakePortalImageTexture PortalTexture;
				const bool PortalTextureOk = PortalFile.GetResource(Identifier, PortalTexture);
				if (PortalTextureOk)
				{
					UE_LOG_ACEWORLDBAKEDATTOOLS(VeryVerbose, TEXT("ExportResources(ResourceType %s [0x%08X]) - Unhandled resource; could be used for image mips"), *ToString(ResourceType), Identifier);
				}

				return true;
			}

			case EACEWorldBakeResource::ImageColor:
			{
				FACEWorldBakePortalImage PortalImage;
				const bool PortalImageOk = HighresFile.GetResource(Identifier, PortalImage) || PortalFile.GetResource(Identifier, PortalImage);
				if (PortalImageOk && PortalImage.Pixels && 0 < PortalImage.Length)
				{
					return ExportImage(&PortalImage, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			case EACEWorldBakeResource::Surface:
			{
				FACEWorldBakePortalSurface Surface;
				const bool SurfaceOk = PortalFile.GetResource(Identifier, Surface);
				if (SurfaceOk)
				{
					// nothing to export; importing to Unreal directly uses FACEWorldBakePortalSurface
					UE_LOG_ACEWORLDBAKEDATTOOLS(VeryVerbose, TEXT("ExportResources(ResourceType %s [0x%08X]) - Unused resource."), *ToString(ResourceType), Identifier);
				}
				return  SurfaceOk;
			}

			//case EACEWorldBakeResource::MotionTable:

			case EACEWorldBakeResource::Sound:
			{
				FACEWorldBakePortalSoundWave Sound;
				const bool SoundOk = PortalFile.GetResource(Identifier, Sound);
				if (SoundOk)
				{
					const bool bExported = ExportSound(&Sound, FString::Printf(TEXT("0x%08X"), Identifier));
					if (!bExported)
					{
						UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("FACEWorldBakePortalSound() - ResourceId: 0x%08X, ChunkSize: %d, DataSize: %d, FormatTag: %d, Channels: %d, SamplesPerSec: %d, AvgBytesPerSec: %d, BlockAlign: %d, BitsPerSample: %d, BytesEx: %d"), Sound.ResourceId, Sound.ChunkSize, Sound.DataSize, Sound.FormatTag, Sound.Channels, Sound.SamplesPerSec, Sound.AvgBytesPerSec, Sound.BlockAlign, Sound.BitsPerSample, Sound.BytesEx);
					}
				}
			}
			break;

			case EACEWorldBakeResource::Environment:
			{
				FACEWorldBakePortalEnvironment Environment;
				const bool EnvironmentOk = PortalFile.GetResource(Identifier, Environment);
				if (EnvironmentOk)
				{
					return ExportEnvironment(&Environment, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			case EACEWorldBakeResource::Scene:
			{
				FACEWorldBakePortalScene Scene;
				const bool SceneOk = PortalFile.GetResource(Identifier, Scene);
				if (SceneOk)
				{
					return ExportScene(&Scene, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			case EACEWorldBakeResource::Region:
			{
				FACEWorldBakePortalRegion Region;
				const bool RegionOk = PortalFile.GetResource(Identifier, Region);
				if (RegionOk)
				{
					return ExportRegion(&Region, FString::Printf(TEXT("0x%08X"), Identifier));
				}
			}
			break;

			//case EACEWorldBakeResource::SoundTable:
			//case EACEWorldBakeResource::EnumMapper:
			//case EACEWorldBakeResource::ParticleEmitter:
			//case EACEWorldBakeResource::PhysicsScript:
			//case EACEWorldBakeResource::PhysicsScriptTable:
		}

		return false;
	}

	virtual int32 ExportResources(EACEWorldBakeResource::Type ResourceType, uint32 IdentifierMask) override
	{
		FACEWorldBakeDatFilePtr portalDatFile = GetDatFile(EACEWorldBakeDatFile::Portal);
		if (!portalDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakeDatFilePtr highresDatFile = GetDatFile(EACEWorldBakeDatFile::Highres);
		if (!highresDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakePortalFile portalFile(portalDatFile);
		FACEWorldBakePortalFile highresFile(highresDatFile);

		const EACEWorldBakeResource_Native::Type NativeResourceType = ToNative(ResourceType);

		TArray<uint32> ResourceIdentifiers;
		{
			const uint32 NativeMask = NativeResourceType | IdentifierMask;
			ResourceIdentifiers = portalDatFile->MatchIdentifiers(NativeMask);
			ResourceIdentifiers.Append(highresDatFile->MatchIdentifiers(NativeMask));
		}

		int32 WorkItems_ExportResourcesTypeText = ResourceIdentifiers.Num();
		FText ExportResourcesTypeText = FText::Format(LOCTEXT("ExportResourcesType", "Exporting {0} Resources"), FText::FromString(ToString(ResourceType)));
		FScopedSlowTask SlowTask_ExportResourcesType(static_cast<float>(WorkItems_ExportResourcesTypeText), ExportResourcesTypeText);
		SlowTask_ExportResourcesType.MakeDialog();

		int32 NumExported = 0;
		for (auto Identifier : ResourceIdentifiers)
		{
			SlowTask_ExportResourcesType.EnterProgressFrame();

			if (ExportResource(ResourceType, Identifier))
			{
				++NumExported;
			}
		}

		const int32 NumFailed = ResourceIdentifiers.Num() - NumExported;

		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportResources(ResourceType %s [0x%08X]) exported %d resources successfully (%d failures)!"), *ToString(ResourceType), ToNative(ResourceType) | IdentifierMask, NumExported, NumFailed);

		return NumExported;
	}

	virtual bool CorrectPortalImageTextureSettings(UTexture2D* Texture, uint32 Identifier = 0) override
	{
		FACEWorldBakeDatFilePtr PortalDatFile = GetDatFile(EACEWorldBakeDatFile::Portal);
		if (!PortalDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakeDatFilePtr HighresDatFile = GetDatFile(EACEWorldBakeDatFile::Highres);
		if (!HighresDatFile.IsValid())
		{
			return 0;
		}

		FACEWorldBakePortalFile PortalFile(PortalDatFile);
		FACEWorldBakePortalFile HighresFile(HighresDatFile);

		if (0 == Identifier)
		{
			FString AssetName = Texture->GetName();
			Identifier = FCString::Strtoi(*AssetName, nullptr, 16);
		}

		FACEWorldBakePortalImage PortalImage;
		bool PortalImageOk = HighresFile.GetResource(Identifier, PortalImage) || PortalFile.GetResource(Identifier, PortalImage);
		bool bIsLandscapeMask = !PortalImageOk && Texture->GetName().StartsWith(TEXT("Tile")) && (Texture->GetName().EndsWith(TEXT("Mask0")) || Texture->GetName().EndsWith(TEXT("Mask1")) || Texture->GetName().EndsWith(TEXT("Mask2")));
		bool bIsPalette = 0x04000000 == (Identifier & 0xFF000000);

		EACEWorldBakePixelFormat_Native::Type ImageFormat = EACEWorldBakePixelFormat_Native::kUnknown;

		if (bIsPalette)
		{
			ImageFormat = EACEWorldBakePixelFormat_Native::kIndex16;
		}
		else if (bIsLandscapeMask)
		{
			ImageFormat = EACEWorldBakePixelFormat_Native::kCustom_R8G8B8A8;
		}
		else if (PortalImageOk)
		{
			ImageFormat = PortalImage.Format;
		}

		if (EACEWorldBakePixelFormat_Native::kUnknown != ImageFormat)
		{
			FACEWorldBakeImageResourceSettings TextureSettings;

			switch (ImageFormat)
			{
			case EACEWorldBakePixelFormat_Native::kIndex16:
			case EACEWorldBakePixelFormat_Native::kIndex32:
				TextureSettings.CompressionSettings = TC_VectorDisplacementmap;
				TextureSettings.Filter = TF_Nearest;
				TextureSettings.MipGenSettings = TextureMipGenSettings::TMGS_NoMipmaps;
				TextureSettings.SRGB = 0;
				break;
			case EACEWorldBakePixelFormat_Native::kCustom_R8G8B8A8:
				TextureSettings.CompressionSettings = TC_Masks;
				TextureSettings.Filter = TF_Bilinear;
				TextureSettings.MipGenSettings = TextureMipGenSettings::TMGS_FromTextureGroup;
				TextureSettings.SRGB = 0;
				break;
			}

			if (TextureSettings != *Texture)
			{
#if WITH_EDITOR
				Texture->Modify();
				Texture->CompressionSettings = TextureSettings.CompressionSettings;
				Texture->Filter = TextureSettings.Filter;
				Texture->MipGenSettings = TextureSettings.MipGenSettings;
				Texture->SRGB = TextureSettings.SRGB;
				Texture->PostEditChange();
				return true;
#else
				return false;
#endif
			}
		}

		return false;
	}

	bool ExportLandblockInfos(const int32 CellX, const int32 CellY, const FACEWorldBakeCellLandblockObject& LanblockInfos, const FString& FileName)
	{
		const FString T3dContent = FACEWorldBakeT3dWriter::ExportText(CellX, CellY, LanblockInfos);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/LandblockInfos/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.t3d"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(T3dContent, *FilePath);
	}

	bool ExportLandblockScenes(const int32 StartX, const int32 StartY, const int32 CountX, const int32 CountY, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportLandblockScenes() - X:[%d-%d], Y:[%d-%d]"), StartX, StartX + CountX, StartY, StartY + CountY);

		const FString T3dContent = FACEWorldBakeT3dWriter::ExportLandblockScenesText(StartX, StartY, CountX, CountY);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/LandblockInfos/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.t3d"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(T3dContent, *FilePath);
	}

private:

	void SaveLandblockInfo(const FACEWorldBakeCellFile& cellFile, uint32 LandblockX, uint32 LandblockY)
	{
		FACEWorldBakeCellLandblockObject landblockInfos;
		if (cellFile.GetLandblockObject(LandblockX, LandblockY, landblockInfos))
		{
			const FString FileName = FString::Printf(TEXT("%03d_%03d_%08X"), LandblockX, LandblockY, landblockInfos.Identifier);
			ExportLandblockInfos(LandblockX, LandblockY, landblockInfos, FileName);
		}
	}

	void SaveLandblockScenes(const FACEWorldBakeCellFile& cellFile, uint32 LandblockX, uint32 LandblockY)
	{
		const FString FileName = FString::Printf(TEXT("%03d_%03d_%03d_%03d_Scenes"), LandblockX, LandblockY, LandblockX + kSceneLandblockWidth, LandblockY + kSceneLandblockWidth);
		ExportLandblockScenes(LandblockX, LandblockY, kSceneLandblockWidth, kSceneLandblockWidth, FileName);
	}

	TArray<FColor> ToColorArray(const uint8* ImageData, uint32 ImageDataLength, EACEWorldBakePixelFormat_Native::Type Format)
	{
		int32 BytesPerPixel = 4;
		switch (Format)
		{
		case EACEWorldBakePixelFormat_Native::kIndex16:
		case EACEWorldBakePixelFormat_Native::kA4R4G4B4:
		case EACEWorldBakePixelFormat_Native::kR5G6B5:
			BytesPerPixel = 2;
			break;

		case EACEWorldBakePixelFormat_Native::kCustomLscapeR8G8B8:
		case EACEWorldBakePixelFormat_Native::kR8G8B8:
			BytesPerPixel = 3;
			break;

		case EACEWorldBakePixelFormat_Native::kA8:
		case EACEWorldBakePixelFormat_Native::kP8:
		case EACEWorldBakePixelFormat_Native::kCustomLscapeAlpha:
			BytesPerPixel = 1;
			break;
		}

		TArray<FColor> ColorArray;
		ColorArray.AddZeroed(ImageDataLength / BytesPerPixel);

		uint32 BytesUsed = 0;
		for (int32 i = 0; (i < ColorArray.Num()) && (BytesUsed < ImageDataLength); i++)
		{
			const uint8* const pImageData = ImageData + BytesUsed;
			BytesUsed += BytesPerPixel;

			switch (Format)
			{
			case EACEWorldBakePixelFormat_Native::kCustomLscapeR8G8B8:
			case EACEWorldBakePixelFormat_Native::kR8G8B8:
				{
					ColorArray[i].R = pImageData[2];
					ColorArray[i].G = pImageData[1];
					ColorArray[i].B = pImageData[0];
					ColorArray[i].A = 255;
				}
				break;

			case EACEWorldBakePixelFormat_Native::kA8R8G8B8:
				{
					ColorArray[i].A = pImageData[3];
					ColorArray[i].R = pImageData[2];
					ColorArray[i].G = pImageData[1];
					ColorArray[i].B = pImageData[0];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kA8B8G8R8:
				{
					ColorArray[i].A = pImageData[0];
					ColorArray[i].B = pImageData[1];
					ColorArray[i].G = pImageData[2];
					ColorArray[i].R = pImageData[3];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kX8B8G8R8:
				{
					ColorArray[i].A = pImageData[0];
					ColorArray[i].B = pImageData[1];
					ColorArray[i].G = pImageData[2];
					ColorArray[i].R = pImageData[3];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kCustom_R8G8B8A8:
				{
					ColorArray[i].R = pImageData[0];
					ColorArray[i].G = pImageData[1];
					ColorArray[i].B = pImageData[2];
					ColorArray[i].A = pImageData[3];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kIndex16:
				{
					const uint16 Index16 = *reinterpret_cast<const uint16*>(pImageData);
					check(Index16 >= 0 && Index16 < 2048);
					const uint8 Quotient = Index16 / 256;
					const uint8 Remainder = Index16 % 256;
					ColorArray[i].B = Quotient;
					ColorArray[i].G = Remainder;
					ColorArray[i].R = 0;
					ColorArray[i].A = 255;
				}
				break;

			case EACEWorldBakePixelFormat_Native::kA4R4G4B4:
				{
					ColorArray[i].A = 16 * (pImageData[0] & 0xF0 >> 4);
					ColorArray[i].R = 16 * (pImageData[0] & 0x0F);
					ColorArray[i].G = 16 * (pImageData[1] & 0xF0 >> 4);
					ColorArray[i].B = 16 * (pImageData[1] & 0x0F);
				}
				break;

			case EACEWorldBakePixelFormat_Native::kA8:
				{
					ColorArray[i].B = 0;
					ColorArray[i].G = 0;
					ColorArray[i].R = 255;
					ColorArray[i].A = pImageData[0];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kP8:
				{
					ColorArray[i].B = 0;
					ColorArray[i].G = pImageData[0];
					ColorArray[i].R = 0;
					ColorArray[i].A = 255;
				}
				break;

			case EACEWorldBakePixelFormat_Native::kCustomLscapeAlpha:
				{
					ColorArray[i].B = 0;
					ColorArray[i].G = 0;
					ColorArray[i].R = 255;
					ColorArray[i].A = pImageData[0];
				}
				break;

			case EACEWorldBakePixelFormat_Native::kR5G6B5:
				{
					const uint16 ImageDataBytes = *reinterpret_cast<const uint16*>(&pImageData[0]);
					ColorArray[i].A = 255;
					*reinterpret_cast<uint16*>(&ColorArray[i].B) = ImageDataBytes;
				}
				break;
			}
		}

		return ColorArray;
	}

	bool ExportLinearColorImage(const FACEWorldBakePortalImage* const PortalImage, const FString& FileName)
	{
		TArray<FColor> PixelColors = ToColorArray(PortalImage->Pixels, PortalImage->Length, PortalImage->Format);
		return SaveImageToFile(PixelColors, PortalImage->Width, PortalImage->Height, FileName);
	}

	bool ExportDXTImage(const FACEWorldBakePortalImage* const PortalImage, const FString& FileName, bool AsDDS)
	{
		if (AsDDS)
		{
			const int32 dwMagic = 0x20534444;
			FACEWorldBakeDDSFileHeader ImageHeader;
			FMemory::Memzero(ImageHeader);
			ImageHeader.dwSize = sizeof(FACEWorldBakeDDSFileHeader);
			ImageHeader.dwHeight = PortalImage->Height;
			ImageHeader.dwWidth = PortalImage->Width;
			ImageHeader.dwFlags = DDSF_Caps | DDSF_Height | DDSF_Width | DDSF_PixelFormat;
			ImageHeader.ddpf.dwFlags = DDSPF_FourCC;
			ImageHeader.ddpf.dwSize = sizeof(FACEWorldBakeDDSPixelFormatHeader);

			switch (PortalImage->Format)
			{
			case EACEWorldBakePixelFormat_Native::kDXT1: ImageHeader.ddpf.dwFourCC = DDSPF_DXT1; break;
			case EACEWorldBakePixelFormat_Native::kDXT3: ImageHeader.ddpf.dwFourCC = DDSPF_DXT3; break;
			case EACEWorldBakePixelFormat_Native::kDXT5: ImageHeader.ddpf.dwFourCC = DDSPF_DXT5; break;
			}

			TArray<uint8> ImageBytes;
			ImageBytes.Reserve(PortalImage->Length + sizeof(FACEWorldBakeDDSFileHeader) + sizeof(int32));
			// dds magic
			ImageBytes.Append(reinterpret_cast<const uint8*>(&dwMagic), sizeof(int32));
			// dds header
			ImageBytes.Append(reinterpret_cast<const uint8*>(&ImageHeader), sizeof(FACEWorldBakeDDSFileHeader));
			// dds bytes
			for (uint32 ByteIndex = 0; ByteIndex < PortalImage->Length; ++ByteIndex)
			{
				ImageBytes.Add(PortalImage->Pixels[ByteIndex]);
			}

			const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Images/"));
			const FString FilePath = FString::Printf(TEXT("%s/%s.dds"), *FileDir, *FileName);

			return FFileHelper::SaveArrayToFile(ImageBytes, *FilePath);
		}
		else
		{
			TArray<uint8> DecompressedBytes;
			DecompressedBytes.AddZeroed(PortalImage->Width * PortalImage->Height * 4);

			uint8* ImagePtr = DecompressedBytes.GetData();
			int32 BlockIndex = 0;
			for (uint32 LY = 0; LY < PortalImage->Height; LY += 4)
			{
				for (uint32 LX = 0; LX < PortalImage->Width; LX += 4)
				{
					switch (PortalImage->Format)
					{
					case EACEWorldBakePixelFormat_Native::kDXT1:
						DecompressBlockBC1(LX, LY, PortalImage->Width * sizeof(uint32), PortalImage->Pixels + BlockIndex * 8, ImagePtr);
						break;

					case EACEWorldBakePixelFormat_Native::kDXT3:
						DecompressBlockBC2(LX, LY, PortalImage->Width * sizeof(uint32), PortalImage->Pixels + BlockIndex * 16, ImagePtr);
						break;

					case EACEWorldBakePixelFormat_Native::kDXT5:
						DecompressBlockBC3(LX, LY, PortalImage->Width * sizeof(uint32), PortalImage->Pixels + BlockIndex * 16, ImagePtr);
						break;
					}

					++BlockIndex;
				}
			}

			TArray<FColor> PixelColors = ToColorArray(DecompressedBytes.GetData(), DecompressedBytes.Num(), EACEWorldBakePixelFormat_Native::kCustom_R8G8B8A8);
			return SaveImageToFile(PixelColors, PortalImage->Width, PortalImage->Height, FileName);
		}
	}

	bool ExportJpegImage(const FACEWorldBakePortalImage* const PortalImage, const FString& FileName)
	{
		bool Exported = false;

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
		if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(PortalImage->Pixels, PortalImage->Length))
		{
			const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Images/"));
			const FString FilePath = FString::Printf(TEXT("%s/%s.jpg"), *FileDir, *FileName);

			Exported = FFileHelper::SaveArrayToFile(ImageWrapper->GetCompressed(100), *FilePath);
		}
		ImageWrapper.Reset();

		return Exported;
	}

	bool ExportIndexImage(const FACEWorldBakePortalImage* const PortalImage, const FString& FileName)
	{
		TArray<FColor> PixelColors = ToColorArray(PortalImage->Pixels, PortalImage->Length, PortalImage->Format);
		return SaveImageToFile(PixelColors, PortalImage->Width, PortalImage->Height, FileName);
	}

	bool ExportImage(const FACEWorldBakePortalImage* const PortalImage, const FString& FileName)
	{
		check(PortalImage && PortalImage->Length && PortalImage->Pixels);

		const FString FormatTypeString = EACEWorldBakePixelFormat_Native::TypeToString(PortalImage->Format);

		bool bExported = false;

		switch (PortalImage->Format)
		{
		case EACEWorldBakePixelFormat_Native::kR8G8B8:
		case EACEWorldBakePixelFormat_Native::kA8R8G8B8:
		case EACEWorldBakePixelFormat_Native::kA8B8G8R8:
		case EACEWorldBakePixelFormat_Native::kX8B8G8R8:
		case EACEWorldBakePixelFormat_Native::kIndex16:
		case EACEWorldBakePixelFormat_Native::kA4R4G4B4:
		case EACEWorldBakePixelFormat_Native::kA8:
		case EACEWorldBakePixelFormat_Native::kP8:
		case EACEWorldBakePixelFormat_Native::kCustomLscapeR8G8B8:
		case EACEWorldBakePixelFormat_Native::kCustomLscapeAlpha:
			bExported = ExportLinearColorImage(PortalImage, FileName);
			break;

		case EACEWorldBakePixelFormat_Native::kDXT1:
		case EACEWorldBakePixelFormat_Native::kDXT3:
		case EACEWorldBakePixelFormat_Native::kDXT5:
			bExported = ExportDXTImage(PortalImage, FileName, false);
			break;

		case EACEWorldBakePixelFormat_Native::kR5G6B5:
			bExported = ExportLinearColorImage(PortalImage, FileName);
			break;

		case EACEWorldBakePixelFormat_Native::kCustomRawJPEG:
			bExported = ExportJpegImage(PortalImage, FileName);
			break;

		default:
			UE_LOG_ACEWORLDBAKEDATTOOLS(Warning, TEXT("ExportImage() UNSUPPORTED - image.id(0x%08X), image.format(%s), image.type(%d), image.width(%d), image.height(%d), image.length(%d)"), PortalImage->ResourceId, *FormatTypeString, PortalImage->Type, PortalImage->Width, PortalImage->Height, PortalImage->Length);
			return false;
		}

		if (bExported)
		{
			UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportImage() - image.id(0x%08X), image.format(%s), image.type(%d), image.width(%d), image.height(%d), image.length(%d)"), PortalImage->ResourceId, *FormatTypeString, PortalImage->Type, PortalImage->Width, PortalImage->Height, PortalImage->Length);
		}
		else
		{
			UE_LOG_ACEWORLDBAKEDATTOOLS(Warning, TEXT("ExportImage() FAILED - image.id(0x%08X), image.format(%s), image.type(%d), image.width(%d), image.height(%d), image.length(%d)"), PortalImage->ResourceId, *FormatTypeString, PortalImage->Type, PortalImage->Width, PortalImage->Height, PortalImage->Length);
		}

		return bExported;
	}

	bool ExportSound(const FACEWorldBakePortalSoundWave* const Sound, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportSound() - Sound.ResourceId(0%x)"), Sound->ResourceId);

		FACEWorldBakeRiffWave RiffWave(*Sound);
		const uint8* pRiffWave = reinterpret_cast<const uint8*>(&RiffWave);

		FString ErrorMessage;
		// todo: update header with riff cue data 
		const bool bReadWaveInfo = false; // FWaveModInfo().ReadWaveInfo(pRiffWave, Sound->DataSize + sizeof(FACEWorldBakeRiffWave), &ErrorMessage, false, nullptr);
		if (bReadWaveInfo || (Sound->FormatTag == 0x55))
		{
			const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Sounds/"));
			const FString FileExtension = (Sound->FormatTag == 0x01) ? TEXT("wav") : TEXT("mp3");
			const FString FilePath = FString::Printf(TEXT("%s/%s.%s"), *FileDir, *FileName, *FileExtension);

			TArray<uint8> RawBytesArray(pRiffWave, sizeof(FACEWorldBakeRiffWave));
			RawBytesArray.Append(Sound->WaveData, Sound->DataSize);
			return FFileHelper::SaveArrayToFile(RawBytesArray, *FilePath);
		}
		else
		{
			UE_LOG_ACEWORLDBAKEDATTOOLS(Warning, TEXT("ExportSound() - Sound.ResourceId(0%x), read wave info failed, error: %s"), Sound->ResourceId, *ErrorMessage);
		}

		return false;
	}

	bool ExportEnvironment(const FACEWorldBakePortalEnvironment* const Environment, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportEnvironment() - Environment.ResourceId(0%x)"), Environment->ResourceId);

		bool bSavedRender = false;
		{
			const FString T3DContent = FACEWorldBakeFbxWriter::ExportText(*Environment);
			const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Environments/"));
			const FString FilePath = FString::Printf(TEXT("%s/%s.fbx"), *FileDir, *FileName);
			bSavedRender = FFileHelper::SaveStringToFile(T3DContent, *FilePath);
		}

		bool bSavedPortals = false;
		const bool bHasPortals = Environment->Parts.ContainsByPredicate([](const FACEWorldBakePortalEnvironmentPart& Part) { return Part.Portals.Num() > 0; });
		if (bHasPortals)
		{
			const FString T3DPortalsContent = FACEWorldBakeFbxWriter::ExportPortalsText(*Environment);
			const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Environments/"));
			const FString FilePath = FString::Printf(TEXT("%s/%sPortals.fbx"), *FileDir, *FileName);
			bSavedPortals = FFileHelper::SaveStringToFile(T3DPortalsContent, *FilePath);
		}

		return bSavedRender && (bSavedPortals || !bHasPortals);
	}

	bool ExportModel(const FACEWorldBakePortalModel* const Model, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportModel() - model.ResourceId(0%x), model.Flags(%d), model.SurfaceCount(%d), model.Vertices.Num()(%d), model.NumCollisionFans(%d), model.NumRenderFans(%d)"), Model->ResourceId, (uint32)Model->Flags, Model->SurfaceCount, Model->Vertices.Num(), Model->NumCollisionFans, Model->NumRenderFans);

		const FString FBXContent = FACEWorldBakeFbxWriter::ExportText(*Model);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Models/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.fbx"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(FBXContent, *FilePath);
	}

	bool ExportRegion(const FACEWorldBakePortalRegion* const Region, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportRegion() - Region.ResourceId(0%x)"), Region->ResourceId);

		const FString CSVContent = FACEWorldBakeCsvWriter::ExportText(*Region);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/CSV/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.csv"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(CSVContent, *FilePath);
	}

	bool ExportScene(const FACEWorldBakePortalScene* const Scene, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportScene() - Scene.ResourceId(0%x)"), Scene->ResourceId);

		const FString T3DContent = FACEWorldBakeT3dWriter::ExportText(*Scene);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Scenes/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.t3d"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(T3DContent, *FilePath);
	}

	bool ExportSetup(const FACEWorldBakePortalSetup* const Setup, const FString& FileName)
	{
		UE_LOG_ACEWORLDBAKEDATTOOLS(Log, TEXT("ExportSetup() - Setup.ResourceId(0%x)"), Setup->ResourceId);

		const FString T3DContent = FACEWorldBakeT3dWriter::ExportText(*Setup);

		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("/Setups/"));
		const FString FilePath = FString::Printf(TEXT("%s/%s.t3d"), *FileDir, *FileName);

		return FFileHelper::SaveStringToFile(T3DContent, *FilePath);
	}

private:
	FString DatFileDirectory;
	TMap<EACEWorldBakeDatFile::Type, FACEWorldBakeDatFilePtr> OpenedDatFiles;
};

UACEWorldBakeExportResourcesCommandlet::UACEWorldBakeExportResourcesCommandlet()
{
	IsClient = false;
	IsEditor = false;
	IsServer = false;
	LogToConsole = true;
}

int32 UACEWorldBakeExportResourcesCommandlet::Main(const FString& Params)
{
	const FString ParamsLower = Params.ToLower();
	ParseCommandLine(*ParamsLower, Tokens, Switches);

	IACEWorldBakeDatToolsModule* ACEWorldBakeDatToolsModule = FModuleManager::LoadModulePtr<IACEWorldBakeDatToolsModule>("ACEWorldBakeDatTools");
	if (nullptr != ACEWorldBakeDatToolsModule)
	{
		FString LandblockStr;
		FParse::Value(*Params, TEXT("LANDBLOCKINFO="), LandblockStr);
		if (LandblockStr.Len())
		{
			int32 LandblockX = -1;
			int32 LandblockY = -1;
			if (LandblockStr.Contains(TEXT("0x")))
			{
				UE_LOG_ACEWORLDBAKEDATTOOLS(Fatal, TEXT("UACEWorldBakeExportResourcesCommandlet::Main() LANDBLOCKINFO=%s - Hexadecimal landblock ids not implemented."), *LandblockStr);
			}
			else
			{
				FString XStr, YStr;
				LandblockStr.Split(TEXT("_"), &XStr, &YStr);

				LandblockX = FCString::Strtoi(*XStr, nullptr, 10);
				LandblockY = FCString::Strtoi(*YStr, nullptr, 10);
			}

			ACEWorldBakeDatToolsModule->ExportLandblockInfo(LandblockX, LandblockY);
		}

		if (Switches.Contains(TEXT("LANDBLOCKS")))
		{
			ACEWorldBakeDatToolsModule->ExportLandblocks();
		}

		FString ResourceStr;
		FParse::Value(*Params, TEXT("RESOURCE="), ResourceStr);
		if (ResourceStr.Len())
		{
			const int32 ResourceId = FCString::Strtoi(*ResourceStr, nullptr, 16);
			ACEWorldBakeDatToolsModule->ExportResource(ToACEWorldBake(static_cast<EACEWorldBakeResource_Native::Type>(ResourceId & 0xFFFF0000)), ResourceId);
		}

		for(int32 ResourceTypeIndex = 0; ResourceTypeIndex < UE_ARRAY_COUNT(kResourceTypes); ++ResourceTypeIndex)
		{
			const FString ResourceTypeSwitch = FString::Printf(TEXT("%ss"), *ToString(kResourceTypes[ResourceTypeIndex])).ToLower();
			if (Switches.Contains(TEXT("RESOURCES")) || Switches.Contains(ResourceTypeSwitch))
			{
				ACEWorldBakeDatToolsModule->ExportResources(kResourceTypes[ResourceTypeIndex], 0x0000FFFF);
			}
		}
	}

	return 0;
}

bool SaveImageToFile(const TArray<FColor>& ImageColors, int32 Width, int32 Height, const FString& FileName, const TCHAR* SavedSubDir)
{
	bool Exported = false;
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(ImageColors.GetData(), ImageColors.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8, Width * sizeof(FColor)))
	{
		const FString FileDir = FPaths::Combine(*FPaths::ProjectSavedDir(), SavedSubDir);
		const FString FilePath = FString::Printf(TEXT("%s/%s.png"), *FileDir, *FileName);

		const TArray64<uint8>& PNGData = ImageWrapper->GetCompressed(100);
		Exported = FFileHelper::SaveArrayToFile(PNGData, *FilePath);
	}
	ImageWrapper.Reset();

	return Exported;
}

void ArrayToString(const TArray<FString>& Array, FString& String)
{
	const int32 Count = Array.Num();
	for (int32 i = 0; i < Count; ++i)
	{
		if (i > 0)
		{
			String += TEXT(", ");
		}
		String += Array[i];
	}
}

void ArrayToString(const TArray<float>& Array, FString& String)
{
	const int32 Count = Array.Num();
	for (int32 i = 0; i < Count; ++i)
	{
		if (i > 0)
		{
			String += TEXT(",");
		}
		String += FString::Printf(TEXT("%4.6f"), Array[i]);
	}
}

FACEWorldBakeResourceLocation SumResourceLocation(const FACEWorldBakeResourceLocation& Left, const FACEWorldBakeResourceLocation& Right)
{
	const FVector3f LeftPosition(Left.OffsetX, Left.OffsetY, Left.OffsetZ);
	FQuat4f LeftRotation(Left.HeadingA, Left.HeadingB, Left.HeadingC, Left.HeadingW);
	LeftRotation.Normalize();

	const FMatrix44f LeftMatrix = LeftRotation * FMatrix44f::Identity;

	const FVector3f RightPosition(Right.OffsetX, Right.OffsetY, Right.OffsetZ);
	FQuat4f RightRotation(Right.HeadingA, Right.HeadingB, Right.HeadingC, Right.HeadingW);
	RightRotation.Normalize();

	const FMatrix44f RightMatrix = RightRotation * FMatrix44f::Identity;

	const FVector3f Location = RightRotation.RotateVector(LeftPosition) + RightPosition;
	const FMatrix44f Combined = LeftMatrix * RightMatrix;
	const FQuat4f CombinedQuat = Combined.ToQuat();

	FACEWorldBakeResourceLocation Result;
	Result.OffsetX = Location.X;
	Result.OffsetY = Location.Y;
	Result.OffsetZ = Location.Z;
	Result.HeadingA = CombinedQuat.X;
	Result.HeadingB = CombinedQuat.Y;
	Result.HeadingC = CombinedQuat.Z;
	Result.HeadingW = CombinedQuat.W;
	return Result;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_GAME_MODULE(FACEWorldBakeDatToolsModule, ACEWorldBakeDatTools);
