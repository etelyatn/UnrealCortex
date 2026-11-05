// blueprint.compile_all compiles every Blueprint of a folder or an explicit list through the single-asset
// compile worker, writes a per-asset report file and answers with totals. Read-only content is skipped with a
// reason, level script Blueprints are never enumerated, and `types` filters on registry tags.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPSnapshotTestSupport.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FCortexCommandResult RunCompileAll(const TSharedPtr<FJsonObject>& Params)
	{
		FCortexBPCommandHandler Handler;
		return Handler.Execute(TEXT("compile_all"), Params);
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	TSharedPtr<FJsonObject> FindAsset(const TSharedPtr<FJsonObject>& Report, const FString& AssetPath)
	{
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (!Report.IsValid() || !Report->TryGetArrayField(TEXT("assets"), Assets))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Assets)
		{
			const TSharedPtr<FJsonObject> Asset = Value->AsObject();
			if (Asset.IsValid() && Asset->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Asset;
			}
		}
		return nullptr;
	}

	int32 Total(const TSharedPtr<FJsonObject>& Data, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Totals = nullptr;
		double Value = -1.0;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("totals"), Totals))
		{
			(*Totals)->TryGetNumberField(Name, Value);
		}
		return static_cast<int32>(Value);
	}

	UBlueprint* MakeCompileAllActor(const FString& Dir, const FString& Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None);
	}

	void RetireUnder(const FString& Root)
	{
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Asset = *It;
			if (Asset && Asset->IsAsset() && Asset->GetOutermost()->GetName().StartsWith(Root))
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				Asset->MarkAsGarbage();
			}
		}
		CollectGarbage(RF_NoFlags);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileAllTest,
	"Cortex.Blueprint.CompileAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCompileAllTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPSnapshotTestSupport;

	const FString Suffix = NewSuffix();
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CompileAll_%s"), *Suffix);

	UBlueprint* Clean = MakeCompileAllActor(Dir, TEXT("BP_Clean"));
	UBlueprint* Broken = MakeCompileAllActor(Dir, TEXT("BP_Broken"));
	if (!TestTrue(TEXT("actor fixtures created"), Clean && Broken))
	{
		RetireUnder(Dir);
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Clean);
	{
		// Two custom events with the same name: a guaranteed compile error (see Cortex.Blueprint.CompileFail).
		UEdGraph* EventGraph = Broken->UbergraphPages.Num() > 0 ? Broken->UbergraphPages[0] : nullptr;
		if (!TestNotNull(TEXT("broken fixture event graph"), EventGraph))
		{
			RetireUnder(Dir);
			return false;
		}
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
			Event->CreateNewGuid();
			Event->CustomFunctionName = TEXT("DuplicateEventName");
			EventGraph->AddNode(Event, false, false);
			Event->AllocateDefaultPins();
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Broken);
	}

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateWidget = MakeShared<FJsonObject>();
	CreateWidget->SetStringField(TEXT("name"), TEXT("WBP_Widget"));
	CreateWidget->SetStringField(TEXT("path"), Dir);
	CreateWidget->SetStringField(TEXT("type"), TEXT("Widget"));
	const FCortexCommandResult WidgetCreated = Handler.Execute(TEXT("create"), CreateWidget);
	if (!TestTrue(TEXT("widget fixture created"), WidgetCreated.bSuccess))
	{
		RetireUnder(Dir);
		return false;
	}
	const FString CleanPath = Clean->GetPathName();
	const FString BrokenPath = Broken->GetPathName();
	const FString WidgetPath = Dir + TEXT("/WBP_Widget.WBP_Widget");

	// path_prefix form: all three are compiled and reported.
	const FString ReportPath = OutputPath(TEXT("compile_all"), Suffix);
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_prefix"), Dir);
		Params->SetStringField(TEXT("out_path"), ReportPath);
		const FCortexCommandResult Result = RunCompileAll(Params);
		if (TestTrue(*FString::Printf(TEXT("compile_all by prefix succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage),
			Result.bSuccess && Result.Data.IsValid()))
		{
			TestFalse(TEXT("response carries no per-asset bodies"), Result.Data->HasField(TEXT("assets")));
			TestTrue(TEXT("response names the report"),
				Result.Data->GetStringField(TEXT("out_path")).EndsWith(FString::Printf(TEXT("compile_all_%s.json"), *Suffix)));
			TestEqual(TEXT("total"), Total(Result.Data, TEXT("total")), 3);
			TestEqual(TEXT("one asset failed to compile"), Total(Result.Data, TEXT("error")), 1);
			TestEqual(TEXT("two assets compiled without errors"),
				Total(Result.Data, TEXT("success")) + Total(Result.Data, TEXT("warning")), 2);
			TestEqual(TEXT("nothing skipped"), Total(Result.Data, TEXT("skipped")), 0);
			TestEqual(TEXT("nothing filtered"), Total(Result.Data, TEXT("excluded_by_type")), 0);

			const TArray<TSharedPtr<FJsonValue>>* Failures = nullptr;
			if (TestTrue(TEXT("first_failures present"), Result.Data->TryGetArrayField(TEXT("first_failures"), Failures)))
			{
				if (TestEqual(TEXT("one failing asset listed"), Failures->Num(), 1))
				{
					const TSharedPtr<FJsonObject> Failure = (*Failures)[0]->AsObject();
					TestEqual(TEXT("failing asset is the broken Blueprint"), Failure->GetStringField(TEXT("asset_path")), BrokenPath);
					TestFalse(TEXT("failure summary carries no diagnostics body"), Failure->HasField(TEXT("diagnostics")));
				}
			}
		}

		const TSharedPtr<FJsonObject> Report = ReadJsonFile(ReportPath);
		if (TestTrue(TEXT("report file parses"), Report.IsValid()))
		{
			const TSharedPtr<FJsonObject> CleanEntry = FindAsset(Report, CleanPath);
			if (TestTrue(TEXT("clean entry"), CleanEntry.IsValid()))
			{
				TestEqual(TEXT("clean status"), CleanEntry->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
				TestEqual(TEXT("clean type"), CleanEntry->GetStringField(TEXT("type")), FString(TEXT("Actor")));
				TestEqual(TEXT("clean error_count"), static_cast<int32>(CleanEntry->GetNumberField(TEXT("error_count"))), 0);
			}
			const TSharedPtr<FJsonObject> BrokenEntry = FindAsset(Report, BrokenPath);
			if (TestTrue(TEXT("broken entry"), BrokenEntry.IsValid()))
			{
				TestEqual(TEXT("broken status"), BrokenEntry->GetStringField(TEXT("compile_status")), FString(TEXT("error")));
				TestTrue(TEXT("broken error_count"), BrokenEntry->GetNumberField(TEXT("error_count")) > 0.0);
				TestTrue(TEXT("broken diagnostics listed"), BrokenEntry->GetArrayField(TEXT("diagnostics")).Num() > 0);
			}
			const TSharedPtr<FJsonObject> WidgetEntry = FindAsset(Report, WidgetPath);
			if (TestTrue(TEXT("widget entry"), WidgetEntry.IsValid()))
			{
				TestEqual(TEXT("widget type"), WidgetEntry->GetStringField(TEXT("type")), FString(TEXT("Widget")));
				const FString Status = WidgetEntry->GetStringField(TEXT("compile_status"));
				TestTrue(TEXT("widget compiled"), Status == TEXT("success") || Status == TEXT("warning"));
			}
			const TSharedPtr<FJsonObject>* Totals = nullptr;
			TestTrue(TEXT("report carries totals"), Report->TryGetObjectField(TEXT("totals"), Totals));
			const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
			if (Report->TryGetArrayField(TEXT("assets"), Assets))
			{
				bool bSorted = true;
				for (int32 Index = 1; Index < Assets->Num(); ++Index)
				{
					bSorted &= (*Assets)[Index - 1]->AsObject()->GetStringField(TEXT("asset_path"))
						.Compare((*Assets)[Index]->AsObject()->GetStringField(TEXT("asset_path")), ESearchCase::CaseSensitive) <= 0;
				}
				TestTrue(TEXT("report assets sorted by path"), bSorted);
			}
		}
	}

	// types filters on registry tags: the widget drops out of the run and the report.
	{
		const FString TypedReport = OutputPath(TEXT("compile_all_typed"), Suffix);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_prefix"), Dir);
		Params->SetStringField(TEXT("out_path"), TypedReport);
		Params->SetArrayField(TEXT("types"), StringValues({TEXT("Actor")}));
		const FCortexCommandResult Result = RunCompileAll(Params);
		if (TestTrue(TEXT("typed compile_all succeeds"), Result.bSuccess && Result.Data.IsValid()))
		{
			TestEqual(TEXT("typed total"), Total(Result.Data, TEXT("total")), 2);
			TestEqual(TEXT("widget excluded by type"), Total(Result.Data, TEXT("excluded_by_type")), 1);
		}
		const TSharedPtr<FJsonObject> Report = ReadJsonFile(TypedReport);
		if (TestTrue(TEXT("typed report parses"), Report.IsValid()))
		{
			TestFalse(TEXT("widget absent from typed report"), FindAsset(Report, WidgetPath).IsValid());
			TestTrue(TEXT("actor present in typed report"), FindAsset(Report, CleanPath).IsValid());
		}
		IFileManager::Get().Delete(*TypedReport);
	}

	// paths form: explicit list, a read-only engine Blueprint is skipped with a reason, an unknown path is not_found.
	{
		const FString EnginePath = TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros");
		const FString MissingPath = Dir + TEXT("/BP_Nope.BP_Nope");
		const FString PathsReport = OutputPath(TEXT("compile_all_paths"), Suffix);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("paths"), StringValues({CleanPath, BrokenPath, EnginePath, MissingPath}));
		Params->SetStringField(TEXT("out_path"), PathsReport);
		const FCortexCommandResult Result = RunCompileAll(Params);
		if (TestTrue(*FString::Printf(TEXT("paths compile_all succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage),
			Result.bSuccess && Result.Data.IsValid()))
		{
			TestEqual(TEXT("paths total"), Total(Result.Data, TEXT("total")), 4);
			TestEqual(TEXT("engine Blueprint skipped"), Total(Result.Data, TEXT("skipped")), 1);
			TestEqual(TEXT("unknown path not_found"), Total(Result.Data, TEXT("not_found")), 1);
			TestEqual(TEXT("broken still fails the asset, not the run"), Total(Result.Data, TEXT("error")), 1);
		}
		const TSharedPtr<FJsonObject> Report = ReadJsonFile(PathsReport);
		if (TestTrue(TEXT("paths report parses"), Report.IsValid()))
		{
			const TSharedPtr<FJsonObject> Skipped = FindAsset(Report, EnginePath);
			if (TestTrue(TEXT("engine entry"), Skipped.IsValid()))
			{
				TestEqual(TEXT("engine status"), Skipped->GetStringField(TEXT("compile_status")), FString(TEXT("skipped")));
				TestFalse(TEXT("engine skip carries a reason"), Skipped->GetStringField(TEXT("reason")).IsEmpty());
			}
			const TSharedPtr<FJsonObject> Missing = FindAsset(Report, MissingPath);
			if (TestTrue(TEXT("missing entry"), Missing.IsValid()))
			{
				TestEqual(TEXT("missing status"), Missing->GetStringField(TEXT("compile_status")), FString(TEXT("not_found")));
			}
		}
		IFileManager::Get().Delete(*PathsReport);
	}

	// A prefix over read-only engine content skips every Blueprint it finds instead of failing the run.
	{
		const FString EngineReport = OutputPath(TEXT("compile_all_engine"), Suffix);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_prefix"), TEXT("/Engine/EditorBlueprintResources"));
		Params->SetStringField(TEXT("out_path"), EngineReport);
		const FCortexCommandResult Result = RunCompileAll(Params);
		if (TestTrue(*FString::Printf(TEXT("engine prefix compile_all succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage),
			Result.bSuccess && Result.Data.IsValid()))
		{
			TestTrue(TEXT("engine prefix found Blueprints"), Total(Result.Data, TEXT("total")) > 0);
			TestEqual(TEXT("every engine Blueprint skipped"), Total(Result.Data, TEXT("skipped")), Total(Result.Data, TEXT("total")));
			TestEqual(TEXT("no engine Blueprint compiled"), Total(Result.Data, TEXT("error")) + Total(Result.Data, TEXT("success"))
				+ Total(Result.Data, TEXT("warning")), 0);
		}
		IFileManager::Get().Delete(*EngineReport);
	}

	// Argument validation.
	{
		TSharedPtr<FJsonObject> Neither = MakeShared<FJsonObject>();
		Neither->SetStringField(TEXT("out_path"), ReportPath);
		const FCortexCommandResult NeitherResult = RunCompileAll(Neither);
		TestFalse(TEXT("neither path_prefix nor paths refused"), NeitherResult.bSuccess);
		TestEqual(TEXT("neither code"), NeitherResult.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Both = MakeShared<FJsonObject>();
		Both->SetStringField(TEXT("path_prefix"), Dir);
		Both->SetArrayField(TEXT("paths"), StringValues({CleanPath}));
		Both->SetStringField(TEXT("out_path"), ReportPath);
		const FCortexCommandResult BothResult = RunCompileAll(Both);
		TestFalse(TEXT("both path_prefix and paths refused"), BothResult.bSuccess);
		TestEqual(TEXT("both code"), BothResult.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> NoOut = MakeShared<FJsonObject>();
		NoOut->SetStringField(TEXT("path_prefix"), Dir);
		const FCortexCommandResult NoOutResult = RunCompileAll(NoOut);
		TestFalse(TEXT("missing out_path refused"), NoOutResult.bSuccess);
		TestEqual(TEXT("missing out_path code"), NoOutResult.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> BadType = MakeShared<FJsonObject>();
		BadType->SetStringField(TEXT("path_prefix"), Dir);
		BadType->SetStringField(TEXT("out_path"), ReportPath);
		BadType->SetArrayField(TEXT("types"), StringValues({TEXT("Spaceship")}));
		const FCortexCommandResult BadTypeResult = RunCompileAll(BadType);
		TestFalse(TEXT("unknown type refused"), BadTypeResult.bSuccess);
		TestEqual(TEXT("unknown type code"), BadTypeResult.ErrorCode, CortexErrorCodes::InvalidField);

		const FString Outside = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Source"), FString::Printf(TEXT("CortexCompileAllOutside_%s.json"), *Suffix)));
		TSharedPtr<FJsonObject> OutsideParams = MakeShared<FJsonObject>();
		OutsideParams->SetStringField(TEXT("path_prefix"), Dir);
		OutsideParams->SetStringField(TEXT("out_path"), Outside);
		const FCortexCommandResult OutsideResult = RunCompileAll(OutsideParams);
		TestFalse(TEXT("out_path outside allowed roots refused"), OutsideResult.bSuccess);
		TestEqual(TEXT("outside code"), OutsideResult.ErrorCode, CortexErrorCodes::InvalidFilePath);
		TestFalse(TEXT("nothing written outside allowed roots"), IFileManager::Get().FileExists(*Outside));
	}

	IFileManager::Get().Delete(*ReportPath);
	RetireUnder(Dir);
	IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), FString::Printf(TEXT("CompileAll_%s"), *Suffix)), false, true);
	return true;
}
