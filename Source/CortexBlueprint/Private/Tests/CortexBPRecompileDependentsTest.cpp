#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsTest,
	"Cortex.Blueprint.RecompileDependents.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
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
		GetTransientPackage(),
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
