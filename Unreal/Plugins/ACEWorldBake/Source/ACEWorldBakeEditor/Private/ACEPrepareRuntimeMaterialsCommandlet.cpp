#include "ACEPrepareRuntimeMaterialsCommandlet.h"
#include "ACEDatSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

UACEPrepareRuntimeMaterialsCommandlet::UACEPrepareRuntimeMaterialsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UACEPrepareRuntimeMaterialsCommandlet::Main(const FString& Params)
{
	return PrepareMaterials() ? 0 : 1;
}

bool UACEPrepareRuntimeMaterialsCommandlet::PrepareMaterials()
{
	// No DAT files or account details are required to compile shader parents.
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
	TStrongObjectPtr<UACEDatSubsystem> Dat(NewObject<UACEDatSubsystem>(Instance.Get()));
	const auto Parents = Dat->GetRuntimeMaterialParents();
	TArray<TStrongObjectPtr<UMaterialInterface>> Retained;
	for (UMaterialInterface* Parent : Parents)
	{
		if (!Parent)
		{
			UE_LOG(LogTemp, Error, TEXT("ACE: a runtime material factory failed during cook preparation"));
			return false;
		}
		Retained.Emplace(Parent);
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	TArray<FString> SavedFiles;
	auto Save = [&SaveArgs, &SavedFiles](UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FAssetRegistryModule::AssetCreated(Asset);
		if (!UPackage::SavePackage(Package, Asset, *File, SaveArgs)) return false;
		SavedFiles.Add(File);
		return true;
	};
	// Material expressions reference this persistent collection. Save it before
	// shader parents so a clean checkout/cook has the same shared fog dependency.
	if (!Dat->GetRuntimeFogCollection() || !Save(Dat->GetRuntimeFogCollection())) return false;
	if (!Dat->GetRuntimePortalCollection() || !Save(Dat->GetRuntimePortalCollection())) return false;
	if (!Dat->GetRuntimeFarSceneryCollection() || !Save(Dat->GetRuntimeFarSceneryCollection())) return false;
	// The portal list is replaced with a live float texture at runtime. Its shader
	// default must still be a real, cookable linear texture rather than a transient.
	UPackage* TexturePackage = CreatePackage(TEXT("/Game/ACE/RuntimeMaterials/T_ACEPortalViewsDefault"));
	TexturePackage->FullyLoad();
	UTexture2D* PortalDefault = NewObject<UTexture2D>(TexturePackage, TEXT("T_ACEPortalViewsDefault"), RF_Public | RF_Standalone);
	TArray64<uint8> Zeros;
	Zeros.SetNumZeroed(64 * sizeof(FVector4f));
	PortalDefault->Source.Init(64, 1, 1, 1, TSF_RGBA32F, Zeros.GetData());
	PortalDefault->SRGB = false;
	PortalDefault->CompressionSettings = TC_HDR_F32;
	PortalDefault->MipGenSettings = TMGS_NoMipmaps;
	PortalDefault->NeverStream = true;
	PortalDefault->Filter = TF_Nearest;
	PortalDefault->UpdateResource();
	if (!Save(PortalDefault)) return false;

	for (UMaterialInterface* Parent : Parents)
	{
		const FString Name = Parent->GetName();
		UPackage* Package = CreatePackage(*(TEXT("/Game/ACE/RuntimeMaterials/") + Name));
		Package->FullyLoad();
		// Retire the previous graph before replacing it, including its subobjects.
		if (UMaterial* Previous = FindObject<UMaterial>(Package, *Name))
		{
			const FName RetiredName = MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("ACEPreviousCookMaterial"));
			Previous->ClearFlags(RF_Public | RF_Standalone);
			Previous->SetFlags(RF_Transient);
			if (!Previous->Rename(*RetiredName.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional)) return false;
		}
		UMaterial* Material = DuplicateObject<UMaterial>(Parent->GetMaterial(), Package, *Name);
		Material->ClearFlags(RF_Transient);
		Material->SetFlags(RF_Public | RF_Standalone);
		// UE propagates the transient parent's flags to optional editor data.
		// Clearing only the parent saves an asset without its graph and crashes
		// UMaterial::PostLoad on the next cook/editor launch.
		ForEachObjectWithOuter(Material, [](UObject* Object)
		{
			Object->ClearFlags(RF_Transient);
		}, true);
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Expression);
				Texture && Texture->ParameterName == TEXT("PortalViews"))
			{
				Texture->Texture = PortalDefault;
			}
		}
		Material->PostEditChange();
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			for (const FString& Error : Resource->GetCompileErrors())
				UE_LOG(LogTemp, Error, TEXT("ACE: %s: %s"), *Name, *Error);
			if (!Resource->GetCompileErrors().IsEmpty()) return false;
		}
		if (!Save(Material)) return false;
	}
	// The registry may already have cached missing files during engine startup.
	// AssetCreated alone has no on-disk package metadata for the cooker's lookup.
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(SavedFiles, true);
	UE_LOG(LogTemp, Display, TEXT("ACE: prepared %d runtime shader parents for cooking"), Parents.Num());
	return true;
}
