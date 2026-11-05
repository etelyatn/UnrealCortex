#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexAdoptTestHelpers.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPDependentScan.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

namespace
{
	FString GapFilename(const UPackage* Package, const FString& Extension)
	{
		return FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
	}

	FEdGraphPinType GapOwnerPin(UClass* Class)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategoryObject = Class;
		return PinType;
	}

	UK2Node_VariableGet* GapAddExternalGet(UBlueprint* Blueprint, FName Name, UClass* Parent)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		Node->VariableReference.SetExternalMember(Name, Parent);
		Creator.Finalize();
		return Node;
	}

	/** Deletes a fixture file; a registry scan or a failed load can hold the handle for a moment. */
	void GapDelete(const FString& Filename)
	{
		for (int32 Attempt = 0; Attempt < 100; ++Attempt)
		{
			if (IFileManager::Get().Delete(*Filename, false, true, true) || !IFileManager::Get().FileExists(*Filename))
			{
				return;
			}
			FlushAsyncLoading();
			FPlatformProcess::Sleep(0.1f);
		}
	}

	void GapUnload(const TArray<FName>& PackageNames)
	{
		TArray<UPackage*> Loaded;
		for (const FName PackageName : PackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName.ToString()))
			{
				Loaded.Add(Package);
			}
		}
		FText Error;
		UPackageTools::UnloadPackages(Loaded, Error, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDependentScanUnscannedTest,
	"Cortex.Blueprint.DependentScan.Unscanned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPDependentScanUnscannedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Owner = MakeBlueprint(TEXT("BP_GapOwner"), AActor::StaticClass(), TEXT("CortexGapOwner"));
	UBlueprint* Corrupt = MakeBlueprint(TEXT("BP_GapCorrupt"), AActor::StaticClass(), TEXT("CortexGapCorrupt"));
	if (!TestNotNull(TEXT("Owner"), Owner) || !TestNotNull(TEXT("Corrupt"), Corrupt))
	{
		return false;
	}
	FEdGraphPinType BoolPin;
	BoolPin.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Owner, TEXT("Tracked"), BoolPin);
	FKismetEditorUtilities::CompileBlueprint(Owner);
	FBlueprintEditorUtils::AddMemberVariable(Corrupt, TEXT("Holder"), GapOwnerPin(Owner->GeneratedClass));

	const FString Extension = FPackageName::GetAssetPackageExtension();
	const FString OwnerFile = GapFilename(Owner->GetOutermost(), Extension);
	const FString CorruptFile = GapFilename(Corrupt->GetOutermost(), Extension);
	const FString CorruptPath = Corrupt->GetPathName();
	const TArray<FName> Packages = {Owner->GetOutermost()->GetFName(), Corrupt->GetOutermost()->GetFName()};

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	TestTrue(TEXT("Owner saved"), UPackage::SavePackage(Owner->GetOutermost(), Owner, *OwnerFile, SaveArgs));
	TestTrue(TEXT("Dependent saved"), UPackage::SavePackage(Corrupt->GetOutermost(), Corrupt, *CorruptFile, SaveArgs));
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous({OwnerFile, CorruptFile}, true);
	GapUnload({Packages[1]});

	// The dependent exists on disk and in the registry but cannot be loaded (forced by the test hook).
	FCortexBPDependentScan::SetForcedLoadFailureForTest(TEXT("CortexGapCorrupt"));
	TArray<FCortexBPDependentHit> Hits;
	FCortexBPDependentScan::Find(Owner, TEXT("Tracked"), FCortexBPDependentScan::EMemberKind::Variable, Hits);
	FCortexBPDependentScan::SetForcedLoadFailureForTest(FString());
	const FCortexBPDependentHit* Hit = Hits.FindByPredicate([](const FCortexBPDependentHit& H) { return H.AssetPath.Contains(TEXT("BP_GapCorrupt")); });
	if (TestNotNull(TEXT("A dependent that cannot be loaded is reported, not dropped"), Hit))
	{
		TestTrue(TEXT("Flagged unscanned"), Hit->bUnscanned);
		TestFalse(TEXT("Carries the reason"), Hit->Error.IsEmpty());
		TestEqual(TEXT("Keeps its role"), Hit->Role, FString(TEXT("referencer")));
		TestTrue(TEXT("Json says unscanned"), Hit->ToJson()->GetBoolField(TEXT("unscanned")));
	}

	// Without the forced failure the same dependent is scanned normally.
	TArray<FCortexBPDependentHit> Healthy;
	FCortexBPDependentScan::Find(Owner, TEXT("Tracked"), FCortexBPDependentScan::EMemberKind::Variable, Healthy);
	const FCortexBPDependentHit* HealthyHit = Healthy.FindByPredicate([](const FCortexBPDependentHit& H) { return H.AssetPath.Contains(TEXT("BP_GapCorrupt")); });
	TestTrue(TEXT("Loadable dependent is not flagged"), HealthyHit == nullptr || !HealthyHit->bUnscanned);

	GapUnload(Packages);
	GapDelete(OwnerFile);
	GapDelete(CorruptFile);
	TestFalse(TEXT("Fixture file removed"), IFileManager::Get().FileExists(*CorruptFile));
	Registry.ScanModifiedAssetFiles({OwnerFile, CorruptFile});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDependentScanLevelBlueprintTest,
	"Cortex.Blueprint.DependentScan.LevelBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPDependentScanLevelBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Owner = MakeBlueprint(TEXT("BP_LevelOwner"), AActor::StaticClass(), TEXT("CortexLevelOwner"));
	if (!TestNotNull(TEXT("Owner"), Owner))
	{
		return false;
	}
	FEdGraphPinType BoolPin;
	BoolPin.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Owner, TEXT("Tracked"), BoolPin);
	FKismetEditorUtilities::CompileBlueprint(Owner);

	UPackage* MapPackage = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexLevelMap_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("CortexLevelMap"), MapPackage);
	if (!TestNotNull(TEXT("World"), World))
	{
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone); // SavePackage refuses a top-level asset without a standalone flag
	ULevelScriptBlueprint* LevelBlueprint = World->PersistentLevel->GetLevelScriptBlueprint(false);
	if (!TestNotNull(TEXT("Level Blueprint"), LevelBlueprint))
	{
		return false;
	}
	const FGuid NodeGuid = GapAddExternalGet(LevelBlueprint, TEXT("Tracked"), Owner->GeneratedClass)->NodeGuid;

	const FString OwnerFile = GapFilename(Owner->GetOutermost(), FPackageName::GetAssetPackageExtension());
	const FString MapFile = GapFilename(MapPackage, FPackageName::GetMapPackageExtension());
	const FString MapName = MapPackage->GetName();
	const TArray<FName> Packages = {Owner->GetOutermost()->GetFName(), MapPackage->GetFName()};

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const bool bOwnerSaved = UPackage::SavePackage(Owner->GetOutermost(), Owner, *OwnerFile, SaveArgs);
	const bool bMapSaved = UPackage::SavePackage(MapPackage, World, *MapFile, SaveArgs);
	TestTrue(TEXT("Owner saved"), bOwnerSaved);
	TestTrue(TEXT("Map saved"), bMapSaved);
	if (bOwnerSaved && bMapSaved)
	{
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.ScanFilesSynchronous({OwnerFile, MapFile}, true);
		World->DestroyWorld(false);
		GapUnload({Packages[1]});

		TArray<FCortexBPDependentHit> Hits;
		FCortexBPDependentScan::Find(Owner, TEXT("Tracked"), FCortexBPDependentScan::EMemberKind::Variable, Hits);
		const FString ExpectedPath = TEXT("__level_bp__:") + MapName;
		const FCortexBPDependentHit* Hit = Hits.FindByPredicate([&ExpectedPath](const FCortexBPDependentHit& H) { return H.AssetPath == ExpectedPath; });
		if (TestNotNull(TEXT("The map's Level Blueprint is scanned"), Hit))
		{
			TestEqual(TEXT("Level script role"), Hit->Role, FString(TEXT("level_script")));
			TestFalse(TEXT("Scanned"), Hit->bUnscanned);
			if (TestEqual(TEXT("One reference"), Hit->References.Num(), 1))
			{
				TestEqual(TEXT("Reference node GUID"), Hit->References[0].NodeGuid, NodeGuid);
			}
		}
		Registry.ScanModifiedAssetFiles({OwnerFile, MapFile});
	}

	GapUnload(Packages);
	GapDelete(OwnerFile);
	GapDelete(MapFile);
	return true;
}
