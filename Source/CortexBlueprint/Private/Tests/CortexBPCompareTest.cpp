#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCompareOps.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompareIdenticalTest,
	"Cortex.Blueprint.Compare.Identical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompareIdenticalTest::RunTest(const FString& Parameters)
{
	UBlueprint* SourceBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_Compare_Source")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Source BP created"), SourceBP);
	if (!SourceBP)
	{
		return false;
	}

	UBlueprint* TargetBP = Cast<UBlueprint>(StaticDuplicateObject(SourceBP, GetTransientPackage(), FName(TEXT("BP_Compare_Target"))));
	TestNotNull(TEXT("Target BP duplicated"), TargetBP);
	if (!TargetBP)
	{
		SourceBP->MarkAsGarbage();
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(SourceBP);
	FKismetEditorUtilities::CompileBlueprint(TargetBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("source_path"), SourceBP->GetPathName());
	Params->SetStringField(TEXT("target_path"), TargetBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCompareOps::CompareBlueprints(Params);
	TestTrue(TEXT("compare_blueprints succeeded"), Result.bSuccess);

	bool bMatch = false;
	if (Result.Data.IsValid())
	{
		Result.Data->TryGetBoolField(TEXT("match"), bMatch);
	}
	TestTrue(TEXT("Identical blueprints should match"), bMatch);

	TargetBP->MarkAsGarbage();
	SourceBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompareDifferentTest,
	"Cortex.Blueprint.Compare.Different",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompareDifferentTest::RunTest(const FString& Parameters)
{
	UBlueprint* SourceBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CompareDiff_Source")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Source BP created"), SourceBP);
	if (!SourceBP)
	{
		return false;
	}

	UBlueprint* TargetBP = Cast<UBlueprint>(StaticDuplicateObject(SourceBP, GetTransientPackage(), FName(TEXT("BP_CompareDiff_Target"))));
	TestNotNull(TEXT("Target BP duplicated"), TargetBP);
	if (!TargetBP)
	{
		SourceBP->MarkAsGarbage();
		return false;
	}

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(TargetBP, TEXT("OnlyInTarget"), IntType, TEXT("1"));

	FKismetEditorUtilities::CompileBlueprint(SourceBP);
	FKismetEditorUtilities::CompileBlueprint(TargetBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("source_path"), SourceBP->GetPathName());
	Params->SetStringField(TEXT("target_path"), TargetBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCompareOps::CompareBlueprints(Params);
	TestTrue(TEXT("compare_blueprints succeeded"), Result.bSuccess);

	bool bMatch = true;
	int32 DifferenceCount = 0;
	if (Result.Data.IsValid())
	{
		Result.Data->TryGetBoolField(TEXT("match"), bMatch);
		const TSharedPtr<FJsonObject>* Summary = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("summary"), Summary) && Summary && Summary->IsValid())
		{
			double DifferenceCountDouble = 0;
			if ((*Summary)->TryGetNumberField(TEXT("differences"), DifferenceCountDouble))
			{
				DifferenceCount = static_cast<int32>(DifferenceCountDouble);
			}
		}
	}
	TestFalse(TEXT("Different blueprints should not match"), bMatch);
	TestTrue(TEXT("Different blueprints should report at least one difference"), DifferenceCount > 0);

	TargetBP->MarkAsGarbage();
	SourceBP->MarkAsGarbage();
	return true;
}
