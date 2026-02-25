#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRemoveVariableTest,
	"Cortex.Blueprint.Cleanup.RemoveVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRemoveVariableTest::RunTest(const FString& Parameters)
{
	// Create Blueprint with a variable
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
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
