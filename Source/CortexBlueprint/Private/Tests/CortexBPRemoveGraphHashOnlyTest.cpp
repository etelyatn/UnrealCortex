// blueprint.remove_graph: expected_validation_hash alone authorizes apply (#277 item 2).

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace CortexIssue277RemoveGraphHashOnlyTest
{
	UBlueprint* CreateFixture(const TCHAR* AssetPath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(FString(AssetPath));
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), false, true);
		UPackage* Package = CreatePackage(*PackageName);
		return Package ? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass()) : nullptr;
	}

	void Retire(UBlueprint* Blueprint)
	{
		if (Blueprint == nullptr)
		{
			return;
		}
		UPackage* Package = Blueprint->GetOutermost();
		ForEachObjectWithOuter(Package, [](UObject* Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			if (!Object->IsA<UField>() || Object->IsA<UClass>())
			{
				Object->MarkAsGarbage();
			}
		});
		Package->MarkAsGarbage();
	}

	TSharedPtr<FJsonObject> Request(const FString& AssetPath, const bool bDryRun)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("name"), TEXT("DeleteMe"));
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("compile"), false);
		Params->SetBoolField(TEXT("save"), false);
		Params->SetBoolField(TEXT("cascade_exec_chain"), false);
		return Params;
	}

	/** The apply request bound to a preview by the hash alone. */
	TSharedPtr<FJsonObject> HashOnlyApply(const FString& AssetPath, const FCortexCommandResult& Preview)
	{
		TSharedPtr<FJsonObject> Params = Request(AssetPath, false);
		Params->SetStringField(TEXT("expected_validation_hash"), Preview.Data->GetStringField(TEXT("validation_hash")));
		return Params;
	}

	bool HasFunction(const UBlueprint* Blueprint, const TCHAR* Name)
	{
		return Blueprint->FunctionGraphs.ContainsByPredicate(
			[Name](const UEdGraph* Graph) { return Graph != nullptr && Graph->GetName() == Name; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphHashOnlyApplyTest,
	"Cortex.Blueprint.RemoveGraph.Apply.HashOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphHashOnlyApplyTest::RunTest(const FString&)
{
	using CortexIssue277RemoveGraphHashOnlyTest::CreateFixture;
	using CortexIssue277RemoveGraphHashOnlyTest::Retire;
	using CortexIssue277RemoveGraphHashOnlyTest::Request;
	using CortexIssue277RemoveGraphHashOnlyTest::HashOnlyApply;
	using CortexIssue277RemoveGraphHashOnlyTest::HasFunction;
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexIssue277RemoveGraph/BP_HashOnly");
	UBlueprint* Blueprint = CreateFixture(*Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), Blueprint))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	if (!TestTrue(TEXT("fixture function created"), Handler.Execute(TEXT("add_function"), Add).bSuccess))
	{
		Retire(Blueprint);
		return false;
	}

	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request(Path, true));
	if (!TestTrue(TEXT("preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		Retire(Blueprint);
		return false;
	}

	// A missing hash is refused, naming the field and the dry run field it comes from, even when the fingerprint is supplied.
	{
		TSharedPtr<FJsonObject> FingerprintOnly = Request(Path, false);
		FingerprintOnly->SetObjectField(TEXT("expected_fingerprint"), Preview.Data->GetObjectField(TEXT("fingerprint_before")));
		const FCortexCommandResult Refused = Handler.Execute(TEXT("remove_graph"), FingerprintOnly);
		TestEqual(TEXT("a fingerprint without the hash is refused"), Refused.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("... naming expected_validation_hash"), Refused.ErrorMessage.Contains(TEXT("expected_validation_hash")));
		TestTrue(TEXT("... and the dry run's validation_hash field"), Refused.ErrorMessage.Contains(TEXT("validation_hash field")));
		const FCortexCommandResult Bare = Handler.Execute(TEXT("remove_graph"), Request(Path, false));
		TestEqual(TEXT("an apply with no guard at all is refused"), Bare.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("... naming expected_validation_hash"), Bare.ErrorMessage.Contains(TEXT("expected_validation_hash")));
	}
	// A wrong hash alone is refused; a supplied but stale fingerprint is still checked.
	{
		TSharedPtr<FJsonObject> Wrong = HashOnlyApply(Path, Preview);
		Wrong->SetStringField(TEXT("expected_validation_hash"), TEXT("0000"));
		TestEqual(TEXT("a wrong hash alone is refused"),
			Handler.Execute(TEXT("remove_graph"), Wrong).ErrorCode, CortexErrorCodes::StalePrecondition);

		TSharedPtr<FJsonObject> Tampered = HashOnlyApply(Path, Preview);
		TSharedPtr<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
		Fingerprint->Values = Preview.Data->GetObjectField(TEXT("fingerprint_before"))->Values;
		Fingerprint->SetStringField(TEXT("graph_authoring_hash"), TEXT("deadbeef"));
		Tampered->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
		TestEqual(TEXT("a supplied fingerprint that does not match is still refused"),
			Handler.Execute(TEXT("remove_graph"), Tampered).ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("the target survives every refusal"), HasFunction(Blueprint, TEXT("DeleteMe")));
	}
	// Drift after the preview: the hash commits to the fingerprint it was computed over, so a hash-only apply is stale.
	{
		UEdGraph* Graph = Blueprint->UbergraphPages[0];
		UK2Node_CustomEvent* Drift = NewObject<UK2Node_CustomEvent>(Graph);
		Drift->CreateNewGuid();
		Drift->CustomFunctionName = TEXT("DriftAfterPreview");
		Graph->AddNode(Drift, false, false);
		Drift->AllocateDefaultPins();
		const FCortexCommandResult Stale = Handler.Execute(TEXT("remove_graph"), HashOnlyApply(Path, Preview));
		TestFalse(TEXT("a hash-only apply after drift is refused"), Stale.bSuccess);
		TestEqual(TEXT("... with STALE_PRECONDITION"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("the target survives the stale refusal"), HasFunction(Blueprint, TEXT("DeleteMe")));
	}

	const FCortexCommandResult Fresh = Handler.Execute(TEXT("remove_graph"), Request(Path, true));
	if (!TestTrue(TEXT("a fresh preview succeeds"), Fresh.bSuccess) || !Fresh.Data.IsValid())
	{
		Retire(Blueprint);
		return false;
	}
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), HashOnlyApply(Path, Fresh));
	TestTrue(FString::Printf(TEXT("the hash-only apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess);
	TestFalse(TEXT("the function graph is removed"), HasFunction(Blueprint, TEXT("DeleteMe")));
	Retire(Blueprint);
	return true;
}
