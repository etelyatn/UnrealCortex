#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UPackage* CreateWritableRecompileTestPackage(const TCHAR* Name)
	{
		return CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"),
			Name,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
	}

	bool IsPackageUnderRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName == Root || PackageName.StartsWith(Root + TEXT("/"));
	}

	struct FScopedReadOnlyMountedRoot
	{
		FString Root;
		FString PhysicalDir;

		FScopedReadOnlyMountedRoot()
		{
			Root = FString::Printf(
				TEXT("/CortexReadOnly%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			PhysicalDir = FPaths::ProjectSavedDir() / TEXT("CortexReadOnlyBlueprintTests") / Root.RightChop(1);
			IFileManager::Get().MakeDirectory(*PhysicalDir, true);
			FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
		}

		~FScopedReadOnlyMountedRoot()
		{
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package && IsPackageUnderRoot(Package->GetName(), Root))
				{
					Package->MarkAsGarbage();
				}
			}
			CollectGarbage(RF_NoFlags);
			FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsTest,
	"Cortex.Blueprint.RecompileDependents.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileParent")),
		FName(TEXT("BP_RecompileParent")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileChild")),
		FName(TEXT("BP_RecompileChild")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);
	TestTrue(TEXT("recompile_dependents succeeded"), Result.bSuccess);
	TestTrue(TEXT("Child blueprint up to date"), ChildBP->Status == BS_UpToDate || ChildBP->Status == BS_UpToDateWithWarnings);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsRejectsNonWritableTargetTest,
	"Cortex.Blueprint.RecompileDependents.RejectsNonWritableTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsRejectsNonWritableTargetTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/NotWritable/BP_Target"));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);
	TestFalse(TEXT("recompile_dependents rejects non-writable target"), Result.bSuccess);
	TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsRejectsNonWritableDependentTest,
	"Cortex.Blueprint.RecompileDependents.RejectsNonWritableDependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsRejectsNonWritableDependentTest::RunTest(const FString& Parameters)
{
	FScopedReadOnlyMountedRoot ReadOnlyRoot;

	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileParentWritable")),
		FName(TEXT("BP_RecompileParentWritable")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreatePackage(*(ReadOnlyRoot.Root / TEXT("BP_RecompileReadOnlyChild"))),
		FName(TEXT("BP_RecompileReadOnlyChild")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Read-only child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);
	ChildBP->Status = BS_Dirty;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);

	TestFalse(TEXT("recompile_dependents rejects non-writable dependent"), Result.bSuccess);
	TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("Child was not compiled"), ChildBP->Status, EBlueprintStatus::BS_Dirty);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}
