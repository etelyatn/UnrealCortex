#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "CortexBPTestLiftActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* RemoveGuardCreateLiftBP(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* RemoveGuardAddSCSNode(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VarName)
	{
		if (!BP || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(ComponentClass, FName(VarName));
		BP->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(BP);
		return BP->SimpleConstructionScript->FindSCSNode(FName(VarName));
	}

	UCortexBPTestSubobjComponent* RemoveGuardGetTemplateComponent(UBlueprint* BP, USCS_Node* Node)
	{
		if (!BP || !Node)
		{
			return nullptr;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		if (!BPGC)
		{
			return nullptr;
		}

		return Cast<UCortexBPTestSubobjComponent>(Node->GetActualComponentTemplate(BPGC));
	}

	TSharedPtr<FJsonObject> RemoveGuardMakeRemoveParams(UBlueprint* BP, const FString& ComponentName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetStringField(TEXT("component_name"), ComponentName);
		Params->SetBoolField(TEXT("compile"), false);
		return Params;
	}

	TArray<FString> RemoveGuardGetStringArrayField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		TArray<FString> Out;
		if (!Obj.IsValid())
		{
			return Out;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return Out;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				Out.Add(Value->AsString());
			}
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardCleanTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.Clean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardCleanTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardClean"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	TestNotNull(TEXT("SCS node created"), Node);
	if (!Node)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp")));
	TestTrue(TEXT("Remove succeeds for clean node"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DiffObj = nullptr;
		TestTrue(TEXT("diff exists"), Result.Data->TryGetObjectField(TEXT("diff"), DiffObj));
		FString GuardResult;
		TestTrue(TEXT("guard_result exists"), Result.Data->TryGetStringField(TEXT("guard_result"), GuardResult));
		TestEqual(TEXT("guard_result is clean"), GuardResult, FString(TEXT("clean")));
		if (DiffObj && DiffObj->IsValid())
		{
			const TArray<FString> DirtyKeys = RemoveGuardGetStringArrayField(*DiffObj, TEXT("dirty_keys"));
			TestEqual(TEXT("clean diff has no dirty keys"), DirtyKeys.Num(), 0);
			FString DiffSummary;
			TestTrue(TEXT("diff_summary exists"), (*DiffObj)->TryGetStringField(TEXT("diff_summary"), DiffSummary));
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardTopLevelDirtyTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.TopLevelDirty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardTopLevelDirtyTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardTopDirty"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	UCortexBPTestSubobjComponent* Template = RemoveGuardGetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	if (!Node || !Template)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->ComponentTags.Add(TEXT("DirtyTag"));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp")));
	TestTrue(TEXT("Remove succeeds for top-level dirty"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString GuardResult;
		TestTrue(TEXT("guard_result exists"), Result.Data->TryGetStringField(TEXT("guard_result"), GuardResult));
		TestEqual(TEXT("guard_result is top_level_dirty"), GuardResult, FString(TEXT("top_level_dirty")));

		const TSharedPtr<FJsonObject>* DiffObj = nullptr;
		TestTrue(TEXT("diff exists"), Result.Data->TryGetObjectField(TEXT("diff"), DiffObj));
		if (DiffObj && DiffObj->IsValid())
		{
			const TArray<FString> DirtyKeys = RemoveGuardGetStringArrayField(*DiffObj, TEXT("dirty_keys"));
			TestTrue(TEXT("diff includes ComponentTags"), DirtyKeys.Contains(TEXT("ComponentTags")));
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardSubObjectRefusalTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.SubObjectRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardSubObjectRefusalTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardSubRefuse"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	UCortexBPTestSubobjComponent* Template = RemoveGuardGetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	TestNotNull(TEXT("Payload exists"), Template ? Template->Payload.Get() : nullptr);
	if (!Node || !Template || !Template->Payload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->Payload->Tracks.Add(11);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp")));
	TestFalse(TEXT("Remove refuses sub-object dirty without ack"), Result.bSuccess);
	TestEqual(TEXT("Error code is PotentialDataLoss"), Result.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	if (Result.ErrorDetails.IsValid())
	{
		const TArray<FString> RequiredAck = RemoveGuardGetStringArrayField(Result.ErrorDetails, TEXT("required_acknowledgment"));
		TestTrue(TEXT("required_acknowledgment includes Payload"), RequiredAck.Contains(TEXT("Payload")));
		TestTrue(TEXT("dirty_details present"), Result.ErrorDetails->HasField(TEXT("dirty_details")));
		TestTrue(TEXT("explanation present"), Result.ErrorDetails->HasField(TEXT("explanation")));
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardAcknowledgedRetryTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.AcknowledgedRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardAcknowledgedRetryTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardAckRetry"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	UCortexBPTestSubobjComponent* Template = RemoveGuardGetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	TestNotNull(TEXT("Payload exists"), Template ? Template->Payload.Get() : nullptr);
	if (!Node || !Template || !Template->Payload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->Payload->Tracks.Add(22);

	const FCortexCommandResult First = FCortexBPCleanupOps::RemoveSCSComponent(RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp")));
	TestFalse(TEXT("First call refuses"), First.bSuccess);
	TestEqual(TEXT("First call error code"), First.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	const TArray<FString> RequiredAck = RemoveGuardGetStringArrayField(First.ErrorDetails, TEXT("required_acknowledgment"));
	TestTrue(TEXT("required_acknowledgment not empty"), RequiredAck.Num() > 0);

	TSharedPtr<FJsonObject> RetryParams = RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp"));
	TArray<TSharedPtr<FJsonValue>> AckArray;
	for (const FString& Key : RequiredAck)
	{
		AckArray.Add(MakeShared<FJsonValueString>(Key));
	}
	RetryParams->SetArrayField(TEXT("acknowledged_losses"), AckArray);

	const FCortexCommandResult Retry = FCortexBPCleanupOps::RemoveSCSComponent(RetryParams);
	TestTrue(TEXT("Retry succeeds with matching ack"), Retry.bSuccess);
	if (Retry.Data.IsValid())
	{
		FString GuardResult;
		FString OverrideUsed;
		TestTrue(TEXT("guard_result exists"), Retry.Data->TryGetStringField(TEXT("guard_result"), GuardResult));
		TestTrue(TEXT("override_used exists"), Retry.Data->TryGetStringField(TEXT("override_used"), OverrideUsed));
		TestEqual(TEXT("guard_result is acknowledged"), GuardResult, FString(TEXT("acknowledged")));
		TestEqual(TEXT("override_used is acknowledged_losses"), OverrideUsed, FString(TEXT("acknowledged_losses")));
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardStaleAckTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.StaleAck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardStaleAckTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardStaleAck"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	UCortexBPTestSubobjComponent* Template = RemoveGuardGetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	TestNotNull(TEXT("Payload exists"), Template ? Template->Payload.Get() : nullptr);
	if (!Node || !Template || !Template->Payload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->Payload->Tracks.Add(31);

	TSharedPtr<FJsonObject> Params = RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp"));
	Params->SetArrayField(TEXT("acknowledged_losses"), {MakeShared<FJsonValueString>(TEXT("WrongKey"))});

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestFalse(TEXT("Stale ack should be refused"), Result.bSuccess);
	TestEqual(TEXT("Error code is PotentialDataLoss"), Result.ErrorCode, CortexErrorCodes::PotentialDataLoss);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardForceOverrideTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.ForceOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardForceOverrideTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardForce"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	UCortexBPTestSubobjComponent* Template = RemoveGuardGetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	TestNotNull(TEXT("Payload exists"), Template ? Template->Payload.Get() : nullptr);
	if (!Node || !Template || !Template->Payload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->Payload->Tracks.Add(41);

	TSharedPtr<FJsonObject> Params = RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp"));
	Params->SetBoolField(TEXT("force"), true);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(Params);
	TestTrue(TEXT("Force override succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString GuardResult;
		FString OverrideUsed;
		TestTrue(TEXT("guard_result exists"), Result.Data->TryGetStringField(TEXT("guard_result"), GuardResult));
		TestTrue(TEXT("override_used exists"), Result.Data->TryGetStringField(TEXT("override_used"), OverrideUsed));
		TestEqual(TEXT("guard_result is force_override"), GuardResult, FString(TEXT("force_override")));
		TestEqual(TEXT("override_used is force"), OverrideUsed, FString(TEXT("force")));
	}
	TestTrue(TEXT("Warning emitted for force override"), Result.Warnings.Num() > 0);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardDefaultSceneRootTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.DefaultSceneRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardDefaultSceneRootTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardDefaultRoot"), AActor::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	TestNotNull(TEXT("SCS exists"), BP ? BP->SimpleConstructionScript.Get() : nullptr);
	if (!BP || !BP->SimpleConstructionScript)
	{
		return false;
	}

	USCS_Node* RootNode = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
	TestNotNull(TEXT("Default scene root exists"), RootNode);
	if (!RootNode)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(BP, RootNode->GetVariableName().ToString()));
	TestFalse(TEXT("DefaultSceneRoot removal refused"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardInheritedNodeTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.InheritedNodeRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardInheritedNodeTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardParent"), AActor::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	USCS_Node* ParentNode = RemoveGuardAddSCSNode(ParentBP, USceneComponent::StaticClass(), TEXT("InheritedComp"));
	TestNotNull(TEXT("Parent node created"), ParentNode);
	if (!ParentNode)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent class generated"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardChild"), ParentClass);
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(ChildBP, TEXT("InheritedComp")));
	TestFalse(TEXT("Inherited node removal refused"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Error message references parent class"), Result.ErrorMessage.Contains(ParentClass->GetName()));

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveSCSGuardTOCTOUTest,
	"Cortex.Blueprint.Cleanup.RemoveSCSGuard.TOCTOURecheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveSCSGuardTOCTOUTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	UBlueprint* BP = RemoveGuardCreateLiftBP(TEXT("BP_RemoveGuardTOCTOU"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RemoveGuardAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("GuardComp"));
	TestNotNull(TEXT("Node created"), Node);
	if (!Node)
	{
		BP->MarkAsGarbage();
		return false;
	}

	FCortexBPCleanupOps::SetRemoveSCSComponentMidflightTestHook(
		[](USCS_Node* HookNode, UBlueprint* HookBP)
		{
			if (!HookNode || !HookBP || !HookBP->GeneratedClass)
			{
				return;
			}
			UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(HookBP->GeneratedClass);
			if (!BPGC)
			{
				return;
			}
			UCortexBPTestSubobjComponent* Template =
				Cast<UCortexBPTestSubobjComponent>(HookNode->GetActualComponentTemplate(BPGC));
			if (Template && Template->Payload)
			{
				Template->Payload->Tracks.Add(999);
			}
		});

	const FCortexCommandResult Result = FCortexBPCleanupOps::RemoveSCSComponent(
		RemoveGuardMakeRemoveParams(BP, TEXT("GuardComp")));
	FCortexBPCleanupOps::SetRemoveSCSComponentMidflightTestHook(nullptr);

	TestFalse(TEXT("TOCTOU recheck refused changed dirty state"), Result.bSuccess);
	TestEqual(TEXT("Error code is PotentialDataLoss"), Result.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	TestNotNull(TEXT("Node still exists after aborted transaction"),
		BP->SimpleConstructionScript->FindSCSNode(FName(TEXT("GuardComp"))));

	BP->MarkAsGarbage();
	return true;
#else
	AddInfo(TEXT("WITH_DEV_AUTOMATION_TESTS disabled; skipping TOCTOU hook test."));
	return true;
#endif
}
