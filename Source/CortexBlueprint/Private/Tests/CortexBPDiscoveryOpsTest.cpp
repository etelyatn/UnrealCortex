#include "Misc/AutomationTest.h"

#include "CortexBPTestLiftActor.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* CreateDiscoveryBlueprint(const TCHAR* Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* AddDiscoveryComponent(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VariableName)
	{
		if (!BP || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(ComponentClass, FName(VariableName));
		if (!Node)
		{
			return nullptr;
		}

		BP->SimpleConstructionScript->AddNode(Node);
		Node->SetVariableName(FName(VariableName), false);
		FKismetEditorUtilities::CompileBlueprint(BP);
		return Node;
	}

	const TSharedPtr<FJsonObject>* FindObjectInArrayByStringField(
		const TArray<TSharedPtr<FJsonValue>>* Array,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		if (!Array)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}

			FString Value;
			if ((*Obj)->TryGetStringField(FieldName, Value) && Value == ExpectedValue)
			{
				return Obj;
			}
		}

		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListSCSComponentsTest,
	"Cortex.Blueprint.Discovery.ListSCSComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListSCSComponentsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryListSCSComponents"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(
		TEXT("Discovery component added"),
		AddDiscoveryComponent(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ExtraComp")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FCortexCommandResult Result = FCortexBPComponentOps::ListSCSComponents(Params);

	TestTrue(TEXT("list_scs_components succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		TestTrue(TEXT("components array exists"), Result.Data->TryGetArrayField(TEXT("components"), Components));
		const TSharedPtr<FJsonObject>* ComponentObj = FindObjectInArrayByStringField(
			Components,
			TEXT("name"),
			TEXT("ExtraComp"));
		TestNotNull(TEXT("ExtraComp is returned"), ComponentObj);
		if (ComponentObj && *ComponentObj)
		{
			FString ReferenceForm;
			TestTrue(
				TEXT("reference_form exists"),
				(*ComponentObj)->TryGetStringField(TEXT("reference_form"), ReferenceForm));
			TestTrue(
				TEXT("reference_form uses generated variable path"),
				ReferenceForm.Contains(TEXT("ExtraComp_GEN_VARIABLE")));
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListInheritedPropertiesTest,
	"Cortex.Blueprint.Discovery.ListInheritedProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListInheritedPropertiesTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryInheritedProps"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);

	TestTrue(TEXT("list_inherited_properties succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(TEXT("properties object exists"), Result.Data->TryGetObjectField(TEXT("properties"), Properties));
		if (Properties && *Properties)
		{
			const TSharedPtr<FJsonObject>* MeshObj = nullptr;
			TestTrue(TEXT("Mesh property returned"), (*Properties)->TryGetObjectField(TEXT("Mesh"), MeshObj));
			if (MeshObj && *MeshObj)
			{
				TestTrue(TEXT("accepted_formats exists"), (*MeshObj)->HasField(TEXT("accepted_formats")));
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListSettableDefaultsTest,
	"Cortex.Blueprint.Discovery.ListSettableDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListSettableDefaultsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoverySettableDefaults"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);

	TestTrue(TEXT("list_settable_defaults succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(TEXT("properties object exists"), Result.Data->TryGetObjectField(TEXT("properties"), Properties));
		if (Properties && *Properties)
		{
			const TSharedPtr<FJsonObject>* OpenSeqObj = nullptr;
			TestTrue(TEXT("OpenSeq property returned"), (*Properties)->TryGetObjectField(TEXT("OpenSeq"), OpenSeqObj));
			if (OpenSeqObj && *OpenSeqObj)
			{
				TestTrue(TEXT("value exists"), (*OpenSeqObj)->HasField(TEXT("value")));
				TestTrue(TEXT("accepted_formats exists"), (*OpenSeqObj)->HasField(TEXT("accepted_formats")));
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}
