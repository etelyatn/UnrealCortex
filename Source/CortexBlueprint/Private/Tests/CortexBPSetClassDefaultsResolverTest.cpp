#include "Misc/AutomationTest.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "CortexBPTestLiftActor.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* ResolverCreateLiftBlueprint(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* ResolverAddSCSNode(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VariableName, bool bCompile = true)
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
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
		}
		return Node;
	}

	TSharedPtr<FJsonObject> ResolverMakeSetParams(
		UBlueprint* BP,
		const FString& PropertyName,
		const FString& PropertyValue,
		bool bCompile = false,
		bool bSave = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(PropertyName, PropertyValue);
		Params->SetObjectField(TEXT("properties"), Properties);
		Params->SetBoolField(TEXT("compile"), bCompile);
		Params->SetBoolField(TEXT("save"), bSave);
		return Params;
	}

	TArray<FString> ResolverReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Values) || !Values)
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
	FCortexBPSetClassDefaultsResolverOwnSCSTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.OwnSCSBareName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverOwnSCSTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverOwnSCS"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("MyComp added"), ResolverAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("MyComp")));
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("OpenSeq"), TEXT("MyComp")));

	TestTrue(TEXT("SetClassDefaults succeeds for own SCS bare-name"), Result.bSuccess);
	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverParentSCSTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.ParentSCSBareName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverParentSCSTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverParent"));
	TestNotNull(TEXT("Parent blueprint created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	TestNotNull(TEXT("ParentComp added"),
		ResolverAddSCSNode(ParentBP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ParentComp")));
	UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(ParentBP->GeneratedClass);
	TestNotNull(TEXT("Parent generated class exists"), ParentClass);
	if (!ParentClass)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	UBlueprint* ChildBP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverParentChild"), ParentClass);
	TestNotNull(TEXT("Child blueprint created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(ChildBP, TEXT("OpenSeq"), TEXT("ParentComp")));
	TestTrue(TEXT("SetClassDefaults succeeds for parent SCS bare-name"), Result.bSuccess);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverNativeDefaultSubobjectTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.NativeDefaultSubobjectBareName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverNativeDefaultSubobjectTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverNative"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("Mesh"), TEXT("Mesh")));

	TestTrue(TEXT("SetClassDefaults succeeds for native default subobject bare-name"), Result.bSuccess);
	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverFullPathCompatibilityTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.FullPathCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverFullPathCompatibilityTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverFullPath"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = ResolverAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("PathComp"));
	TestNotNull(TEXT("PathComp added"), Node);
	if (!Node)
	{
		BP->MarkAsGarbage();
		return false;
	}

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
	TestNotNull(TEXT("Generated class exists"), BPGC);
	UCortexBPTestSubobjComponent* Template =
		(BPGC && Node) ? Cast<UCortexBPTestSubobjComponent>(Node->GetActualComponentTemplate(BPGC)) : nullptr;
	TestNotNull(TEXT("Template exists"), Template);
	if (!Template)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("OpenSeq"), Template->GetPathName()));
	TestTrue(TEXT("Full object-path format still succeeds"), Result.bSuccess);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverAmbiguousShadowTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.AmbiguousShadowRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverAmbiguousShadowTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverAmbiguous"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OpenSeq SCS added"),
		ResolverAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OpenSeq"), false));

	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("OpenSeq"), TEXT("OpenSeq")));

	TestFalse(TEXT("Ambiguous bare-name should be refused"), Result.bSuccess);
	TestEqual(TEXT("Error code is AmbiguousComponentReference"),
		Result.ErrorCode, CortexErrorCodes::AmbiguousComponentReference);
	if (Result.ErrorDetails.IsValid())
	{
		const TArray<FString> Candidates = ResolverReadStringArray(Result.ErrorDetails, TEXT("candidates"));
		TestTrue(TEXT("Candidates include @self qualifier"),
			Candidates.Contains(TEXT("OpenSeq@self")));
		TestTrue(TEXT("Candidates include inherited qualifier"),
			Candidates.Contains(FString::Printf(TEXT("OpenSeq@%s"), *ACortexBPTestLiftActor::StaticClass()->GetName())));
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverTypeMismatchCloseMatchTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.TypeMismatchCloseMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverTypeMismatchCloseMatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverCloseMatch"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("OpenSeqComp added"),
		ResolverAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("OpenSeqComp")));

	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("OpenSeq"), TEXT("OpenSeqCmp")));

	TestFalse(TEXT("Type mismatch expected on miss"), Result.bSuccess);
	TestEqual(TEXT("Error code is TypeMismatch"), Result.ErrorCode, CortexErrorCodes::TypeMismatch);
	if (Result.ErrorDetails.IsValid())
	{
		FString ClosestMatch;
		TestTrue(TEXT("closest_match exists"), Result.ErrorDetails->TryGetStringField(TEXT("closest_match"), ClosestMatch));
		TestEqual(TEXT("closest_match points to OpenSeqComp"), ClosestMatch, FString(TEXT("OpenSeqComp")));
		TestTrue(TEXT("retry_with exists"), Result.ErrorDetails->HasField(TEXT("retry_with")));
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsResolverTypeMismatchNoCloseMatchTest,
	"Cortex.Blueprint.ClassDefaults.Resolver.TypeMismatchNoCloseMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsResolverTypeMismatchNoCloseMatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = ResolverCreateLiftBlueprint(TEXT("BP_SetDefaultsResolverNoCloseMatch"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(TEXT("FarComp added"),
		ResolverAddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("FarComp")));

	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::SetClassDefaults(
		ResolverMakeSetParams(BP, TEXT("OpenSeq"), TEXT("TotallyUnknownName")));

	TestFalse(TEXT("Type mismatch expected"), Result.bSuccess);
	TestEqual(TEXT("Error code is TypeMismatch"), Result.ErrorCode, CortexErrorCodes::TypeMismatch);
	if (Result.ErrorDetails.IsValid())
	{
		TestTrue(TEXT("accepted_formats exists"), Result.ErrorDetails->HasField(TEXT("accepted_formats")));
		TestTrue(TEXT("scs_nodes_in_blueprint exists"), Result.ErrorDetails->HasField(TEXT("scs_nodes_in_blueprint")));
		TestTrue(TEXT("native_default_subobjects exists"), Result.ErrorDetails->HasField(TEXT("native_default_subobjects")));
		TestFalse(TEXT("retry_with should be absent for no close match"), Result.ErrorDetails->HasField(TEXT("retry_with")));
	}

	BP->MarkAsGarbage();
	return true;
}
