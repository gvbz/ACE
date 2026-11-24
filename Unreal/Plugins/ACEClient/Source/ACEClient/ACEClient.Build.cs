using UnrealBuildTool;
using System.IO;

public class ACEClient : ModuleRules
{
	public ACEClient(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
            "Core",
            "ApplicationCore",
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
			"InputCore",
			"HeadMountedDisplay",
			"XRBase",
			"UMG",
			"Slate",
			"SlateCore",
			"ProceduralMeshComponent",
			"MeshDescription",
			"StaticMeshDescription",
			"Json",
			"JsonUtilities",
			"Projects",
			"ImageWrapper",
			"Landscape"
		});
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI", "HTTP", "XmlParser", "AssetRegistry", "ACERetailRender", "PhysicsCore", "Chaos" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "ICU", "Expat");
		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "SDL3");
		}
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Crypt32.lib");
			PublicSystemLibraries.Add("Ole32.lib");
			PublicSystemLibraries.Add("Shell32.lib");
		}
		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("Launch");
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "ACEClient_Android.xml"));
		}
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
		RuntimeDependencies.Add("$(PluginDir)/Docs/UI/Resolved/*.json", StagedFileType.NonUFS);
        // These are the shared policy, recipe and routing APIs. Optional plugin manifests and assets are downloaded.
        foreach (string file in new[] { "api.json", "legacy-plugins.xml", "LICENSE-Lua.txt", "LICENSE-VTClassic.txt",
            "loot_rules.lua", "ucm/main.lua", "ucm/default.json", "ucm/monster-elements.json",
            "itemtools/main.lua", "itemtools/default.json", "autocraft/catalog.json", "autocraft/NOTICE.txt",
            "autocraft/LICENSE", "waypoint/MapRegions.xml", "waypoint/LICENSE_GOARROW.txt" })
            RuntimeDependencies.Add("$(PluginDir)/ClientMods/" + file, StagedFileType.UFS);
        RuntimeDependencies.Add("$(PluginDir)/Docs/Plugins/...", StagedFileType.NonUFS);
        RuntimeDependencies.Add("$(PluginDir)/Data/Personal/...", StagedFileType.NonUFS); // personal: far-windows
	}
}
