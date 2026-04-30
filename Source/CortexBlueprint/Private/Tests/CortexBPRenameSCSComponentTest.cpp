#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "CortexBPTestLiftActor.h"
#include "CortexTypes.h"
#include "Components/TimelineComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* RenameCreateLiftBP(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* RenameAddSCSNode(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VarName)
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

	TSharedPtr<FJsonObject> RenameMakeParams(
		UBlueprint* BP,
		const FString& OldName,
		const FString& NewName,
		bool bCompile = true)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetStringField(TEXT("old_name"), OldName);
		Params->SetStringField(TEXT("new_name"), NewName);
		Params->SetBoolField(TEXT("compile"), bCompile);
		return Params;
	}

	bool RenameHasSCSNode(UBlueprint* BP, const FString& Name)
	{
		return BP
			&& BP->SimpleConstructionScript
			&& BP->SimpleConstructionScript->FindSCSNode(FName(*Name)) != nullptr;
	}

	bool RenameAddBlueprintBoolVariable(UBlueprint* BP, const FString& Name)
	{
		if (!BP)
		{
			return false;
		}

		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return FBlueprintEditorUtils::AddMemberVariable(BP, FName(*Name), PinType);
	}

	UEdGraph* RenameGetOrCreateEventGraph(UBlueprint* BP)
	{
		if (!BP)
		{
			return nullptr;
		}

		if (BP->UbergraphPages.Num() > 0 && BP->UbergraphPages[0])
		{
			return BP->UbergraphPages[0];
		}

		UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP,
			FName(TEXT("EventGraph")),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!EventGraph)
		{
			return nullptr;
		}

		FBlueprintEditorUtils::AddUbergraphPage(BP, EventGraph);
		return EventGraph;
	}

	bool RenameAddVariableGetNode(UBlueprint* BP, const FString& VariableName)
	{
		UEdGraph* EventGraph = RenameGetOrCreateEventGraph(BP);
		if (!EventGraph)
		{
			return false;
		}

		UK2Node_VariableGet* VariableGet = NewObject<UK2Node_VariableGet>(EventGraph);
		if (!VariableGet)
		{
			return false;
		}

		VariableGet->CreateNewGuid();
		VariableGet->VariableReference.SetSelfMember(FName(*VariableName));
		EventGraph->AddNode(VariableGet, false, false);
		VariableGet->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		return true;
	}

	TArray<FString> RenameCollectVariableGetNames(UBlueprint* BP)
	{
		TArray<FString> Names;
		if (!BP)
		{
			return Names;
		}

		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_VariableGet* VariableGet = Cast<UK2Node_VariableGet>(Node))
				{
					Names.Add(VariableGet->GetVarName().ToString());
				}
			}
		}

		return Names;
	}

	UTimelineComponent* RenameGetTimelineTemplate(UBlueprint* BP, const FString& Name)
	{
		if (!BP || !BP->GeneratedClass || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*Name));
		if (!Node)
		{
			return nullptr;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		return BPGC ? Cast<UTimelineComponent>(Node->GetActualComponentTemplate(BPGC)) : nullptr;
	}

	UCortexBPTestSubobjComponent* RenameGetSubobjTemplate(UBlueprint* BP, const FString& Name)
	{
		if (!BP || !BP->GeneratedClass || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*Name));
		if (!Node)
		{
			return nullptr;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		return BPGC ? Cast<UCortexBPTestSubobjComponent>(Node->GetActualComponentTemplate(BPGC)) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentHappyPathTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentHappyPathTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_Happy"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp"));
	TestNotNull(TEXT("OldComp node added"), Node);
	if (!Node)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("NewComp"), false));
	TestTrue(TEXT("rename_scs_component succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString AssetPath;
		FString OldName;
		FString NewName;
		bool bCompiled = true;
		FString CompileStatus;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		Result.Data->TryGetStringField(TEXT("old_name"), OldName);
		Result.Data->TryGetStringField(TEXT("new_name"), NewName);
		Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
		Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus);

		TestEqual(TEXT("asset_path echoed"), AssetPath, BP->GetPathName());
		TestEqual(TEXT("old_name echoed"), OldName, FString(TEXT("OldComp")));
		TestEqual(TEXT("new_name echoed"), NewName, FString(TEXT("NewComp")));
		TestFalse(TEXT("compiled false when compile=false"), bCompiled);
		TestFalse(TEXT("compile_status is populated"), CompileStatus.IsEmpty());
	}

	TestFalse(TEXT("OldComp no longer exists"), RenameHasSCSNode(BP, TEXT("OldComp")));
	TestTrue(TEXT("NewComp exists"), RenameHasSCSNode(BP, TEXT("NewComp")));

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentCollisionSCSTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.Collision.SCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentCollisionSCSTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_CollisionSCS"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("Old node"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));
	TestNotNull(TEXT("Existing node"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ExistingComp")));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("ExistingComp"), false));
	TestFalse(TEXT("Rename refused on existing SCS node collision"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentCollisionVariableTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.Collision.BlueprintVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentCollisionVariableTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_CollisionVar"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("Old node"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));
	TestTrue(TEXT("Blueprint variable added"), RenameAddBlueprintBoolVariable(BP, TEXT("TakenName")));
	FKismetEditorUtilities::CompileBlueprint(BP);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("TakenName"), false));
	TestFalse(TEXT("Rename refused on Blueprint variable collision"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentCollisionInheritedUPropertyTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.Collision.InheritedUProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentCollisionInheritedUPropertyTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_CollisionInheritedProp"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("Old node"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("OpenSeq"), false));
	TestFalse(TEXT("Rename refused on inherited UPROPERTY collision"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentInheritedTargetRefusalTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.InheritedTargetRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentInheritedTargetRefusalTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_ParentInheritedTarget"), AActor::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("ParentComp added"), RenameAddSCSNode(ParentBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ParentComp")));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent generated class"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_ChildInheritedTarget"), ParentClass);
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(ChildBP, TEXT("ParentComp"), TEXT("RenamedParentComp"), false));
	TestFalse(TEXT("Rename refused on inherited target"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Error mentions parent class"), Result.ErrorMessage.Contains(ParentClass->GetName()));

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentCollisionInheritedSCSTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.Collision.InheritedSCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentCollisionInheritedSCSTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_ParentInheritedCollision"), AActor::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("ParentComp added"), RenameAddSCSNode(ParentBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ParentComp")));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent generated class"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_ChildInheritedCollision"), ParentClass);
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	TestNotNull(TEXT("Child OldComp added"), RenameAddSCSNode(ChildBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(ChildBP, TEXT("OldComp"), TEXT("ParentComp"), false));
	TestFalse(TEXT("Rename refused on inherited SCS collision"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentDependentShadowRefusalTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.DependentShadowRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentDependentShadowRefusalTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_DepParent"));
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("OldComp added"), RenameAddSCSNode(ParentBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent generated class"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_DepChild"), ParentClass);
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	TestTrue(TEXT("Child variable ShadowName added"), RenameAddBlueprintBoolVariable(ChildBP, TEXT("ShadowName")));
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(ParentBP, TEXT("OldComp"), TEXT("ShadowName"), false));
	TestFalse(TEXT("Rename refused due to dependent shadow"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentLocalReferencePatchTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.LocalReferencePatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentLocalReferencePatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_LocalRefPatch"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OldComp added"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));
	TestTrue(TEXT("VariableGet node added"), RenameAddVariableGetNode(BP, TEXT("OldComp")));
	FKismetEditorUtilities::CompileBlueprint(BP);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("NewComp"), true));
	TestTrue(TEXT("Rename succeeds"), Result.bSuccess);

	const TArray<FString> VariableGetNames = RenameCollectVariableGetNames(BP);
	TestTrue(TEXT("VariableGet references new name"), VariableGetNames.Contains(TEXT("NewComp")));
	TestFalse(TEXT("VariableGet no longer references old name"), VariableGetNames.Contains(TEXT("OldComp")));

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentDependentRecompileAndPatchTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.DependentRecompileAndPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentDependentRecompileAndPatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_DepPatchParent"));
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("OldComp added"), RenameAddSCSNode(ParentBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent generated class"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = RenameCreateLiftBP(TEXT("BP_RenameSCS_DepPatchChild"), ParentClass);
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	TestTrue(TEXT("Child VariableGet added"), RenameAddVariableGetNode(ChildBP, TEXT("OldComp")));
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(ParentBP, TEXT("OldComp"), TEXT("NewComp"), true));
	TestTrue(TEXT("Rename succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Dependents = nullptr;
		TestTrue(TEXT("dependent_blueprints returned"), Result.Data->TryGetArrayField(TEXT("dependent_blueprints"), Dependents));
		TestNotNull(TEXT("dependent_blueprints array valid"), Dependents);
		if (Dependents && Dependents->Num() > 0 && (*Dependents)[0].IsValid())
		{
			const TSharedPtr<FJsonObject>* FirstDependent = nullptr;
			TestTrue(TEXT("first dependent object"), (*Dependents)[0]->TryGetObject(FirstDependent));
			if (FirstDependent && FirstDependent->IsValid())
			{
				FString Path;
				FString Status;
				TestTrue(TEXT("dependent path exists"), (*FirstDependent)->TryGetStringField(TEXT("path"), Path));
				TestTrue(TEXT("dependent compile_status exists"), (*FirstDependent)->TryGetStringField(TEXT("compile_status"), Status));
				TestEqual(TEXT("dependent path matches child"), Path, ChildBP->GetPathName());
				TestEqual(TEXT("dependent compile_status"), Status, FString(TEXT("UpToDate")));
			}
		}
	}
	TestTrue(
		TEXT("Child blueprint compiled after parent rename"),
		ChildBP->Status == BS_UpToDate || ChildBP->Status == BS_UpToDateWithWarnings);

	const TArray<FString> VariableGetNames = RenameCollectVariableGetNames(ChildBP);
	TestTrue(TEXT("Child VariableGet references new name"), VariableGetNames.Contains(TEXT("NewComp")));
	TestFalse(TEXT("Child VariableGet no longer references old name"), VariableGetNames.Contains(TEXT("OldComp")));

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentTimelineRefusalTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.TimelineRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentTimelineRefusalTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_Timeline"), AActor::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* TimelineNode = RenameAddSCSNode(BP, UTimelineComponent::StaticClass(), TEXT("TimelineComp"));
	if (!TimelineNode)
	{
		AddInfo(TEXT("UTimelineComponent SCS fixture could not be created in this test environment; skipping timeline refusal check."));
		BP->MarkAsGarbage();
		return true;
	}

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("TimelineComp"), TEXT("RenamedTimelineComp"), false));
	TestFalse(TEXT("Timeline rename refused"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentTimelineReferenceRefusalTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.TimelineReferenceRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentTimelineReferenceRefusalTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_TimelineReference"), AActor::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* TimelineNode = RenameAddSCSNode(BP, UTimelineComponent::StaticClass(), TEXT("TimelineComp"));
	USCS_Node* HolderNode = RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("HolderComp"));
	if (!TimelineNode || !HolderNode)
	{
		AddInfo(TEXT("Timeline-reference refusal fixture could not be created in this runtime; skipping."));
		BP->MarkAsGarbage();
		return true;
	}

	UTimelineComponent* TimelineTemplate = RenameGetTimelineTemplate(BP, TEXT("TimelineComp"));
	UCortexBPTestSubobjComponent* HolderTemplate = RenameGetSubobjTemplate(BP, TEXT("HolderComp"));
	TestNotNull(TEXT("Timeline template exists"), TimelineTemplate);
	TestNotNull(TEXT("Holder template exists"), HolderTemplate);
	if (!TimelineTemplate || !HolderTemplate)
	{
		BP->MarkAsGarbage();
		return false;
	}

	HolderTemplate->TimelineDependency = TimelineTemplate;

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("HolderComp"), TEXT("RenamedHolder"), false));
	TestFalse(TEXT("Rename refused when component references local timeline"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Error mentions timeline"), Result.ErrorMessage.Contains(TEXT("timeline")));

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameSCSComponentUndoTest,
	"Cortex.Blueprint.Cleanup.RenameSCSComponent.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRenameSCSComponentUndoTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RenameCreateLiftBP(TEXT("BP_RenameSCS_Undo"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OldComp added"), RenameAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OldComp")));

	if (!GEditor || !GEditor->Trans || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system unavailable; skipping undo verification."));
		BP->MarkAsGarbage();
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex RenameSCS Undo Test Setup")));
	const int32 InitialQueueLength = GEditor->Trans->GetQueueLength();

	const FCortexCommandResult Result = FCortexBPCleanupOps::RenameSCSComponent(
		RenameMakeParams(BP, TEXT("OldComp"), TEXT("NewComp"), false));
	TestTrue(TEXT("Rename succeeds"), Result.bSuccess);

	const int32 FinalQueueLength = GEditor->Trans->GetQueueLength();
	TestTrue(TEXT("Rename creates a new undo transaction"), FinalQueueLength > InitialQueueLength);
	TestTrue(TEXT("Undo is available after rename"), GEditor->Trans->CanUndo());
	if (FinalQueueLength <= InitialQueueLength || !GEditor->Trans->CanUndo())
	{
		GEditor->ResetTransaction(FText::FromString(TEXT("Cortex RenameSCS Undo Test Cleanup")));
		BP->MarkAsGarbage();
		return false;
	}

	const bool bUndid = GEditor->UndoTransaction();
	TestTrue(TEXT("UndoTransaction succeeds"), bUndid);
	TestTrue(TEXT("OldComp restored after undo"), RenameHasSCSNode(BP, TEXT("OldComp")));
	TestFalse(TEXT("NewComp removed after undo"), RenameHasSCSNode(BP, TEXT("NewComp")));

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex RenameSCS Undo Test Cleanup")));
	BP->MarkAsGarbage();
	return true;
}
