#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Engine/Blueprint.h"

namespace
{
	FString CreateSearchTestBlueprint(FCortexBPCommandHandler& Handler, const FString& Suffix)
	{
		const FString Name = FString::Printf(TEXT("BP_SearchTest_%s"), *Suffix);
		const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPSearch_%s"), *Suffix);

		const TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), Name);
		CreateParams->SetStringField(TEXT("path"), Dir);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

		const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		if (!CreateResult.bSuccess || !CreateResult.Data.IsValid())
		{
			return FString();
		}

		FString AssetPath;
		CreateResult.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		return AssetPath;
	}

	void CleanupSearchTestBlueprint(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty())
		{
			return;
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (Blueprint)
		{
			Blueprint->GetOutermost()->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchMissingParamsTest,
	"Cortex.Blueprint.Search.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchMissingParamsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail without required params"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchEmptyQueryTest,
	"Cortex.Blueprint.Search.EmptyQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchEmptyQueryTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/SomeAsset"));
	Params->SetStringField(TEXT("query"), TEXT(""));

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail with empty query"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchInvalidSearchInTest,
	"Cortex.Blueprint.Search.InvalidSearchIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchInvalidSearchInTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/SomeAsset"));
	Params->SetStringField(TEXT("query"), TEXT("test"));
	TArray<TSharedPtr<FJsonValue>> SearchIn;
	SearchIn.Add(MakeShared<FJsonValueString>(TEXT("nodes")));
	Params->SetArrayField(TEXT("search_in"), SearchIn);

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail with invalid search_in"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchNonexistentBlueprintTest,
	"Cortex.Blueprint.Search.Nonexistent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchNonexistentBlueprintTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonexistentBP"));
	Params->SetStringField(TEXT("query"), TEXT("anything"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail for nonexistent blueprint"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchCDOMatchTest,
	"Cortex.Blueprint.Search.CDOMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchCDOMatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateSearchTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	const TSharedPtr<FJsonObject> AddVariableParams = MakeShared<FJsonObject>();
	AddVariableParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("TestMessage"));
	AddVariableParams->SetStringField(TEXT("type"), TEXT("String"));
	AddVariableParams->SetStringField(TEXT("default_value"), TEXT("UniqueSearchTarget_XYZ"));

	const FCortexCommandResult AddVariableResult = Handler.Execute(TEXT("add_variable"), AddVariableParams);
	TestTrue(TEXT("add_variable should succeed"), AddVariableResult.bSuccess);

	const TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult CompileResult = Handler.Execute(TEXT("compile"), CompileParams);
	TestTrue(TEXT("compile should succeed"), CompileResult.bSuccess);

	const TSharedPtr<FJsonObject> SetDefaultsParams = MakeShared<FJsonObject>();
	SetDefaultsParams->SetStringField(TEXT("blueprint_path"), AssetPath);
	SetDefaultsParams->SetBoolField(TEXT("compile"), false);
	SetDefaultsParams->SetBoolField(TEXT("save"), false);
	TSharedPtr<FJsonObject> DefaultsObject = MakeShared<FJsonObject>();
	DefaultsObject->SetStringField(TEXT("TestMessage"), TEXT("UniqueSearchTarget_XYZ"));
	SetDefaultsParams->SetObjectField(TEXT("properties"), DefaultsObject);

	const FCortexCommandResult SetDefaultsResult = Handler.Execute(TEXT("set_class_defaults"), SetDefaultsParams);
	TestTrue(TEXT("set_class_defaults should succeed"), SetDefaultsResult.bSuccess);

	const TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), AssetPath);
	SearchParams->SetStringField(TEXT("query"), TEXT("UniqueSearchTarget"));

	const FCortexCommandResult SearchResult = Handler.Execute(TEXT("search"), SearchParams);
	TestTrue(TEXT("search should succeed"), SearchResult.bSuccess);

	if (SearchResult.Data.IsValid())
	{
		int32 MatchCount = 0;
		SearchResult.Data->TryGetNumberField(TEXT("match_count"), MatchCount);
		TestTrue(TEXT("search should return at least one match"), MatchCount > 0);

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstMatch = (*Matches)[0]->AsObject();
			if (FirstMatch.IsValid())
			{
				TestTrue(TEXT("match value should contain search query"),
					FirstMatch->GetStringField(TEXT("value")).Contains(
						TEXT("UniqueSearchTarget"), ESearchCase::CaseSensitive));
				TestEqual(TEXT("match type should be cdo"),
					FirstMatch->GetStringField(TEXT("type")), TEXT("cdo"));
			}
		}
	}

	CleanupSearchTestBlueprint(AssetPath);
	return true;
}
