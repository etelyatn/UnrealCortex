#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPTestLiftActor.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* AnalyzeCollisionCreateBlueprint(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* AnalyzeCollisionAddSCSNode(
		UBlueprint* BP,
		UClass* ComponentClass,
		const TCHAR* VariableName,
		bool bCompileAfterAdd = false)
	{
		if (!BP || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(ComponentClass, FName(VariableName));
		BP->SimpleConstructionScript->AddNode(Node);
		Node->SetVariableName(FName(VariableName), false);
		if (bCompileAfterAdd)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
		}
		return Node;
	}

	FCortexCommandResult AnalyzeCollisionRun(UBlueprint* BP)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP ? BP->GetPathName() : TEXT(""));
		return Handler.Execute(TEXT("analyze_for_migration"), Params);
	}

	const TSharedPtr<FJsonObject> AnalyzeCollisionFindEntry(
		const TArray<TSharedPtr<FJsonValue>>* Collisions,
		const FString& Name,
		const FString& Kind = TEXT(""))
	{
		if (!Collisions)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Collisions)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}

			FString EntryName;
			FString EntryKind;
			Entry->TryGetStringField(TEXT("name"), EntryName);
			Entry->TryGetStringField(TEXT("inherited_kind"), EntryKind);

			if (EntryName == Name && (Kind.IsEmpty() || EntryKind == Kind))
			{
				return Entry;
			}
		}

		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeMigrationCollisionCleanTest,
	"Cortex.Blueprint.AnalyzeForMigration.SCSCollisions.Clean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalyzeMigrationCollisionCleanTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionClean"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("UniqueComp added"),
		AnalyzeCollisionAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("UniqueComp")));

	const FCortexCommandResult Result = AnalyzeCollisionRun(BP);
	TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
	TestTrue(TEXT("data is valid"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Collisions = nullptr;
		TestTrue(TEXT("scs_collisions field exists"), Result.Data->TryGetArrayField(TEXT("scs_collisions"), Collisions));
		TestEqual(TEXT("clean blueprint has no collisions"), Collisions ? Collisions->Num() : -1, 0);
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeMigrationCollisionAdoptableUPropertyTest,
	"Cortex.Blueprint.AnalyzeForMigration.SCSCollisions.AdoptableUProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalyzeMigrationCollisionAdoptableUPropertyTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionAdoptable"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OpenSeq SCS added"),
		AnalyzeCollisionAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OpenSeq")));

	const FCortexCommandResult Result = AnalyzeCollisionRun(BP);
	TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		BP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Collisions = nullptr;
	TestTrue(TEXT("scs_collisions exists"), Result.Data->TryGetArrayField(TEXT("scs_collisions"), Collisions));
	const TSharedPtr<FJsonObject> Collision = AnalyzeCollisionFindEntry(Collisions, TEXT("OpenSeq"), TEXT("uproperty"));
	TestTrue(TEXT("OpenSeq UPROPERTY collision found"), Collision.IsValid());
	if (Collision.IsValid())
	{
		FString Severity;
		FString Action;
		FString Tool;
		Collision->TryGetStringField(TEXT("severity"), Severity);
		Collision->TryGetStringField(TEXT("recommended_action"), Action);
		Collision->TryGetStringField(TEXT("recommended_tool"), Tool);
		TestEqual(TEXT("severity is adoptable"), Severity, FString(TEXT("adoptable")));
		TestEqual(TEXT("recommended_action is adopt_into_parent"), Action, FString(TEXT("adopt_into_parent")));
		TestEqual(TEXT("tool is set_class_defaults"), Tool, FString(TEXT("blueprint.set_class_defaults")));

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		TestTrue(TEXT("recommended_params exists"), Collision->TryGetObjectField(TEXT("recommended_params"), ParamsObj));
		if (ParamsObj && ParamsObj->IsValid())
		{
			const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
			TestTrue(TEXT("properties payload exists"),
				(*ParamsObj)->TryGetObjectField(TEXT("properties"), PropertiesObj));
			if (PropertiesObj && PropertiesObj->IsValid())
			{
				FString EchoValue;
				TestTrue(TEXT("properties contains OpenSeq"),
					(*PropertiesObj)->TryGetStringField(TEXT("OpenSeq"), EchoValue));
				TestEqual(TEXT("bare-name value echoes OpenSeq"), EchoValue, FString(TEXT("OpenSeq")));
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeMigrationCollisionBlockingUPropertyTest,
	"Cortex.Blueprint.AnalyzeForMigration.SCSCollisions.BlockingUProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalyzeMigrationCollisionBlockingUPropertyTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionBlocking"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("Mesh SCS added with mismatched type"),
		AnalyzeCollisionAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("Mesh")));

	const FCortexCommandResult Result = AnalyzeCollisionRun(BP);
	TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		BP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Collisions = nullptr;
	TestTrue(TEXT("scs_collisions exists"), Result.Data->TryGetArrayField(TEXT("scs_collisions"), Collisions));
	const TSharedPtr<FJsonObject> Collision = AnalyzeCollisionFindEntry(Collisions, TEXT("Mesh"), TEXT("uproperty"));
	TestTrue(TEXT("Mesh UPROPERTY collision found"), Collision.IsValid());
	if (Collision.IsValid())
	{
		FString Severity;
		FString Action;
		FString Tool;
		Collision->TryGetStringField(TEXT("severity"), Severity);
		Collision->TryGetStringField(TEXT("recommended_action"), Action);
		Collision->TryGetStringField(TEXT("recommended_tool"), Tool);
		TestEqual(TEXT("severity is blocking"), Severity, FString(TEXT("blocking")));
		TestEqual(TEXT("recommended_action is rename"), Action, FString(TEXT("rename")));
		TestEqual(TEXT("tool is rename_scs_component"), Tool, FString(TEXT("blueprint.rename_scs_component")));

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		TestTrue(TEXT("recommended_params exists"), Collision->TryGetObjectField(TEXT("recommended_params"), ParamsObj));
		if (ParamsObj && ParamsObj->IsValid())
		{
			FString OldName;
			FString NewName;
			TestTrue(TEXT("old_name exists"), (*ParamsObj)->TryGetStringField(TEXT("old_name"), OldName));
			TestTrue(TEXT("new_name exists"), (*ParamsObj)->TryGetStringField(TEXT("new_name"), NewName));
			TestEqual(TEXT("old_name is Mesh"), OldName, FString(TEXT("Mesh")));
			TestFalse(TEXT("new_name is non-empty"), NewName.IsEmpty());
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeMigrationCollisionInheritedSCSTest,
	"Cortex.Blueprint.AnalyzeForMigration.SCSCollisions.InheritedSCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalyzeMigrationCollisionInheritedSCSTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionSCSParent"), AActor::StaticClass());
	TestNotNull(TEXT("Parent blueprint created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("Parent SharedComp added"),
		AnalyzeCollisionAddSCSNode(ParentBP, USceneComponent::StaticClass(), TEXT("SharedComp"), true));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent class generated"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionSCSChild"), ParentClass);
	TestNotNull(TEXT("Child blueprint created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	TestNotNull(TEXT("Child SharedComp added"),
		AnalyzeCollisionAddSCSNode(ChildBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("SharedComp")));

	const FCortexCommandResult Result = AnalyzeCollisionRun(ChildBP);
	TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		ChildBP->MarkAsGarbage();
		ParentBP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Collisions = nullptr;
	TestTrue(TEXT("scs_collisions exists"), Result.Data->TryGetArrayField(TEXT("scs_collisions"), Collisions));
	const TSharedPtr<FJsonObject> Collision = AnalyzeCollisionFindEntry(Collisions, TEXT("SharedComp"), TEXT("scs"));
	TestTrue(TEXT("Inherited SCS collision found"), Collision.IsValid());

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeMigrationCollisionDelegateTest,
	"Cortex.Blueprint.AnalyzeForMigration.SCSCollisions.Delegate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalyzeMigrationCollisionDelegateTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = AnalyzeCollisionCreateBlueprint(TEXT("BP_AnalyzeCollisionDelegate"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OnLifted SCS added"),
		AnalyzeCollisionAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OnLifted")));

	const FCortexCommandResult Result = AnalyzeCollisionRun(BP);
	TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		BP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Collisions = nullptr;
	TestTrue(TEXT("scs_collisions exists"), Result.Data->TryGetArrayField(TEXT("scs_collisions"), Collisions));
	const TSharedPtr<FJsonObject> Collision = AnalyzeCollisionFindEntry(Collisions, TEXT("OnLifted"), TEXT("delegate"));
	TestTrue(TEXT("Delegate collision found"), Collision.IsValid());
	if (Collision.IsValid())
	{
		FString Severity;
		FString Action;
		FString Tool;
		Collision->TryGetStringField(TEXT("severity"), Severity);
		Collision->TryGetStringField(TEXT("recommended_action"), Action);
		Collision->TryGetStringField(TEXT("recommended_tool"), Tool);
		TestEqual(TEXT("severity is blocking"), Severity, FString(TEXT("blocking")));
		TestEqual(TEXT("recommended_action is rename"), Action, FString(TEXT("rename")));
		TestEqual(TEXT("tool is rename_scs_component"), Tool, FString(TEXT("blueprint.rename_scs_component")));
	}

	BP->MarkAsGarbage();
	return true;
}
