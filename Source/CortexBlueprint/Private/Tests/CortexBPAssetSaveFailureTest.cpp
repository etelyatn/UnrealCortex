#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// bp.save persists the package itself, so a package the engine refuses to save is a save failure
// (rule 3), not a serialization error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSaveFailureTest,
	"Cortex.Blueprint.Asset.SaveFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSaveFailureTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	UPackage* TestPackage = CreatePackage(*FString::Printf(
		TEXT("/Game/Temp/CortexBPSaveFailure_%s"), *Suffix));

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		FName(*FString::Printf(TEXT("BP_SaveFailure_%s"), *Suffix)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());

	// The editor's save hook refuses exactly this package. The command saves with SAVE_NoError, so
	// the engine reports the refusal as a return value instead of logging an error.
	const FString RefusedPackageName = TestPackage->GetName();
	FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda(
		[RefusedPackageName](UPackage* Package, const FString& Filename, FOutputDevice* ErrorText)
		{
			(void)Filename;
			(void)ErrorText;
			return Package == nullptr || Package->GetName() != RefusedPackageName;
		});

	const FCortexCommandResult Result = Handler.Execute(TEXT("save"), Params);
	FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Unbind();

	TestFalse(TEXT("a refused package save fails bp.save"), Result.bSuccess);
	TestEqual(TEXT("refused package save uses SAVE_FAILED"), Result.ErrorCode, CortexErrorCodes::SaveFailed);

	TestBP->MarkAsGarbage();
	return true;
}
