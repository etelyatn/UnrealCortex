#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"

namespace
{
	UPackage* CreateWritableCleanupTestPackage(const TCHAR* Name)
	{
		return CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"),
			Name,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupSaveContractTest,
	"Cortex.Blueprint.Cleanup.Migration.SaveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupSaveContractTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupSaveContract")), TEXT("BP_CleanupSaveContract"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) return false;
	FEdGraphPinType Type;
	Type.PinCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("OldVariable"), Type);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("OldUncompiledVariable"), Type);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	FCortexCommandResult Initial = FCortexBPCleanupOps::CleanupMigration(Params);
	if (!TestTrue(TEXT("Default compile and save succeed"), Initial.bSuccess)) return false;
	TestTrue(TEXT("Default saves"), Initial.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Saved package clean"), Initial.Data->GetBoolField(TEXT("is_dirty")));
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	TArray<uint8> Before;
	TestTrue(TEXT("Initial package exists on disk"), FFileHelper::LoadFileToArray(Before, *Filename));
	Params->SetArrayField(TEXT("remove_variables"), {MakeShared<FJsonValueString>(TEXT("OldVariable"))});
	Params->SetBoolField(TEXT("save"), false);
	FCortexCommandResult Staged = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Staging succeeds"), Staged.bSuccess);
	if (Staged.bSuccess)
	{
		TestFalse(TEXT("Staging does not save"), Staged.Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Staging keeps package dirty"), Staged.Data->GetBoolField(TEXT("is_dirty")));
		TestTrue(TEXT("Staging compiles independently"), Staged.Data->GetBoolField(TEXT("compiled")));
	}
	TArray<uint8> After;
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("Staging leaves disk bytes unchanged"), Before == After);
	TestFalse(TEXT("Variable removed in memory"), Blueprint->NewVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("OldVariable"); }));
	TestTrue(TEXT("Independent uncompiled variable exists before staging"), Blueprint->NewVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("OldUncompiledVariable"); }));
	Params->SetArrayField(TEXT("remove_variables"), {MakeShared<FJsonValueString>(TEXT("OldUncompiledVariable"))});
	Params->SetBoolField(TEXT("compile"), false);
	FCortexCommandResult Uncompiled = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Uncompiled unsaved mutation succeeds"), Uncompiled.bSuccess);
	if (Uncompiled.bSuccess)
	{
		TestFalse(TEXT("Uncompiled staging reports no compilation"), Uncompiled.Data->GetBoolField(TEXT("compiled")));
		TestEqual(TEXT("Uncompiled staging reports skipped status"), Uncompiled.Data->GetStringField(TEXT("compile_status")), FString(TEXT("skipped")));
		TestFalse(TEXT("Uncompiled staging does not save"), Uncompiled.Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Uncompiled staging keeps dirty state"), Uncompiled.Data->GetBoolField(TEXT("is_dirty")));
	}
	TestFalse(TEXT("Independent variable removed without compilation"), Blueprint->NewVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("OldUncompiledVariable"); }));
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("Uncompiled unsaved mutation preserves disk bytes"), Before == After);
	Params->RemoveField(TEXT("remove_variables"));
	Params->SetStringField(TEXT("save"), TEXT("false"));
	FCortexCommandResult Invalid = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Nonboolean save refused"), Invalid.bSuccess);
	TestEqual(TEXT("Invalid flag error"), Invalid.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Invalid request preserves dirty state"), Blueprint->GetOutermost()->IsDirty());
	Params->SetBoolField(TEXT("save"), false);
	Params->SetNumberField(TEXT("compile"), 1);
	Invalid = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Numeric compile refused"), Invalid.bSuccess);
	TestEqual(TEXT("Invalid compile flag error"), Invalid.ErrorCode, CortexErrorCodes::InvalidField);
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("Invalid flags leave disk unchanged"), Before == After);
	Params->SetBoolField(TEXT("save"), true);
	Params->SetBoolField(TEXT("compile"), false);
	Blueprint->Status = BS_Error;
	FCortexCommandResult Failed = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Saving without a compile is refused"), Failed.bSuccess);
	TestEqual(TEXT("Save-without-compile code"), Failed.ErrorCode, CortexErrorCodes::InvalidOperation);
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("Compiler failure leaves disk unchanged"), Before == After);
	// A reachable missing call exercises a real requested compile failure, independently of the
	// pre-existing BS_Error guard above. Allocate a real impure actor call first, then invalidate
	// only its member reference: this models a stale call while preserving its execution pins.
	UEdGraph* EventGraph = Blueprint->UbergraphPages.IsEmpty() ? nullptr : Blueprint->UbergraphPages[0];
	if (!TestNotNull(TEXT("Error fixture has an event graph"), EventGraph))
	{
		IFileManager::Get().Delete(*Filename);
		Blueprint->MarkAsGarbage();
		return false;
	}
	UK2Node_CustomEvent* Trigger = NewObject<UK2Node_CustomEvent>(EventGraph);
	Trigger->CustomFunctionName = TEXT("RunCleanupSaveContractBrokenCall");
	Trigger->CreateNewGuid();
	EventGraph->AddNode(Trigger, false, false);
	Trigger->AllocateDefaultPins();
	UK2Node_CallFunction* BrokenCall = NewObject<UK2Node_CallFunction>(EventGraph);
	BrokenCall->FunctionReference.SetExternalMember(TEXT("K2_DestroyActor"), AActor::StaticClass());
	BrokenCall->CreateNewGuid();
	EventGraph->AddNode(BrokenCall, false, false);
	BrokenCall->AllocateDefaultPins();
	BrokenCall->FunctionReference.SetSelfMember(TEXT("MissingCleanupSaveContractFunction"));
	UEdGraphPin* Then = Trigger->FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* Execute = BrokenCall->FindPin(UEdGraphSchema_K2::PN_Execute);
	if (!TestNotNull(TEXT("Error trigger exposes execution"), Then)
		|| !TestNotNull(TEXT("Missing call exposes execution"), Execute))
	{
		IFileManager::Get().Delete(*Filename);
		Blueprint->MarkAsGarbage();
		return false;
	}
	Then->MakeLinkTo(Execute);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Params->SetBoolField(TEXT("compile"), true);
	// The guarded compile runs in silent mode, so the compiler message is returned in the diagnostics payload, not logged.
	FCortexCommandResult RequestedFailure = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Actual requested compile fails"), RequestedFailure.bSuccess);
	TestEqual(TEXT("Actual compile failure code"), RequestedFailure.ErrorCode, CortexErrorCodes::CompileFailed);
	TestTrue(TEXT("Actual compiler leaves BS_Error"), Blueprint->Status == BS_Error);
	if (TestTrue(TEXT("Compile failure carries outcome details"), RequestedFailure.ErrorDetails.IsValid()))
	{
		TestTrue(TEXT("Failed requested compile was attempted"), RequestedFailure.ErrorDetails->GetBoolField(TEXT("compiled")));
		// The fault pre-dates the call, so it survives the restore and the recovery compile cannot prove the class:
		// the guarded run reports an unverified rollback and blocks the asset instead of claiming a restoration.
		TestEqual(TEXT("Unverifiable rollback is reported"),
			RequestedFailure.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("unverified")));
		TestTrue(TEXT("Failed recovery blocks the asset"), RequestedFailure.ErrorDetails->GetBoolField(TEXT("blocked")));
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (TestTrue(TEXT("Failed compile returns diagnostics"),
			RequestedFailure.ErrorDetails->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics != nullptr))
		{
			TestTrue(TEXT("Diagnostics name the missing function"),
				Diagnostics->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
				{
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					return Value->TryGetObject(Entry) && Entry != nullptr
						&& (*Entry)->GetStringField(TEXT("message")).Contains(TEXT("MissingCleanupSaveContractFunction"));
				}));
		}
		TestEqual(TEXT("Failed compile status reported"), RequestedFailure.ErrorDetails->GetStringField(TEXT("compile_status")), FString(TEXT("failed")));
		TestFalse(TEXT("Failed requested compile never saved"), RequestedFailure.ErrorDetails->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Failed requested compile remains dirty"), RequestedFailure.ErrorDetails->GetBoolField(TEXT("is_dirty")));
	}
	TestTrue(TEXT("Failure preserves broken graph for inspection"), EventGraph->Nodes.Contains(BrokenCall)
		&& EventGraph->Nodes.Contains(Trigger) && Then->LinkedTo.Contains(Execute));
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("Actual requested compile failure preserves disk bytes"), Before == After);
	// The asset is blocked now (the pre-existing fault made the recovery unverifiable), so the repaired save runs on a
	// fresh Blueprint rather than on a blocked one.
	IFileManager::Get().Delete(*Filename);
	Blueprint->MarkAsGarbage();

	UBlueprint* Repaired = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupSaveRepaired")), TEXT("BP_CleanupSaveRepaired"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Repaired Blueprint created"), Repaired))
	{
		return false;
	}
	FEdGraphPinType RepairedType;
	RepairedType.PinCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(Repaired, TEXT("RepairedVariable"), RepairedType);
	FKismetEditorUtilities::CompileBlueprint(Repaired);
	const FString RepairedFilename = FPackageName::LongPackageNameToFilename(
		Repaired->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	TSharedPtr<FJsonObject> RepairedParams = MakeShared<FJsonObject>();
	RepairedParams->SetStringField(TEXT("asset_path"), Repaired->GetPathName());
	RepairedParams->SetArrayField(TEXT("remove_variables"), {MakeShared<FJsonValueString>(TEXT("RepairedVariable"))});
	RepairedParams->SetBoolField(TEXT("compile"), true);
	RepairedParams->SetBoolField(TEXT("save"), true);
	FCortexCommandResult Final = FCortexBPCleanupOps::CleanupMigration(RepairedParams);
	TestTrue(TEXT("Successful final compile and save"), Final.bSuccess);
	if (Final.bSuccess)
	{
		TestTrue(TEXT("Final save reported"), Final.Data->GetBoolField(TEXT("saved")));
		TestFalse(TEXT("Repaired saved package is clean"), Final.Data->GetBoolField(TEXT("is_dirty")));
		TestEqual(TEXT("Repaired compile status valid"), Final.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
		TestTrue(TEXT("Repaired package exists on disk"), FFileHelper::LoadFileToArray(After, *RepairedFilename));
		TestTrue(TEXT("Successful final save persists the staged variable removal"), !Repaired->NewVariables.ContainsByPredicate(
			[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("RepairedVariable"); }));
	}
	IFileManager::Get().Delete(*RepairedFilename);
	Repaired->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRemoveVariableTest,
	"Cortex.Blueprint.Cleanup.RemoveVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRemoveVariableTest::RunTest(const FString& Parameters)
{
	// Create Blueprint with a variable
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupVarTest")),
		FName(TEXT("BP_CleanupVarTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	// Add variable
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("Health"), PinType);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Verify variable exists
	TestTrue(TEXT("Variable exists before cleanup"),
		TestBP->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V) {
			return V.VarName == TEXT("Health");
		}));

	// Call cleanup to remove the variable
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	TArray<TSharedPtr<FJsonValue>> VarsToRemove;
	VarsToRemove.Add(MakeShared<FJsonValueString>(TEXT("Health")));
	Params->SetArrayField(TEXT("remove_variables"), VarsToRemove);
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Cleanup succeeded"), Result.bSuccess);

	// Verify variable removed
	TestFalse(TEXT("Variable removed after cleanup"),
		TestBP->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V) {
			return V.VarName == TEXT("Health");
		}));

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRemoveFunctionTest,
	"Cortex.Blueprint.Cleanup.RemoveFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRemoveFunctionTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupFuncTest")),
		FName(TEXT("BP_CleanupFuncTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("CleanupTargetFunc")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(TestBP, FuncGraph, true, static_cast<UClass*>(nullptr));
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	TestTrue(TEXT("Function exists before cleanup"),
		TestBP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) {
			return Graph && Graph->GetName() == TEXT("CleanupTargetFunc");
		}));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	TArray<TSharedPtr<FJsonValue>> FuncsToRemove;
	FuncsToRemove.Add(MakeShared<FJsonValueString>(TEXT("CleanupTargetFunc")));
	Params->SetArrayField(TEXT("remove_functions"), FuncsToRemove);
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Cleanup succeeded"), Result.bSuccess);

	TestFalse(TEXT("Function removed after cleanup"),
		TestBP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) {
			return Graph && Graph->GetName() == TEXT("CleanupTargetFunc");
		}));

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRejectsInvalidReparentTest,
	"Cortex.Blueprint.Cleanup.RejectInvalidReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRejectsInvalidReparentTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupInvalidReparentTest")),
		FName(TEXT("BP_CleanupInvalidReparentTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	const UClass* OriginalParent = TestBP->ParentClass;
	TestNotNull(TEXT("Original parent exists"), OriginalParent);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("new_parent_class"), TEXT("/Script/CoreUObject.Object"));
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Cleanup should reject invalid reparent"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Parent class should remain unchanged"), TestBP->ParentClass == OriginalParent);

	UBlueprint* ObjectBP = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_CleanupObjectModel")),
		FName(TEXT("BP_CleanupObjectModel")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Ordinary object Blueprint is created"), ObjectBP))
	{
		TestBP->MarkAsGarbage();
		return false;
	}
	Params->SetStringField(TEXT("asset_path"), ObjectBP->GetPathName());
	Params->SetStringField(TEXT("new_parent_class"), USaveGame::StaticClass()->GetPathName());
	Params->SetBoolField(TEXT("compile"), true);
	Params->SetBoolField(TEXT("save"), false);
	const FCortexCommandResult ObjectResult = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Ordinary object model can acquire a native object parent"), ObjectResult.bSuccess);
	TestTrue(TEXT("Object model has its requested native parent"), ObjectBP->ParentClass == USaveGame::StaticClass());
	Params->SetStringField(TEXT("new_parent_class"), AActor::StaticClass()->GetPathName());
	const FCortexCommandResult ObjectToActor = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Object model cannot become an Actor asset"), ObjectToActor.bSuccess);
	TestTrue(TEXT("Rejected family change preserves object model parent"),
		ObjectBP->ParentClass == USaveGame::StaticClass());
	ObjectBP->MarkAsGarbage();

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupWidgetReparentTypeFamilyTest,
	"Cortex.Blueprint.Cleanup.WidgetReparentTypeFamily",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupWidgetReparentTypeFamilyTest::RunTest(const FString& Parameters)
{
	UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (!UserWidgetClass || !WidgetBlueprintClass)
	{
		AddInfo(TEXT("UMG not available; skipping widget reparent test"));
		return true;
	}

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UserWidgetClass,
		CreateWritableCleanupTestPackage(TEXT("WBP_CleanupWidgetReparentTest")),
		FName(TEXT("WBP_CleanupWidgetReparentTest")),
		BPTYPE_Normal,
		WidgetBlueprintClass,
		UBlueprintGeneratedClass::StaticClass());
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("new_parent_class"), TEXT("/Script/UMG.UserWidget"));
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Widget->Widget reparent is allowed"), Result.bSuccess);

	Params->SetStringField(TEXT("new_parent_class"), TEXT("/Script/Engine.Actor"));
	const FCortexCommandResult BadResult = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Widget->Actor reparent is rejected"), BadResult.bSuccess);
	TestEqual(TEXT("Invalid field code"), BadResult.ErrorCode, CortexErrorCodes::InvalidField);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupWidgetPreservationTest,
	"Cortex.Blueprint.Cleanup.WidgetPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupWidgetPreservationTest::RunTest(const FString& Parameters)
{
	UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (!UserWidgetClass || !WidgetBlueprintClass)
	{
		AddInfo(TEXT("UMG not available; skipping widget preservation test"));
		return true;
	}

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UserWidgetClass,
		CreateWritableCleanupTestPackage(TEXT("WBP_CleanupPreservationTest")),
		FName(TEXT("WBP_CleanupPreservationTest")),
		BPTYPE_Normal,
		WidgetBlueprintClass,
		UBlueprintGeneratedClass::StaticClass());
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	FObjectProperty* WidgetTreeProp = CastField<FObjectProperty>(
		TestBP->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	UObject* WidgetTreeBefore = nullptr;
	if (WidgetTreeProp)
	{
		WidgetTreeBefore = WidgetTreeProp->GetObjectPropertyValue(
			WidgetTreeProp->ContainerPtrToValuePtr<void>(TestBP));
	}
	TestNotNull(TEXT("WidgetTree should exist before cleanup"), WidgetTreeBefore);

	FArrayProperty* AnimsProp = CastField<FArrayProperty>(
		TestBP->GetClass()->FindPropertyByName(TEXT("Animations")));
	TestNotNull(TEXT("Animations property should exist"), AnimsProp);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("new_parent_class"), TEXT("/Script/UMG.UserWidget"));
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Cleanup should succeed"), Result.bSuccess);

	UObject* WidgetTreeAfter = nullptr;
	if (WidgetTreeProp)
	{
		WidgetTreeAfter = WidgetTreeProp->GetObjectPropertyValue(
			WidgetTreeProp->ContainerPtrToValuePtr<void>(TestBP));
	}
	TestNotNull(TEXT("WidgetTree must survive cleanup"), WidgetTreeAfter);
	TestTrue(TEXT("Widget BP has no SCS"), TestBP->SimpleConstructionScript == nullptr);

	TestBP->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// RemoveSCSComponent tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSComponentLeafTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSComponent.Leaf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSComponentLeafTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_RemoveSCSLeafTest")),
		FName(TEXT("BP_RemoveSCSLeafTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	USCS_Node* LeafNode = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("LeafComp")));
	SCS->AddNode(LeafNode);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	TestTrue(TEXT("LeafComp exists before removal"),
		SCS->GetAllNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("LeafComp"));
		}));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("LeafComp"));
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestTrue(TEXT("RemoveSCSComponent succeeded"), Result.bSuccess);

	TestFalse(TEXT("LeafComp removed from SCS"),
		SCS->GetAllNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("LeafComp"));
		}));

	FString CompileStatus;
	if (Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus))
	{
		TestEqual(TEXT("compile_status is UpToDate"), CompileStatus, FString(TEXT("UpToDate")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSComponentChildrenPromotedTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSComponent.ChildrenPromoted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSComponentChildrenPromotedTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_RemoveSCSChildPromoteTest")),
		FName(TEXT("BP_RemoveSCSChildPromoteTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	// Build hierarchy: ParentComp → MiddleComp → ChildComp
	USCS_Node* ParentNode = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("ParentComp")));
	USCS_Node* MiddleNode = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("MiddleComp")));
	USCS_Node* ChildNode  = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("ChildComp")));
	SCS->AddNode(ParentNode);
	ParentNode->AddChildNode(MiddleNode);
	MiddleNode->AddChildNode(ChildNode);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Remove MiddleComp — ChildComp should be promoted to ParentComp
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("MiddleComp"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestTrue(TEXT("RemoveSCSComponent succeeded"), Result.bSuccess);

	TestFalse(TEXT("MiddleComp removed"),
		SCS->GetAllNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("MiddleComp"));
		}));

	TestTrue(TEXT("ChildComp still exists"),
		SCS->GetAllNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("ChildComp"));
		}));

	TestTrue(TEXT("ChildComp promoted to ParentComp"),
		ParentNode->GetChildNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("ChildComp"));
		}));

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSComponentNotFoundTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSComponent.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSComponentNotFoundTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_RemoveSCSNotFoundTest")),
		FName(TEXT("BP_RemoveSCSNotFoundTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("DoesNotExist"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestFalse(TEXT("Returns failure for missing component"), Result.bSuccess);
	TestEqual(TEXT("Error code is ComponentNotFound"), Result.ErrorCode, CortexErrorCodes::ComponentNotFound);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSComponentNoSCSTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSComponent.NoSCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSComponentNoSCSTest::RunTest(const FString& Parameters)
{
	// UActorComponent parent produces a Blueprint with no SimpleConstructionScript
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		CreateWritableCleanupTestPackage(TEXT("BP_RemoveSCSNoSCSTest")),
		FName(TEXT("BP_RemoveSCSNoSCSTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TestNull(TEXT("Blueprint has no SCS"), TestBP->SimpleConstructionScript);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("AnyComp"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestFalse(TEXT("Returns failure when no SCS"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	TestBP->MarkAsGarbage();
	return true;
}
