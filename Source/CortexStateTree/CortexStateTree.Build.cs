using UnrealBuildTool;

public class CortexStateTree : ModuleRules
{
	public CortexStateTree(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CortexCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AssetTools",
			"UnrealEd",
			"GameplayTags",
			"GameplayStateTreeModule",
			"StateTreeModule",
			"StateTreeEditorModule",
			"AIModule",
			"BlueprintGraph",
			"Kismet",
		});

		// UE 5.4: FStateTreeCompilerLog has no ToTokenizedMessages(); messages are
		// extracted through a MessageLog listing instead (see CortexSTCompat.h).
		// StructUtils owns FInstancedStruct before its 5.5 move into CoreUObject.
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
		{
			PrivateDependencyModuleNames.Add("MessageLog");
			PrivateDependencyModuleNames.Add("StructUtils");
		}

		// UE 5.6 introduces the PropertyBindingUtils module (FPropertyBindingBindingCollection, which the editor bindings and
		// the tests call into); 5.7 moves the property binding support types there.
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 6)
		{
			PrivateDependencyModuleNames.Add("PropertyBindingUtils");
		}
	}
}
