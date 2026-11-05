#include "Misc/AutomationTest.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** Same keys and values `compile` produced before the extraction. */
	bool PayloadMatches(FAutomationTestBase& Test, const TCHAR* Label, const TSharedPtr<FJsonObject>& Payload, const FCortexBPCompileOutcome& Outcome, const FString& AssetPath)
	{
		if (!Test.TestTrue(FString::Printf(TEXT("%s payload"), Label), Payload.IsValid()))
		{
			return false;
		}
		FString AssetField;
		FString Status;
		FString Alias;
		double Errors = -1;
		double Warnings = -1;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		Test.TestTrue(TEXT("asset_path"), Payload->TryGetStringField(TEXT("asset_path"), AssetField));
		Test.TestEqual(TEXT("asset_path value"), AssetField, AssetPath);
		Test.TestTrue(TEXT("compile_status"), Payload->TryGetStringField(TEXT("compile_status"), Status));
		Test.TestTrue(TEXT("status"), Payload->TryGetStringField(TEXT("status"), Alias));
		Test.TestEqual(TEXT("status alias"), Alias, Status);
		Test.TestEqual(FString::Printf(TEXT("%s status"), Label), Status, Outcome.Status);
		Test.TestTrue(TEXT("error_count"), Payload->TryGetNumberField(TEXT("error_count"), Errors));
		Test.TestTrue(TEXT("warning_count"), Payload->TryGetNumberField(TEXT("warning_count"), Warnings));
		Test.TestEqual(TEXT("error_count value"), static_cast<int32>(Errors), Outcome.ErrorCount);
		Test.TestEqual(TEXT("warning_count value"), static_cast<int32>(Warnings), Outcome.WarningCount);
		Test.TestTrue(TEXT("diagnostics"), Payload->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
		return true;
	}

	UBlueprint* MakeBrokenBlueprint()
	{
		UBlueprint* Blueprint = CortexAdoptTest::MakeBlueprint(TEXT("BP_CompileSupportBroken"), AActor::StaticClass());
		if (Blueprint == nullptr || Blueprint->UbergraphPages.Num() == 0)
		{
			return Blueprint;
		}
		UEdGraph* EventGraph = Blueprint->UbergraphPages[0];
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
			Event->CreateNewGuid();
			Event->CustomFunctionName = FName("DuplicateEventName");
			EventGraph->AddNode(Event, false, false);
			Event->AllocateDefaultPins();
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		return Blueprint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileSupportTest,
	"Cortex.Blueprint.CompileSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCompileSupportTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	// Clean Blueprint: shape equals the `compile` command payload.
	UBlueprint* Clean = MakeBlueprint(TEXT("BP_CompileSupportClean"), AActor::StaticClass());
	if (TestNotNull(TEXT("Clean Blueprint"), Clean))
	{
		FCortexBPCompileOutcome Outcome;
		FCortexBPCompileSupport::Compile(Clean, EBlueprintCompileOptions::None, Outcome);
		TestEqual(TEXT("Clean status"), Outcome.Status, FString(TEXT("success")));
		TestEqual(TEXT("Clean errors"), Outcome.ErrorCount, 0);
		TestEqual(TEXT("Clean warnings"), Outcome.WarningCount, 0);
		TestFalse(TEXT("Clean has no errors"), Outcome.HasErrors());
		PayloadMatches(*this, TEXT("clean"), Outcome.ToPayload(Clean->GetPathName()), Outcome, Clean->GetPathName());

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Clean->GetPathName());
		const FCortexCommandResult Command = FCortexBPAssetOps::Compile(Params);
		TestTrue(TEXT("compile command succeeds"), Command.bSuccess);
		if (Command.Data.IsValid())
		{
			PayloadMatches(*this, TEXT("clean command"), Command.Data, Outcome, Clean->GetPathName());
		}
	}

	// Error Blueprint: same counts through the command, COMPILE_FAILED with the payload as details.
	UBlueprint* Broken = MakeBrokenBlueprint();
	if (TestNotNull(TEXT("Broken Blueprint"), Broken))
	{
		FCortexBPCompileOutcome Outcome;
		FCortexBPCompileSupport::Compile(Broken, EBlueprintCompileOptions::None, Outcome);
		TestEqual(TEXT("Error status"), Outcome.Status, FString(TEXT("error")));
		TestTrue(TEXT("Error counted"), Outcome.HasErrors());
		TestTrue(TEXT("Diagnostics reported"), Outcome.Diagnostics.Num() > 0);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Broken->GetPathName());
		const FCortexCommandResult Command = FCortexBPAssetOps::Compile(Params);
		TestFalse(TEXT("compile command fails"), Command.bSuccess);
		TestEqual(TEXT("compile command code"), Command.ErrorCode, CortexErrorCodes::CompileFailed);
		if (TestTrue(TEXT("compile command details"), Command.ErrorDetails.IsValid()))
		{
			PayloadMatches(*this, TEXT("error command"), Command.ErrorDetails, Outcome, Broken->GetPathName());
			const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
			if (Command.ErrorDetails->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics->Num() > 0)
			{
				const TSharedPtr<FJsonObject> First = (*Diagnostics)[0]->AsObject();
				for (const TCHAR* Key : { TEXT("graph"), TEXT("node_id"), TEXT("node_class"), TEXT("node_name"), TEXT("severity"), TEXT("message") })
				{
					TestTrue(FString::Printf(TEXT("diagnostic field %s"), Key), First.IsValid() && First->HasField(Key));
				}
			}
		}
	}

	// Warning Blueprint: a deprecated callable produces a warning where the runtime has one.
	UBlueprint* Warn = MakeBlueprint(TEXT("BP_CompileSupportWarn"), AActor::StaticClass());
	bool bWarningFound = false;
	if (TestNotNull(TEXT("Warning Blueprint"), Warn) && Warn->UbergraphPages.Num() > 0)
	{
		UEdGraph* EventGraph = Warn->UbergraphPages[0];
		TArray<UFunction*> Candidates;
		for (TObjectIterator<UFunction> It; It; ++It)
		{
			UFunction* Function = *It;
			UClass* Owner = Function ? Function->GetOwnerClass() : nullptr;
			if (Function && IsValid(Function) && Owner && IsValid(Owner) && Owner->IsNative()
				&& Function->HasAllFunctionFlags(FUNC_BlueprintCallable | FUNC_Static)
				&& Function->HasMetaData(TEXT("DeprecatedFunction"))
				&& Owner->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
			{
				Candidates.Add(Function);
			}
		}
		Candidates.Sort([](const UFunction& A, const UFunction& B) { return A.GetPathName() < B.GetPathName(); });
		for (UFunction* Candidate : Candidates)
		{
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(EventGraph);
			Node->CreateNewGuid();
			Node->SetFromFunction(Candidate);
			EventGraph->AddNode(Node, false, false);
			Node->AllocateDefaultPins();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Warn);

			FCortexBPCompileOutcome Outcome;
			FCortexBPCompileSupport::Compile(Warn, EBlueprintCompileOptions::None, Outcome);
			if (Outcome.Status == TEXT("warning") && Outcome.ErrorCount == 0 && Outcome.WarningCount > 0)
			{
				bWarningFound = true;
				PayloadMatches(*this, TEXT("warning"), Outcome.ToPayload(Warn->GetPathName()), Outcome, Warn->GetPathName());
				break;
			}
			EventGraph->RemoveNode(Node);
			Node->MarkAsGarbage();
		}
	}
	if (!bWarningFound)
	{
		AddInfo(TEXT("Warning path skipped: no deprecated callable produced a compile warning in this runtime"));
	}

	// Delta options are forwarded to the engine call untouched.
	UBlueprint* Delta = MakeBlueprint(TEXT("BP_CompileSupportDelta"), AActor::StaticClass());
	if (TestNotNull(TEXT("Delta Blueprint"), Delta))
	{
		TArray<EBlueprintCompileOptions> Seen;
		FCortexBPCompileSupport::SetCompileObserverForTest([&Seen](UBlueprint*, EBlueprintCompileOptions Options) { Seen.Add(Options); });
		const EBlueprintCompileOptions Requested = EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing
			| EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
		FCortexBPCompileOutcome Outcome;
		FCortexBPCompileSupport::Compile(Delta, Requested, Outcome);
		FCortexBPCompileSupport::ClearCompileObserverForTest();
		TestEqual(TEXT("One compile observed"), Seen.Num(), 1);
		if (Seen.Num() == 1)
		{
			TestTrue(TEXT("Options forwarded verbatim"), Seen[0] == Requested);
		}
		TestEqual(TEXT("Delta compile is clean"), Outcome.Status, FString(TEXT("success")));
	}

	for (UBlueprint* Blueprint : { Clean, Broken, Warn, Delta })
	{
		Discard(Blueprint);
	}
	return true;
}
