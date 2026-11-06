// #122 data.verify_type_replacement: redirects must be active (never a vacuous pass), load/compile/resave, the normalized baseline diff
// never masks a real difference, remaining referencers are reported.

#include "Misc/AutomationTest.h"

#include "CortexDataTypeReplacementTestSupport.h"
#include "CortexDataTypeReplacementTestTypes.h"
#include "CortexAssetMutationGuard.h"
#include "CortexTypes.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"

namespace CortexIssue122DataTest
{
	namespace Verify
	{
		/** A saved UDS with a DataTable (three rows) and a Blueprint (variable + default), planned with a baseline, then unloaded. */
		struct FStructSetup
		{
			FFixture Fixture;
			UUserDefinedStruct* Uds = nullptr;
			FString UserPath;
			FString NativePath;
			FString TablePath;
			FString BlueprintPath;
			FString PlanPath;
			FString TableFile;
			TArray<FString> BaselineFiles;
			TArray<TPair<FString, FString>> Members;
			TArray<FString> Lines;
			TMap<FName, FRowValues> Baseline;
			FRowValues PayloadValues{ TEXT("5"), TEXT("hello"), TEXT("True") };
		};

		bool PrepareStruct(FAutomationTestBase& Test, FStructSetup& Setup)
		{
			Setup.Uds = Setup.Fixture.MakeRowStruct(TEXT("UDS_Row"));
			if (!Test.TestNotNull(TEXT("UDS"), Setup.Uds))
			{
				return false;
			}
			UDataTable* Table = Setup.Fixture.MakeTable(TEXT("DT_Rows"), Setup.Uds);
			if (!Test.TestNotNull(TEXT("Table"), Table)
				|| !Test.TestTrue(TEXT("Rows added"),
					FFixture::AddRow(Table, TEXT("R1"), FRowMembers(), { TEXT("1"), TEXT("one"), TEXT("True") })
						&& FFixture::AddRow(Table, TEXT("R2"), FRowMembers(), { TEXT("2"), TEXT("two"), TEXT("False") })
						&& FFixture::AddRow(Table, TEXT("R3"), FRowMembers(), { TEXT("3"), TEXT("three"), TEXT("True") })))
			{
				return false;
			}
			FStructOnScope Payload(Setup.Uds);
			SetMember(Setup.Uds, Payload.GetStructMemory(), TEXT("Count"), Setup.PayloadValues.Count);
			SetMember(Setup.Uds, Payload.GetStructMemory(), TEXT("Label"), Setup.PayloadValues.Label);
			SetMember(Setup.Uds, Payload.GetStructMemory(), TEXT("Flag"), Setup.PayloadValues.Flag);
			FString PayloadText;
			Setup.Uds->ExportText(PayloadText, Payload.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
			UBlueprint* Blueprint = Setup.Fixture.MakeBlueprintWithVariable(TEXT("BP_Holder"), TEXT("Payload"), StructPin(Setup.Uds), PayloadText);
			if (!Test.TestNotNull(TEXT("Blueprint"), Blueprint)
				|| !Test.TestTrue(TEXT("Saved"), Setup.Fixture.SaveAsset(Setup.Uds) && Setup.Fixture.SaveAsset(Table) && Setup.Fixture.SaveAsset(Blueprint)))
			{
				return false;
			}
			Setup.Fixture.RegisterSavedFiles();

			Setup.UserPath = Setup.Uds->GetPathName();
			Setup.NativePath = FCortexIssue122Row::StaticStruct()->GetPathName();
			Setup.TablePath = Table->GetPathName();
			Setup.BlueprintPath = Blueprint->GetPathName();
			Setup.Baseline = ReadRows(Table, FRowMembers());
			FString TableFile;
			FPackageName::TryConvertLongPackageNameToFilename(Table->GetOutermost()->GetName(), TableFile, FPackageName::GetAssetPackageExtension());
			Setup.TableFile = TableFile;
			Setup.Members = {
				{ UserName(Setup.Uds, TEXT("Count")), TEXT("Count") },
				{ UserName(Setup.Uds, TEXT("Label")), TEXT("Label") },
				{ UserName(Setup.Uds, TEXT("Flag")), TEXT("bFlag") },
			};

			TSharedPtr<FJsonObject> Params = PlanParams(Setup.UserPath, Setup.NativePath);
			SetStringMap(Params, TEXT("member_map"), { { TEXT("Flag"), TEXT("bFlag") } });
			Params->SetStringField(TEXT("baseline_dir"), Setup.Fixture.ScratchPath(TEXT("baseline")));
			Setup.PlanPath = Setup.Fixture.ScratchPath(TEXT("plan.json"));
			Params->SetStringField(TEXT("out_path"), Setup.PlanPath);
			const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
			if (!Test.TestTrue(TEXT("plan succeeds"), Plan.bSuccess))
			{
				Test.AddError(Plan.ErrorMessage);
				return false;
			}
			Setup.Lines = Strings(Plan.Data, TEXT("redirects"));
			for (const TSharedPtr<FJsonObject>& Entry : Objects(Plan.Data, TEXT("baseline")))
			{
				Setup.BaselineFiles.Add(Entry->GetStringField(TEXT("file")));
			}
			return Test.TestEqual(TEXT("Five redirect lines"), Setup.Lines.Num(), 5)
				&& Test.TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table, Blueprint }));
		}

		TSharedPtr<FJsonObject> VerifyParams(const FString& PlanPath, const bool bSave)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("plan_path"), PlanPath);
			Params->SetBoolField(TEXT("save"), bSave);
			return Params;
		}

		TSharedPtr<FJsonObject> AssetEntry(const FCortexCommandResult& Result, const FString& AssetPath)
		{
			for (const TSharedPtr<FJsonObject>& Entry : Objects(Result.Data, TEXT("assets")))
			{
				if (Entry->GetStringField(TEXT("asset_path")) == AssetPath)
				{
					return Entry;
				}
			}
			return nullptr;
		}

		bool SameSet(const TArray<FString>& A, const TArray<FString>& B)
		{
			TArray<FString> SortedA = A;
			TArray<FString> SortedB = B;
			SortedA.Sort();
			SortedB.Sort();
			return SortedA == SortedB;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyRedirectsNotActiveTest,
	"Cortex.Data.TypeReplacement.VerifyRedirectsNotActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyRedirectsNotActiveTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup))
	{
		return false;
	}
	const FString TablePackage = FPackageName::ObjectPathToPackageName(Setup.TablePath);
	const FString BlueprintPackage = FPackageName::ObjectPathToPackageName(Setup.BlueprintPath);
	const FString TableHash = FileBytesHash(Setup.TableFile);

	// No redirects: refused, listing every missing line; nothing was loaded and nothing saved.
	{
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, /*bSave=*/true));
		TestFalse(TEXT("Without redirects verification is refused, never a vacuous pass"), Result.bSuccess);
		TestEqual(TEXT("REDIRECTS_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		if (Result.ErrorDetails.IsValid())
		{
			TestTrue(TEXT("All five lines are listed as missing"), SameSet(Strings(Result.ErrorDetails, TEXT("missing_redirects")), Setup.Lines));
			TestTrue(TEXT("The restart instruction is part of the refusal"), Result.ErrorDetails->GetStringField(TEXT("instruction")).Contains(TEXT("editor_restart")));
		}
		TestTrue(TEXT("The DataTable was not loaded"), FindPackage(nullptr, *TablePackage) == nullptr);
		TestTrue(TEXT("The Blueprint was not loaded"), FindPackage(nullptr, *BlueprintPackage) == nullptr);
		TestTrue(TEXT("The DataTable file was not saved"), FileBytesHash(Setup.TableFile) == TableHash);
	}

	// Each probe has to pass: the Struct redirect alone missing (a Blueprint CDO default would be dropped) ...
	{
		TArray<FCoreRedirect> Partial;
		for (const FCoreRedirect& Redirect : MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members))
		{
			if (Redirect.RedirectFlags != ECoreRedirectFlags::Type_Struct)
			{
				Partial.Add(Redirect);
			}
		}
		FRedirectScope Scope(Partial);
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false));
		TestEqual(TEXT("Missing StructRedirects: REDIRECTS_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		TestTrue(TEXT("... naming only the StructRedirects line"), Result.ErrorDetails.IsValid() && SameSet(Strings(Result.ErrorDetails, TEXT("missing_redirects")), { Setup.Lines[1] }));
	}
	// ... the Object redirect alone missing (the UDS import would not change) ...
	{
		TArray<FCoreRedirect> Partial;
		for (const FCoreRedirect& Redirect : MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members))
		{
			if (Redirect.RedirectFlags != ECoreRedirectFlags::Type_Object)
			{
				Partial.Add(Redirect);
			}
		}
		FRedirectScope Scope(Partial);
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false));
		TestEqual(TEXT("Missing ObjectRedirects: REDIRECTS_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		TestTrue(TEXT("... naming only the ObjectRedirects line"), Result.ErrorDetails.IsValid() && SameSet(Strings(Result.ErrorDetails, TEXT("missing_redirects")), { Setup.Lines[0] }));
	}
	// ... and the property redirects missing (every member would be dropped).
	{
		TArray<FCoreRedirect> Partial;
		for (const FCoreRedirect& Redirect : MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members))
		{
			if (Redirect.RedirectFlags != ECoreRedirectFlags::Type_Property)
			{
				Partial.Add(Redirect);
			}
		}
		FRedirectScope Scope(Partial);
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false));
		TestEqual(TEXT("Missing PropertyRedirects: REDIRECTS_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		TestTrue(TEXT("... naming the three property lines"), Result.ErrorDetails.IsValid() && SameSet(Strings(Result.ErrorDetails, TEXT("missing_redirects")), { Setup.Lines[2], Setup.Lines[3], Setup.Lines[4] }));
	}
	TestTrue(TEXT("Still nothing loaded"), FindPackage(nullptr, *TablePackage) == nullptr && FindPackage(nullptr, *BlueprintPackage) == nullptr);

	// An enum redirect that lacks its ValueChanges is not active either.
	{
		UUserDefinedEnum* Ude = Setup.Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
		if (TestNotNull(TEXT("UDE"), Ude))
		{
			TSharedPtr<FJsonObject> EnumParams = PlanParams(Ude->GetPathName(), StaticEnum<ECortexIssue122Mode>()->GetPathName());
			EnumParams->SetStringField(TEXT("out_path"), Setup.Fixture.ScratchPath(TEXT("enum.plan.json")));
			const FCortexCommandResult EnumPlan = Run(TEXT("plan_type_replacement"), EnumParams);
			if (TestTrue(TEXT("enum plan"), EnumPlan.bSuccess))
			{
				FRedirectScope Scope({ MakeRedirect(ECoreRedirectFlags::Type_Enum, Ude->GetPathName(), StaticEnum<ECortexIssue122Mode>()->GetPathName(), TEXT("/Script/CoreUObject.Enum")) });
				const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.Fixture.ScratchPath(TEXT("enum.plan.json")), false));
				TestEqual(TEXT("An enum redirect without its ValueChanges is not active"), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
				TestTrue(TEXT("... the enum line is listed"), Result.ErrorDetails.IsValid() && SameSet(Strings(Result.ErrorDetails, TEXT("missing_redirects")), Strings(EnumPlan.Data, TEXT("redirects"))));
			}
		}
	}

	// Request and plan integrity.
	{
		TSharedPtr<FJsonObject> NoSave = MakeShared<FJsonObject>();
		NoSave->SetStringField(TEXT("plan_path"), Setup.PlanPath);
		TestFalse(TEXT("save is required (the save is always an explicit choice)"), Run(TEXT("verify_type_replacement"), NoSave).bSuccess);
		TestEqual(TEXT("A missing plan file is reported"), Run(TEXT("verify_type_replacement"), VerifyParams(Setup.Fixture.ScratchPath(TEXT("nope.plan.json")), false)).ErrorCode, CortexErrorCodes::FileNotFound);

		FString PlanText = ReadFile(Setup.PlanPath);
		FFileHelper::SaveStringToFile(PlanText.Replace(TEXT("\"bFlag\""), TEXT("\"Flag\"")), *FFixture::Absolute(Setup.PlanPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestEqual(TEXT("A plan edited after it was generated is stale"), Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false)).ErrorCode, CortexErrorCodes::StalePrecondition);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyReportAliasTest,
	"Cortex.Data.TypeReplacement.VerifyReportAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyReportAliasTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup) || !TestEqual(TEXT("One baseline file"), Setup.BaselineFiles.Num(), 1))
	{
		return false;
	}
	const FString PlanHash = FileBytesHash(FFixture::Absolute(Setup.PlanPath));
	const FString BaselineHash = FileBytesHash(FFixture::Absolute(Setup.BaselineFiles[0]));
	const TArray<FString> Aliases = {
		Setup.PlanPath,
		FFixture::Absolute(Setup.PlanPath),
		FPaths::Combine(FPaths::GetPath(Setup.PlanPath), TEXT("."), FPaths::GetCleanFilename(Setup.PlanPath)),
		Setup.BaselineFiles[0],
		FFixture::Absolute(Setup.BaselineFiles[0]),
	};
	for (const FString& Alias : Aliases)
	{
		TSharedPtr<FJsonObject> Params2 = VerifyParams(Setup.PlanPath, true);
		Params2->SetStringField(TEXT("out_path"), Alias);
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), Params2);
		TestFalse(*FString::Printf(TEXT("out_path %s aliasing an input is refused"), *Alias), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("... as INVALID_FILE_PATH (%s)"), *Alias), Result.ErrorCode, CortexErrorCodes::InvalidFilePath);
		TestTrue(TEXT("... the plan bytes are unchanged"), FileBytesHash(FFixture::Absolute(Setup.PlanPath)) == PlanHash);
		TestTrue(TEXT("... the baseline bytes are unchanged"), FileBytesHash(FFixture::Absolute(Setup.BaselineFiles[0])) == BaselineHash);
	}
	TestTrue(TEXT("Nothing was loaded"), FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Setup.TablePath)) == nullptr && FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Setup.BlueprintPath)) == nullptr);

	// A distinct report path is fine and receives the full per-asset result.
	{
		FRedirectScope Scope(MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members), /*bAlreadyRegistered=*/true);
		if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Setup.Lines)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Params2 = VerifyParams(Setup.PlanPath, false);
		const FString ReportPath = Setup.Fixture.ScratchPath(TEXT("verify.report.json"));
		Params2->SetStringField(TEXT("out_path"), ReportPath);
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), Params2);
		TestTrue(TEXT("A distinct out_path succeeds"), Result.bSuccess);
		TestTrue(TEXT("... and the report exists"), IFileManager::Get().FileExists(*FFixture::Absolute(ReportPath)));
		TestTrue(TEXT("... the plan is untouched"), FileBytesHash(FFixture::Absolute(Setup.PlanPath)) == PlanHash);
		TestTrue(TEXT("Unloaded again"), FFixture::Unload({ FFixture::Reload<UDataTable>(Setup.TablePath), FFixture::Reload<UBlueprint>(Setup.BlueprintPath) }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyBlockedTest,
	"Cortex.Data.TypeReplacement.VerifyBlocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyBlockedTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup))
	{
		return false;
	}
	// The Blueprint is quarantined after an unverified recovery (a process-lifetime block on this unique fixture path).
	UBlueprint* Blueprint = FFixture::Reload<UBlueprint>(Setup.BlueprintPath);
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	FCortexAssetMutationGuard::Block(Blueprint, TEXT("issue 122 test quarantine"));
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Blueprint })))
	{
		return false;
	}

	FRedirectScope Scope(MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members), /*bAlreadyRegistered=*/true);
	if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Setup.Lines)))
	{
		return false;
	}
	for (const bool bSave : { false, true })
	{
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, bSave));
		TestFalse(*FString::Printf(TEXT("A blocked Blueprint is refused (save:%s)"), bSave ? TEXT("true") : TEXT("false")), Result.bSuccess);
		TestEqual(TEXT("... as INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("... naming the Blueprint"), Result.ErrorDetails.IsValid() && Strings(Result.ErrorDetails, TEXT("blocked_assets")).Num() == 1);
		TestTrue(TEXT("The Blueprint was neither loaded nor compiled"), FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Setup.BlueprintPath)) == nullptr);
		TestTrue(TEXT("The DataTable was not loaded either"), FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Setup.TablePath)) == nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyTest,
	"Cortex.Data.TypeReplacement.Verify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup))
	{
		return false;
	}
	const FString UdsFile = [&Setup]()
	{
		FString File;
		FPackageName::TryConvertLongPackageNameToFilename(Setup.Uds->GetOutermost()->GetName(), File, FPackageName::GetAssetPackageExtension());
		return File;
	}();
	const FString TablePackage = FPackageName::ObjectPathToPackageName(Setup.TablePath);
	const FString BlueprintPackage = FPackageName::ObjectPathToPackageName(Setup.BlueprintPath);

	{
		// The operator's path: the plan's lines through the engine's ini reader, then a "restart" (the assets are unloaded).
		FRedirectScope Scope(MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members), /*bAlreadyRegistered=*/true);
		if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Setup.Lines)))
		{
			return false;
		}

		// 1. save:false verifies in memory: redirects probed, assets loaded, the Blueprint compiled, the normalized baseline equals a fresh export.
		const FCortexCommandResult Memory = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false));
		if (!TestTrue(TEXT("verify (save:false) succeeds"), Memory.bSuccess))
		{
			AddError(Memory.ErrorMessage);
			return false;
		}
		TestTrue(TEXT("redirects_active"), Memory.Data->GetBoolField(TEXT("redirects_active")));
		TestTrue(TEXT("The normalized baseline equals the fresh export (zero differences)"), Memory.Data->GetBoolField(TEXT("equal")));
		TestTrue(TEXT("baseline_compared"), Memory.Data->GetBoolField(TEXT("baseline_compared")));
		TestEqual(TEXT("No problems"), static_cast<int32>(Memory.Data->GetNumberField(TEXT("problem_count"))), 0);
		TestEqual(TEXT("Nothing saved"), static_cast<int32>(Memory.Data->GetNumberField(TEXT("saved_count"))), 0);
		const TSharedPtr<FJsonObject> Table = AssetEntry(Memory, Setup.TablePath);
		const TSharedPtr<FJsonObject> Blueprint = AssetEntry(Memory, Setup.BlueprintPath);
		if (TestTrue(TEXT("Both assets are reported"), Table.IsValid() && Blueprint.IsValid()))
		{
			TestTrue(TEXT("DataTable converted to the native struct"), Table->GetBoolField(TEXT("converted")));
			TestEqual(TEXT("rows_changed 0"), static_cast<int32>(Table->GetNumberField(TEXT("rows_changed"))), 0);
			TestEqual(TEXT("rows_added 0"), static_cast<int32>(Table->GetNumberField(TEXT("rows_added"))), 0);
			TestEqual(TEXT("rows_removed 0"), static_cast<int32>(Table->GetNumberField(TEXT("rows_removed"))), 0);
			TestTrue(TEXT("DataTable equal"), Table->GetBoolField(TEXT("equal")));
			const FString Status = Blueprint->GetStringField(TEXT("compile_status"));
			TestTrue(TEXT("The Blueprint compiles (status up to date)"), Status == TEXT("up_to_date") || Status == TEXT("up_to_date_with_warnings"));
			TestTrue(TEXT("The Blueprint variable no longer uses the user struct"), Blueprint->GetBoolField(TEXT("converted")));
		}
		TestEqual(TEXT("The disk still references the user struct, so both assets are still_referenced"), static_cast<int32>(Memory.Data->GetNumberField(TEXT("still_referenced_count"))), 2);
		const UPackage* TablePkg = FindPackage(nullptr, *TablePackage);
		const UPackage* BlueprintPkg = FindPackage(nullptr, *BlueprintPackage);
		TestTrue(TEXT("In-memory verification leaves the editor state as found (not dirty)"), TablePkg != nullptr && !TablePkg->IsDirty() && BlueprintPkg != nullptr && !BlueprintPkg->IsDirty());

		// 2. A row edited after the baseline is reported with its key and its native field name: the normalization maps the raw
		//    GUID-suffixed keys but never masks a real difference.
		UDataTable* Loaded = FFixture::Reload<UDataTable>(Setup.TablePath);
		FCortexIssue122Row* Row = Loaded != nullptr ? Loaded->FindRow<FCortexIssue122Row>(TEXT("R2"), TEXT("Issue122")) : nullptr;
		if (!TestNotNull(TEXT("Loaded native row R2"), Row))
		{
			return false;
		}
		Row->Label = TEXT("edited");
		Loaded->GetOutermost()->MarkPackageDirty();
		const FCortexCommandResult Edited = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, false));
		if (TestTrue(TEXT("verify after the edit succeeds"), Edited.bSuccess))
		{
			TestFalse(TEXT("equal is false"), Edited.Data->GetBoolField(TEXT("equal")));
			const TSharedPtr<FJsonObject> EditedTable = AssetEntry(Edited, Setup.TablePath);
			if (TestTrue(TEXT("DataTable reported"), EditedTable.IsValid()))
			{
				TestEqual(TEXT("rows_changed is exactly 1"), static_cast<int32>(EditedTable->GetNumberField(TEXT("rows_changed"))), 1);
				TestFalse(TEXT("The table is not equal"), EditedTable->GetBoolField(TEXT("equal")));
				const TArray<TSharedPtr<FJsonObject>> Changed = Objects(EditedTable, TEXT("changed"));
				if (TestEqual(TEXT("One changed sample"), Changed.Num(), 1))
				{
					TestEqual(TEXT("... the changed key is R2"), Changed[0]->GetStringField(TEXT("key")), FString(TEXT("R2")));
					const TArray<FString> Fields = Strings(Changed[0], TEXT("fields"));
					TestTrue(TEXT("... the changed field is the native Label (the GUID-suffixed key was mapped through member_map)"), Fields.Num() == 1 && Fields[0] == TEXT("Label"));
				}
			}
		}

		// 3. save:true needs clean packages: the edited table has unsaved changes.
		const FString TableHash = FileBytesHash(Setup.TableFile);
		const FCortexCommandResult Dirty = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, true));
		TestFalse(TEXT("save:true over unsaved changes is refused"), Dirty.bSuccess);
		TestEqual(TEXT("DIRTY_EDITOR_STATE"), Dirty.ErrorCode, CortexErrorCodes::DirtyEditorState);
		TestTrue(TEXT("... naming the DataTable"), Dirty.ErrorDetails.IsValid() && Strings(Dirty.ErrorDetails, TEXT("dirty_assets")).Contains(Setup.TablePath));
		TestTrue(TEXT("... and nothing was saved"), FileBytesHash(Setup.TableFile) == TableHash);

		// 4. With the edit reverted, save:true resaves explicitly: the user struct disappears from the saved packages.
		Row->Label = TEXT("two");
		Loaded->GetOutermost()->ClearDirtyFlag();
		const FCortexCommandResult Saved = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, true));
		if (TestTrue(TEXT("verify (save:true) succeeds"), Saved.bSuccess))
		{
			TestEqual(TEXT("Both assets saved"), static_cast<int32>(Saved.Data->GetNumberField(TEXT("saved_count"))), 2);
			TestTrue(TEXT("equal after the resave"), Saved.Data->GetBoolField(TEXT("equal")));
			TestEqual(TEXT("Nothing references the user struct any more"), static_cast<int32>(Saved.Data->GetNumberField(TEXT("still_referenced_count"))), 0);
			TestFalse(TEXT("The user struct is never deleted by verify"), Saved.Data->GetBoolField(TEXT("user_type_deleted")));
		}
		TestTrue(TEXT("The DataTable file was rewritten"), FileBytesHash(Setup.TableFile) != TableHash);
		TestTrue(TEXT("The user struct file is still there"), IFileManager::Get().FileExists(*UdsFile));
		TestTrue(TEXT("... and the object is alive"), IsValid(Setup.Uds));
		TestTrue(TEXT("Unloaded again"), FFixture::Unload({ FFixture::Reload<UDataTable>(Setup.TablePath), FFixture::Reload<UBlueprint>(Setup.BlueprintPath) }));
	}

	// 5. A second reload without any redirect reads identical rows: the resave wrote the native struct and property names.
	UDataTable* Reloaded = FFixture::Reload<UDataTable>(Setup.TablePath);
	UBlueprint* ReloadedBlueprint = FFixture::Reload<UBlueprint>(Setup.BlueprintPath);
	if (TestNotNull(TEXT("Reloaded DataTable"), Reloaded) && TestNotNull(TEXT("Reloaded Blueprint"), ReloadedBlueprint))
	{
		TestTrue(TEXT("The resaved DataTable uses the native struct without any redirect"), Reloaded->GetRowStruct() == FCortexIssue122Row::StaticStruct());
		const TMap<FName, FRowValues> Rows = ReadRows(Reloaded, NativeMembers());
		TestEqual(TEXT("Three rows"), Rows.Num(), 3);
		for (const TPair<FName, FRowValues>& Pair : Setup.Baseline)
		{
			const FRowValues* Row = Rows.Find(Pair.Key);
			TestTrue(*FString::Printf(TEXT("Row %s is identical"), *Pair.Key.ToString()), Row != nullptr && *Row == Pair.Value);
		}
		const FStructProperty* Payload = ReloadedBlueprint->GeneratedClass ? FindFProperty<FStructProperty>(ReloadedBlueprint->GeneratedClass, TEXT("Payload")) : nullptr;
		if (TestNotNull(TEXT("Payload property"), Payload))
		{
			TestTrue(TEXT("The Blueprint variable is the native struct"), Payload->Struct == FCortexIssue122Row::StaticStruct());
			const FRowValues Cdo = ReadRow(Payload->Struct, Payload->ContainerPtrToValuePtr<uint8>(ReloadedBlueprint->GeneratedClass->GetDefaultObject()), NativeMembers());
			TestTrue(TEXT("The Blueprint default survived"), Cdo == Setup.PayloadValues);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyEnumTest,
	"Cortex.Data.TypeReplacement.VerifyEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyEnumTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FFixture Fixture;
	UUserDefinedEnum* Ude = Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
	if (!TestNotNull(TEXT("UDE"), Ude))
	{
		return false;
	}
	UEnum* Native = StaticEnum<ECortexIssue122Mode>();
	UBlueprint* Blueprint = Fixture.MakeBlueprintWithVariable(TEXT("BP_Holder"), TEXT("Mode"), EnumPin(Ude), Ude->GetNameStringByIndex(1));
	if (!TestNotNull(TEXT("Blueprint"), Blueprint) || !TestTrue(TEXT("Saved"), Fixture.SaveAsset(Ude) && Fixture.SaveAsset(Blueprint)))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();
	const FString UserPath = Ude->GetPathName();
	const FString BlueprintPath = Blueprint->GetPathName();

	TSharedPtr<FJsonObject> Params = PlanParams(UserPath, Native->GetPathName());
	const FString PlanPath = Fixture.ScratchPath(TEXT("enum.plan.json"));
	Params->SetStringField(TEXT("out_path"), PlanPath);
	const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
	if (!TestTrue(TEXT("plan succeeds"), Plan.bSuccess) || !TestTrue(TEXT("Unloaded"), FFixture::Unload({ Blueprint })))
	{
		return false;
	}

	TMap<FString, FString> Changes;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Changes.Add(Ude->GetNameStringByIndex(Index), Native->GetNameStringByIndex(Index));
	}
	{
		FRedirectScope Scope({ MakeEnumRedirect(UserPath, Native->GetPathName(), Changes) }, /*bAlreadyRegistered=*/true);
		if (!TestTrue(TEXT("The plan's line is registered through the ini reader"), RegisterLinesThroughIni(Strings(Plan.Data, TEXT("redirects")))))
		{
			return false;
		}
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
		if (TestTrue(TEXT("verify succeeds"), Result.bSuccess))
		{
			TestTrue(TEXT("redirects_active"), Result.Data->GetBoolField(TEXT("redirects_active")));
			TestFalse(TEXT("Without a baseline the report says so"), Result.Data->GetBoolField(TEXT("baseline_compared")));
			TestTrue(TEXT("... equal is null, not a pass"), Result.Data->HasTypedField<EJson::Null>(TEXT("equal")));
			TestTrue(TEXT("... with a note"), Result.Data->GetStringField(TEXT("note")).Contains(TEXT("no baseline")));
			const TSharedPtr<FJsonObject> Entry = AssetEntry(Result, BlueprintPath);
			if (TestTrue(TEXT("The Blueprint is reported"), Entry.IsValid()))
			{
				TestTrue(TEXT("The Blueprint variable is the native enum now"), Entry->GetBoolField(TEXT("converted")));
				const FString Status = Entry->GetStringField(TEXT("compile_status"));
				TestTrue(TEXT("It compiles"), Status == TEXT("up_to_date") || Status == TEXT("up_to_date_with_warnings"));
			}
			TestEqual(TEXT("The disk still references the user enum"), static_cast<int32>(Result.Data->GetNumberField(TEXT("still_referenced_count"))), 1);
		}
		if (UBlueprint* Loaded = FFixture::Reload<UBlueprint>(BlueprintPath))
		{
			TestTrue(TEXT("Unloaded again"), FFixture::Unload({ Loaded }));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyNestedTest,
	"Cortex.Data.TypeReplacement.VerifyNested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyNestedTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FFixture Fixture;
	UUserDefinedStruct* Inner = Fixture.MakeStruct(TEXT("UDS_InnerValue"), { { TEXT("Value"), IntPin() } });
	UUserDefinedStruct* Outer = Fixture.MakeStruct(TEXT("UDS_Outer"), { { TEXT("Count"), IntPin() }, { TEXT("Inner"), StructPin(Inner) } });
	if (!TestNotNull(TEXT("Inner"), Inner) || !TestNotNull(TEXT("Outer"), Outer))
	{
		return false;
	}
	const FString InnerGuid = UserName(Inner, TEXT("Value"));
	const FString NestedText = FString::Printf(TEXT("(%s=7)"), *InnerGuid);

	UDataTable* Table = Fixture.MakeTable(TEXT("DT_Outer"), Outer);
	if (!TestNotNull(TEXT("Table"), Table))
	{
		return false;
	}
	{
		FStructOnScope Row(Outer);
		if (!TestTrue(TEXT("Row values"), SetMember(Outer, Row.GetStructMemory(), TEXT("Count"), TEXT("4")) && SetMember(Outer, Row.GetStructMemory(), TEXT("Inner"), NestedText)))
		{
			return false;
		}
		Table->AddRow(TEXT("R1"), Row.GetStructMemory(), Outer);
	}
	FStructOnScope Payload(Outer);
	SetMember(Outer, Payload.GetStructMemory(), TEXT("Count"), TEXT("9"));
	SetMember(Outer, Payload.GetStructMemory(), TEXT("Inner"), NestedText);
	FString PayloadText;
	Outer->ExportText(PayloadText, Payload.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
	UBlueprint* Blueprint = Fixture.MakeBlueprintWithVariable(TEXT("BP_Outer"), TEXT("Payload"), StructPin(Outer), PayloadText);
	if (!TestNotNull(TEXT("Blueprint"), Blueprint) || !TestTrue(TEXT("Saved"), Fixture.SaveAsset(Inner) && Fixture.SaveAsset(Outer) && Fixture.SaveAsset(Table) && Fixture.SaveAsset(Blueprint)))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString OuterPath = Outer->GetPathName();
	const FString WrapperPath = FCortexIssue122WrapperRow::StaticStruct()->GetPathName();
	const FString InnerPath = Inner->GetPathName();
	const FString InnerNative = FCortexIssue122Inner::StaticStruct()->GetPathName();
	const TArray<TPair<FString, FString>> InnerMembers = { { InnerGuid, TEXT("Value") } };
	const TArray<TPair<FString, FString>> OuterMembers = { { UserName(Outer, TEXT("Count")), TEXT("Count") }, { UserName(Outer, TEXT("Inner")), TEXT("Inner") } };
	const FString TablePath = Table->GetPathName();
	const FString BlueprintPath = Blueprint->GetPathName();
	const FString PlanPath = Fixture.ScratchPath(TEXT("outer.plan.json"));

	// Plan the outer struct while the inner struct's complete replacement is active.
	TArray<FString> Lines;
	{
		FRedirectScope InnerFull(MakeStructRedirects(InnerPath, InnerNative, InnerMembers));
		TSharedPtr<FJsonObject> Params = PlanParams(OuterPath, WrapperPath);
		Params->SetStringField(TEXT("baseline_dir"), Fixture.ScratchPath(TEXT("baseline")));
		Params->SetStringField(TEXT("out_path"), PlanPath);
		const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
		if (!TestTrue(TEXT("The outer plan succeeds"), Plan.bSuccess))
		{
			AddError(Plan.ErrorMessage);
			return false;
		}
		TestEqual(TEXT("One prerequisite"), Objects(Plan.Data, TEXT("prerequisites")).Num(), 1);
		Lines = Strings(Plan.Data, TEXT("redirects"));
	}
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table, Blueprint })))
	{
		return false;
	}

	// The outer redirects are active (the operator's path); the inner ones come and go.
	FRedirectScope OuterScope(MakeStructRedirects(OuterPath, WrapperPath, OuterMembers), /*bAlreadyRegistered=*/true);
	if (!TestTrue(TEXT("The outer lines are registered through the ini reader"), RegisterLinesThroughIni(Lines)))
	{
		return false;
	}
	const FString TablePackage = FPackageName::ObjectPathToPackageName(TablePath);
	const auto ExpectRefused = [&](const TCHAR* Label)
	{
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
		TestFalse(*FString::Printf(TEXT("%s: refused"), Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s: REDIRECTS_NOT_ACTIVE"), Label), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		TestTrue(*FString::Printf(TEXT("%s: the inner prerequisite is named"), Label), Result.ErrorDetails.IsValid() && Strings(Result.ErrorDetails, TEXT("missing_prerequisites")).Num() > 0);
		TestTrue(*FString::Printf(TEXT("%s: nothing was loaded"), Label), FindPackage(nullptr, *TablePackage) == nullptr);
	};
	const auto WithoutFlag = [&](const ECoreRedirectFlags Flag)
	{
		TArray<FCoreRedirect> Kept;
		for (const FCoreRedirect& Redirect : MakeStructRedirects(InnerPath, InnerNative, InnerMembers))
		{
			if (Redirect.RedirectFlags != Flag)
			{
				Kept.Add(Redirect);
			}
		}
		return Kept;
	};
	ExpectRefused(TEXT("No inner redirects"));
	{
		FRedirectScope ObjectOnly({ MakeRedirect(ECoreRedirectFlags::Type_Object, InnerPath, InnerNative, TEXT("/Script/CoreUObject.ScriptStruct")) });
		ExpectRefused(TEXT("Object-only inner redirect"));
	}
	{
		FRedirectScope Scope(WithoutFlag(ECoreRedirectFlags::Type_Struct));
		ExpectRefused(TEXT("Missing inner StructRedirects"));
	}
	{
		FRedirectScope Scope(WithoutFlag(ECoreRedirectFlags::Type_Property));
		ExpectRefused(TEXT("Missing inner GUID-member PropertyRedirects"));
	}

	// The full inner contract: the non-default nested payload is preserved through verify and a reload.
	{
		FRedirectScope InnerFull(MakeStructRedirects(InnerPath, InnerNative, InnerMembers));
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
		if (TestTrue(TEXT("verify succeeds with the complete inner contract"), Result.bSuccess))
		{
			TestTrue(TEXT("The normalized baseline (outer and inner GUID names) equals the fresh export"), Result.Data->GetBoolField(TEXT("equal")));
			TestEqual(TEXT("No problems"), static_cast<int32>(Result.Data->GetNumberField(TEXT("problem_count"))), 0);
		}
		else
		{
			AddError(Result.ErrorMessage);
		}
		UDataTable* Reloaded = FFixture::Reload<UDataTable>(TablePath);
		UBlueprint* ReloadedBlueprint = FFixture::Reload<UBlueprint>(BlueprintPath);
		if (TestNotNull(TEXT("Reloaded table"), Reloaded) && TestNotNull(TEXT("Reloaded blueprint"), ReloadedBlueprint))
		{
			TestTrue(TEXT("The table uses the native wrapper row"), Reloaded->GetRowStruct() == FCortexIssue122WrapperRow::StaticStruct());
			const FCortexIssue122WrapperRow* Row = Reloaded->FindRow<FCortexIssue122WrapperRow>(TEXT("R1"), TEXT("Issue122"));
			if (TestNotNull(TEXT("Row R1"), Row))
			{
				TestEqual(TEXT("Count"), Row->Count, 4);
				TestEqual(TEXT("The nested payload survived"), Row->Inner.Value, 7);
			}
			const FStructProperty* Property = ReloadedBlueprint->GeneratedClass ? FindFProperty<FStructProperty>(ReloadedBlueprint->GeneratedClass, TEXT("Payload")) : nullptr;
			if (TestNotNull(TEXT("Payload property"), Property) && TestTrue(TEXT("native wrapper"), Property->Struct == FCortexIssue122WrapperRow::StaticStruct()))
			{
				const FCortexIssue122WrapperRow* Cdo = Property->ContainerPtrToValuePtr<FCortexIssue122WrapperRow>(ReloadedBlueprint->GeneratedClass->GetDefaultObject());
				TestEqual(TEXT("The Blueprint's nested default survived"), Cdo->Inner.Value, 7);
				TestEqual(TEXT("... and the outer one"), Cdo->Count, 9);
			}
			TestTrue(TEXT("Unloaded again"), FFixture::Unload({ Reloaded, ReloadedBlueprint }));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyTypedEnumTest,
	"Cortex.Data.TypeReplacement.VerifyTypedEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyTypedEnumTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FFixture Fixture;
	UUserDefinedEnum* Ude = Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
	UEnum* Native = StaticEnum<ECortexIssue122Mode>();
	UDataTable* Table = Fixture.MakeTable(TEXT("DT_EnumRef"), FCortexIssue122EnumRefRow::StaticStruct());
	if (!TestNotNull(TEXT("UDE"), Ude) || !TestNotNull(TEXT("Table"), Table))
	{
		return false;
	}
	// A row named like an enumerator, an unrelated string equal to a token, and an enum-typed field. The table hard-references the UDE.
	const auto AddEnumRow = [&](const TCHAR* Name, const ECortexIssue122Mode Mode, const TCHAR* Note)
	{
		FCortexIssue122EnumRefRow Row;
		Row.EnumRef = Ude;
		Row.Mode = Mode;
		Row.Note = Note;
		Table->AddRow(Name, Row);
	};
	AddEnumRow(*Ude->GetNameStringByIndex(0), ECortexIssue122Mode::A, *Ude->GetNameStringByIndex(0));
	AddEnumRow(TEXT("R2"), ECortexIssue122Mode::B, TEXT("plain"));
	if (!TestTrue(TEXT("Saved"), Fixture.SaveAsset(Ude) && Fixture.SaveAsset(Table)))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString UserPath = Ude->GetPathName();
	const FString TablePath = Table->GetPathName();
	TSharedPtr<FJsonObject> Params = PlanParams(UserPath, Native->GetPathName());
	Params->SetStringField(TEXT("baseline_dir"), Fixture.ScratchPath(TEXT("baseline")));
	const FString PlanPath = Fixture.ScratchPath(TEXT("enum.plan.json"));
	Params->SetStringField(TEXT("out_path"), PlanPath);
	const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
	if (!TestTrue(TEXT("plan succeeds"), Plan.bSuccess))
	{
		AddError(Plan.ErrorMessage);
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> BaselineEntries = Objects(Plan.Data, TEXT("baseline"));
	if (!TestEqual(TEXT("A baseline for the table"), BaselineEntries.Num(), 1))
	{
		return false;
	}

	// The baseline of a real UDE-typed field holds tokens: simulate it by rewriting the exported Mode values to the old tokens.
	{
		const FString BaselineFile = BaselineEntries[0]->GetStringField(TEXT("file"));
		TSharedPtr<FJsonObject> Baseline;
		if (!TestTrue(TEXT("The baseline parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadFile(BaselineFile)), Baseline) && Baseline.IsValid()))
		{
			return false;
		}
		int32 Rewritten = 0;
		for (const TSharedPtr<FJsonValue>& RowValue : Baseline->GetArrayField(TEXT("rows")))
		{
			const TSharedPtr<FJsonObject> Data = RowValue->AsObject()->GetObjectField(TEXT("row_data"));
			const FString Name = Data->GetStringField(TEXT("Mode")) == TEXT("A") ? Ude->GetNameStringByIndex(0) : Ude->GetNameStringByIndex(1);
			Data->SetStringField(TEXT("Mode"), Name);
			++Rewritten;
		}
		FString Text;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Baseline.ToSharedRef(), Writer);
		TestTrue(TEXT("The baseline is rewritten"), Rewritten == 2 && FFileHelper::SaveStringToFile(Text, *FFixture::Absolute(BaselineFile), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table })))
	{
		return false;
	}

	TMap<FString, FString> Changes;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Changes.Add(Ude->GetNameStringByIndex(Index), Native->GetNameStringByIndex(Index));
	}
	FRedirectScope Scope({ MakeEnumRedirect(UserPath, Native->GetPathName(), Changes) }, /*bAlreadyRegistered=*/true);
	if (!TestTrue(TEXT("The plan's line is registered through the ini reader"), RegisterLinesThroughIni(Strings(Plan.Data, TEXT("redirects")))))
	{
		return false;
	}

	// Untouched data: the enum field is mapped by its TYPE, the string equal to a token and the row named like a token are left alone.
	const FCortexCommandResult Equal = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
	if (!TestTrue(TEXT("verify succeeds"), Equal.bSuccess))
	{
		AddError(Equal.ErrorMessage);
		return false;
	}
	const TSharedPtr<FJsonObject> EqualEntry = AssetEntry(Equal, TablePath);
	if (TestTrue(TEXT("The table is reported"), EqualEntry.IsValid()))
	{
		TestEqual(TEXT("No false difference (rows_changed)"), static_cast<int32>(EqualEntry->GetNumberField(TEXT("rows_changed"))), 0);
		TestEqual(TEXT("The row named like a token keeps its identity (rows_added)"), static_cast<int32>(EqualEntry->GetNumberField(TEXT("rows_added"))), 0);
		TestEqual(TEXT("... (rows_removed)"), static_cast<int32>(EqualEntry->GetNumberField(TEXT("rows_removed"))), 0);
		TestTrue(TEXT("equal"), EqualEntry->GetBoolField(TEXT("equal")));
	}

	// A real edit of the unrelated string (to the very name the token maps to) is reported, not masked.
	UDataTable* Loaded = FFixture::Reload<UDataTable>(TablePath);
	FCortexIssue122EnumRefRow* Row = Loaded != nullptr ? Loaded->FindRow<FCortexIssue122EnumRefRow>(*Ude->GetNameStringByIndex(0), TEXT("Issue122")) : nullptr;
	if (!TestNotNull(TEXT("The row named like the token"), Row))
	{
		return false;
	}
	Row->Note = TEXT("A");
	Loaded->GetOutermost()->MarkPackageDirty();
	const FCortexCommandResult Edited = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
	if (TestTrue(TEXT("verify after the edit succeeds"), Edited.bSuccess))
	{
		const TSharedPtr<FJsonObject> EditedEntry = AssetEntry(Edited, TablePath);
		if (TestTrue(TEXT("The table is reported"), EditedEntry.IsValid()))
		{
			TestEqual(TEXT("The edit is reported"), static_cast<int32>(EditedEntry->GetNumberField(TEXT("rows_changed"))), 1);
			const TArray<TSharedPtr<FJsonObject>> Changed = Objects(EditedEntry, TEXT("changed"));
			if (TestEqual(TEXT("One changed sample"), Changed.Num(), 1))
			{
				TestEqual(TEXT("... on the row named like the token"), Changed[0]->GetStringField(TEXT("key")), Ude->GetNameStringByIndex(0));
				const TArray<FString> Fields = Strings(Changed[0], TEXT("fields"));
				TestTrue(TEXT("... in the Note field only"), Fields.Num() == 1 && Fields[0] == TEXT("Note"));
			}
		}
	}
	Row->Note = *Ude->GetNameStringByIndex(0);
	Loaded->GetOutermost()->ClearDirtyFlag();
	TestTrue(TEXT("Unloaded again"), FFixture::Unload({ Loaded }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyReferenceTableTest,
	"Cortex.Data.TypeReplacement.VerifyReferenceTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyReferenceTableTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FFixture Fixture;
	UUserDefinedStruct* Uds = Fixture.MakeRowStruct(TEXT("UDS_Row"));
	UDataTable* Table = Fixture.MakeTable(TEXT("DT_Refs"), FCortexIssue122StructRefRow::StaticStruct());
	if (!TestNotNull(TEXT("UDS"), Uds) || !TestNotNull(TEXT("Table"), Table))
	{
		return false;
	}
	FCortexIssue122StructRefRow Row;
	Row.Struct = Uds;
	Row.Note = TEXT("ref");
	Table->AddRow(TEXT("R1"), Row);
	if (!TestTrue(TEXT("Saved"), Fixture.SaveAsset(Uds) && Fixture.SaveAsset(Table)))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString UserPath = Uds->GetPathName();
	const FString NativePath = FCortexIssue122Row::StaticStruct()->GetPathName();
	const FString TablePath = Table->GetPathName();
	TSharedPtr<FJsonObject> Params = PlanParams(UserPath, NativePath);
	SetStringMap(Params, TEXT("member_map"), { { TEXT("Flag"), TEXT("bFlag") } });
	Params->SetStringField(TEXT("baseline_dir"), Fixture.ScratchPath(TEXT("baseline")));
	const FString PlanPath = Fixture.ScratchPath(TEXT("plan.json"));
	Params->SetStringField(TEXT("out_path"), PlanPath);
	const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
	if (!TestTrue(TEXT("plan succeeds"), Plan.bSuccess))
	{
		AddError(Plan.ErrorMessage);
		return false;
	}
	bool bFound = false;
	for (const TSharedPtr<FJsonObject>& Asset : Objects(Plan.Data, TEXT("affected_assets")))
	{
		if (Asset->GetStringField(TEXT("path")) == TablePath)
		{
			bFound = true;
			TestEqual(TEXT("The table is a datatable"), Asset->GetStringField(TEXT("kind")), FString(TEXT("datatable")));
			TestFalse(TEXT("row_struct is false: the user struct is only referenced from row data"), Asset->GetBoolField(TEXT("row_struct")));
		}
	}
	if (!TestTrue(TEXT("The table is an affected asset"), bFound) || !TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table })))
	{
		return false;
	}

	const TArray<TPair<FString, FString>> Members = { { UserName(Uds, TEXT("Count")), TEXT("Count") }, { UserName(Uds, TEXT("Label")), TEXT("Label") }, { UserName(Uds, TEXT("Flag")), TEXT("bFlag") } };
	{
		FRedirectScope Scope(MakeStructRedirects(UserPath, NativePath, Members), /*bAlreadyRegistered=*/true);
		if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Strings(Plan.Data, TEXT("redirects")))))
		{
			return false;
		}
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, true));
		if (TestTrue(TEXT("verify (save:true) succeeds although the row struct is not the replaced struct"), Result.bSuccess))
		{
			TestEqual(TEXT("No problems"), static_cast<int32>(Result.Data->GetNumberField(TEXT("problem_count"))), 0);
			TestEqual(TEXT("The table was resaved"), static_cast<int32>(Result.Data->GetNumberField(TEXT("saved_count"))), 1);
			TestTrue(TEXT("The reference moved to the native struct: equal"), Result.Data->GetBoolField(TEXT("equal")));
			TestEqual(TEXT("Nothing references the user struct any more"), static_cast<int32>(Result.Data->GetNumberField(TEXT("still_referenced_count"))), 0);
		}
		else
		{
			AddError(Result.ErrorMessage);
		}
		UDataTable* Loaded = FFixture::Reload<UDataTable>(TablePath);
		if (TestNotNull(TEXT("Reloaded"), Loaded))
		{
			TestTrue(TEXT("The row struct is unchanged"), Loaded->GetRowStruct() == FCortexIssue122StructRefRow::StaticStruct());
			TestTrue(TEXT("Unloaded again"), FFixture::Unload({ Loaded }));
		}
	}
	// A reload without redirects reads the native reference the resave wrote.
	UDataTable* Reloaded = FFixture::Reload<UDataTable>(TablePath);
	if (TestNotNull(TEXT("Reloaded without redirects"), Reloaded))
	{
		const FCortexIssue122StructRefRow* Saved = Reloaded->FindRow<FCortexIssue122StructRefRow>(TEXT("R1"), TEXT("Issue122"));
		if (TestNotNull(TEXT("Row"), Saved))
		{
			TestTrue(TEXT("The saved reference is the native struct"), Saved->Struct == FCortexIssue122Row::StaticStruct());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyChainTest,
	"Cortex.Data.TypeReplacement.VerifyChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyChainTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	// outer -> middle -> leaf (UDS), the leaf holding a UDE-typed member: the leaf and the enum are two levels below the planned struct.
	FFixture Fixture;
	UUserDefinedEnum* Ude = Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
	if (!TestNotNull(TEXT("UDE"), Ude))
	{
		return false;
	}
	UUserDefinedStruct* Leaf = Fixture.MakeStruct(TEXT("UDS_Leaf"), { { TEXT("Value"), IntPin() }, { TEXT("Mode"), EnumPin(Ude) } });
	UUserDefinedStruct* Middle = TestNotNull(TEXT("Leaf"), Leaf) ? Fixture.MakeStruct(TEXT("UDS_Middle"), { { TEXT("Count"), IntPin() }, { TEXT("Leaf"), StructPin(Leaf) } }) : nullptr;
	UUserDefinedStruct* Chain = TestNotNull(TEXT("Middle"), Middle) ? Fixture.MakeStruct(TEXT("UDS_Chain"), { { TEXT("Count"), IntPin() }, { TEXT("Middle"), StructPin(Middle) } }) : nullptr;
	if (!TestNotNull(TEXT("Chain"), Chain))
	{
		return false;
	}
	const FString LeafValue = UserName(Leaf, TEXT("Value"));
	const FString LeafMode = UserName(Leaf, TEXT("Mode"));
	const FString MiddleCount = UserName(Middle, TEXT("Count"));
	const FString MiddleLeaf = UserName(Middle, TEXT("Leaf"));
	const FString ModeToken = Ude->GetNameStringByIndex(1);
	const FString MiddleText = FString::Printf(TEXT("(%s=2,%s=(%s=3,%s=%s))"), *MiddleCount, *MiddleLeaf, *LeafValue, *LeafMode, *ModeToken);

	UDataTable* Table = Fixture.MakeTable(TEXT("DT_Chain"), Chain);
	if (!TestNotNull(TEXT("Table"), Table))
	{
		return false;
	}
	{
		FStructOnScope Row(Chain);
		if (!TestTrue(TEXT("Row values"), SetMember(Chain, Row.GetStructMemory(), TEXT("Count"), TEXT("1")) && SetMember(Chain, Row.GetStructMemory(), TEXT("Middle"), MiddleText)))
		{
			return false;
		}
		Table->AddRow(TEXT("R1"), Row.GetStructMemory(), Chain);
	}
	if (!TestTrue(TEXT("Saved"), Fixture.SaveAsset(Ude) && Fixture.SaveAsset(Leaf) && Fixture.SaveAsset(Middle) && Fixture.SaveAsset(Chain) && Fixture.SaveAsset(Table)))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString ChainPath = Chain->GetPathName();
	const FString ChainNative = FCortexIssue122ChainRow::StaticStruct()->GetPathName();
	const FString MiddlePath = Middle->GetPathName();
	const FString MiddleNative = FCortexIssue122Middle::StaticStruct()->GetPathName();
	const FString LeafPath = Leaf->GetPathName();
	const FString LeafNative = FCortexIssue122Leaf::StaticStruct()->GetPathName();
	UEnum* NativeEnum = StaticEnum<ECortexIssue122Mode>();
	const FString TablePath = Table->GetPathName();
	const TArray<TPair<FString, FString>> LeafMembers = { { LeafValue, TEXT("Value") }, { LeafMode, TEXT("Mode") } };
	const TArray<TPair<FString, FString>> MiddleMembers = { { MiddleCount, TEXT("Count") }, { MiddleLeaf, TEXT("Leaf") } };
	const TArray<TPair<FString, FString>> ChainMembers = { { UserName(Chain, TEXT("Count")), TEXT("Count") }, { UserName(Chain, TEXT("Middle")), TEXT("Middle") } };
	TMap<FString, FString> EnumChanges;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		EnumChanges.Add(Ude->GetNameStringByIndex(Index), NativeEnum->GetNameStringByIndex(Index));
	}
	const auto LeafRedirects = [&]() { return MakeStructRedirects(LeafPath, LeafNative, LeafMembers); };
	const auto MiddleRedirects = [&]() { return MakeStructRedirects(MiddlePath, MiddleNative, MiddleMembers); };
	const auto EnumRedirect = [&]() { return MakeEnumRedirect(Ude->GetPathName(), NativeEnum->GetPathName(), EnumChanges); };
	const FString PlanPath = Fixture.ScratchPath(TEXT("chain.plan.json"));
	const auto PlanChain = [&]() -> FCortexCommandResult
	{
		TSharedPtr<FJsonObject> Params = PlanParams(ChainPath, ChainNative);
		Params->SetStringField(TEXT("baseline_dir"), Fixture.ScratchPath(TEXT("baseline")));
		Params->SetStringField(TEXT("out_path"), PlanPath);
		return Run(TEXT("plan_type_replacement"), Params);
	};
	const auto ExpectPlanRefused = [&](const TCHAR* Label)
	{
		const FCortexCommandResult Result = PlanChain();
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(*FString::Printf(TEXT("%s: refused"), Label), Result.bSuccess);
		TestTrue(*FString::Printf(TEXT("%s: as nested_user_type on Middle"), Label), ProblemCodes(Result, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("nested_user_type"), TEXT("Middle")));
	};

	// Planning needs the whole chain: the middle's contract alone, or the middle and leaf without the leaf's enum, is not enough.
	{
		FRedirectScope MiddleOnly(MiddleRedirects());
		ExpectPlanRefused(TEXT("Middle only (leaf and enum missing)"));
	}
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope LeafScope(LeafRedirects());
		ExpectPlanRefused(TEXT("Middle and leaf without the leaf's enum"));
	}
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope EnumScope({ EnumRedirect() });
		ExpectPlanRefused(TEXT("Middle and enum without the leaf"));
	}
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope LeafScope(LeafRedirects());
		FRedirectScope EnumScope({ EnumRedirect() });
		const FCortexCommandResult Plan = PlanChain();
		if (!TestTrue(TEXT("The plan succeeds with the whole chain active"), Plan.bSuccess))
		{
			AddError(Plan.ErrorMessage);
			return false;
		}
		TArray<FString> Recorded;
		for (const TSharedPtr<FJsonObject>& Prerequisite : Objects(Plan.Data, TEXT("prerequisites")))
		{
			Recorded.Add(Prerequisite->GetStringField(TEXT("user_type")));
		}
		TestEqual(TEXT("Middle, leaf and the enum are all recorded"), Recorded.Num(), 3);
		TestTrue(TEXT("... middle"), Recorded.Contains(MiddlePath));
		TestTrue(TEXT("... leaf"), Recorded.Contains(LeafPath));
		TestTrue(TEXT("... enum"), Recorded.Contains(Ude->GetPathName()));
	}
	TArray<FString> Lines;
	{
		const FCortexCommandResult Plan = [&]()
		{
			FRedirectScope Scope(MiddleRedirects());
			FRedirectScope LeafScope(LeafRedirects());
			FRedirectScope EnumScope({ EnumRedirect() });
			return PlanChain();
		}();
		if (!TestTrue(TEXT("The plan is written"), Plan.bSuccess))
		{
			return false;
		}
		Lines = Strings(Plan.Data, TEXT("redirects"));
	}
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table })))
	{
		return false;
	}

	FRedirectScope OuterScope(MakeStructRedirects(ChainPath, ChainNative, ChainMembers), /*bAlreadyRegistered=*/true);
	if (!TestTrue(TEXT("The outer lines are registered through the ini reader"), RegisterLinesThroughIni(Lines)))
	{
		return false;
	}
	const FString TablePackage = FPackageName::ObjectPathToPackageName(TablePath);
	const auto ExpectVerifyRefused = [&](const TCHAR* Label)
	{
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
		TestFalse(*FString::Printf(TEXT("%s: refused"), Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s: REDIRECTS_NOT_ACTIVE"), Label), Result.ErrorCode, CortexErrorCodes::RedirectsNotActive);
		TestTrue(*FString::Printf(TEXT("%s: a prerequisite is named"), Label), Result.ErrorDetails.IsValid() && Strings(Result.ErrorDetails, TEXT("missing_prerequisites")).Num() > 0);
		TestTrue(*FString::Printf(TEXT("%s: nothing was loaded"), Label), FindPackage(nullptr, *TablePackage) == nullptr);
	};
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope EnumScope({ EnumRedirect() });
		ExpectVerifyRefused(TEXT("Leaf redirects missing (two levels below the plan)"));
	}
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope LeafScope(LeafRedirects());
		ExpectVerifyRefused(TEXT("Leaf's enum redirect missing"));
	}

	// Everything active: the nested non-default payload survives, and the leaf's enum value is normalized through the recorded inner mapping.
	{
		FRedirectScope Scope(MiddleRedirects());
		FRedirectScope LeafScope(LeafRedirects());
		FRedirectScope EnumScope({ EnumRedirect() });
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(PlanPath, false));
		if (TestTrue(TEXT("verify succeeds with the whole chain active"), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Entry = AssetEntry(Result, TablePath);
			if (TestTrue(TEXT("The table is reported"), Entry.IsValid()))
			{
				TestEqual(TEXT("No false difference from the inner enum (rows_changed)"), static_cast<int32>(Entry->GetNumberField(TEXT("rows_changed"))), 0);
				TestTrue(TEXT("The table is equal"), Entry->GetBoolField(TEXT("equal")));
			}
			TestTrue(TEXT("equal"), Result.Data->GetBoolField(TEXT("equal")));
		}
		else
		{
			AddError(Result.ErrorMessage);
		}
		UDataTable* Reloaded = FFixture::Reload<UDataTable>(TablePath);
		if (TestNotNull(TEXT("Reloaded table"), Reloaded))
		{
			const FCortexIssue122ChainRow* Row = Reloaded->FindRow<FCortexIssue122ChainRow>(TEXT("R1"), TEXT("Issue122"));
			if (TestNotNull(TEXT("Row R1"), Row))
			{
				TestEqual(TEXT("Count"), Row->Count, 1);
				TestEqual(TEXT("Middle.Count"), Row->Middle.Count, 2);
				TestEqual(TEXT("Middle.Leaf.Value"), Row->Middle.Leaf.Value, 3);
				TestTrue(TEXT("Middle.Leaf.Mode"), Row->Middle.Leaf.Mode == ECortexIssue122Mode::B);
			}
			TestTrue(TEXT("Unloaded again"), FFixture::Unload({ Reloaded }));
		}
	}
	return true;
}

namespace CortexIssue122DataTest
{
	namespace Verify
	{
		UEdGraphPin* FindInputByType(UEdGraphNode* Node, const FName Category)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == Category)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		/** BeginPlay -> Set members (Flag shown, non-default true) -> Print; Payload -> Break; Break.Label -> Print.InString. */
		struct FNodeGraph
		{
			UK2Node_SetFieldsInStruct* Set = nullptr;
			UK2Node_BreakStruct* Break = nullptr;
			UK2Node_CallFunction* Print = nullptr;
		};

		bool BuildNodeGraph(FAutomationTestBase& Test, UBlueprint* Blueprint, UUserDefinedStruct* Uds, FNodeGraph& Out)
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			int32 PosY = 0;
			UK2Node_Event* Begin = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, FName(TEXT("ReceiveBeginPlay")), AActor::StaticClass(), PosY);
			FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Graph);
			UK2Node_VariableGet* Get = GetCreator.CreateNode();
			Get->VariableReference.SetSelfMember(TEXT("Payload"));
			GetCreator.Finalize();
			FGraphNodeCreator<UK2Node_SetFieldsInStruct> SetCreator(*Graph);
			Out.Set = SetCreator.CreateNode();
			Out.Set->StructType = Uds;
			SetCreator.Finalize();
			const FName FlagName(*UserName(Uds, TEXT("Flag")));
			for (FOptionalPinFromProperty& Entry : Out.Set->ShowPinForProperties)
			{
				if (Entry.PropertyName == FlagName)
				{
					Entry.bShowPin = true;
				}
			}
			Out.Set->ReconstructNode();
			FGraphNodeCreator<UK2Node_BreakStruct> BreakCreator(*Graph);
			Out.Break = BreakCreator.CreateNode();
			Out.Break->StructType = Uds;
			BreakCreator.Finalize();
			FGraphNodeCreator<UK2Node_CallFunction> PrintCreator(*Graph);
			Out.Print = PrintCreator.CreateNode();
			Out.Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
			PrintCreator.Finalize();

			UEdGraphPin* FlagPin = Out.Set->FindPin(FlagName, EGPD_Input);
			UEdGraphPin* GetOut = Get->GetValuePin();
			UEdGraphPin* LabelOut = Out.Break->FindPin(FName(*UserName(Uds, TEXT("Label"))), EGPD_Output);
			UEdGraphPin* PrintString = Out.Print->FindPin(TEXT("InString"), EGPD_Input);
			UEdGraphPin* SetRef = FindInputByType(Out.Set, UEdGraphSchema_K2::PC_Struct);
			UEdGraphPin* BreakIn = FindInputByType(Out.Break, UEdGraphSchema_K2::PC_Struct);
			if (!Test.TestTrue(TEXT("Node pins exist"), Begin && FlagPin && GetOut && LabelOut && PrintString && SetRef && BreakIn))
			{
				return false;
			}
			Schema->TrySetDefaultValue(*FlagPin, TEXT("true"));
			return Test.TestTrue(TEXT("Links made"),
				Schema->TryCreateConnection(Begin->FindPin(UEdGraphSchema_K2::PN_Then), Out.Set->FindPin(UEdGraphSchema_K2::PN_Execute))
				&& Schema->TryCreateConnection(Out.Set->FindPin(UEdGraphSchema_K2::PN_Then), Out.Print->FindPin(UEdGraphSchema_K2::PN_Execute))
				&& Schema->TryCreateConnection(GetOut, SetRef) && Schema->TryCreateConnection(GetOut, BreakIn) && Schema->TryCreateConnection(LabelOut, PrintString));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyStructNodesTest,
	"Cortex.Data.TypeReplacement.VerifyStructNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyStructNodesTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup))
	{
		return false;
	}
	// Loading the Blueprint under the redirects logs the engine's own warning about the pin it could not carry over: the stale state this repairs.
	AddExpectedMessagePlain(TEXT("specifying non-default value no longer exists on node"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	// The graph nodes of the Blueprint (the plan only needs its variable): compiled clean before the swap.
	UBlueprint* Blueprint = FFixture::Reload<UBlueprint>(Setup.BlueprintPath);
	FNodeGraph Nodes;
	if (!TestNotNull(TEXT("Blueprint"), Blueprint) || !BuildNodeGraph(*this, Blueprint, Setup.Uds, Nodes))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("The Blueprint compiles before the swap"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings)
		|| !TestTrue(TEXT("Saved"), Setup.Fixture.SaveAsset(Blueprint)) || !TestTrue(TEXT("Unloaded"), FFixture::Unload({ Blueprint })))
	{
		return false;
	}
	{
		FRedirectScope Scope(MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members), /*bAlreadyRegistered=*/true);
		if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Setup.Lines)))
		{
			return false;
		}
		const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, true));
		if (!TestTrue(TEXT("verify (save:true) succeeds: the stale struct-node pins are repaired, the Blueprint compiles"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		const TSharedPtr<FJsonObject> Entry = AssetEntry(Result, Setup.BlueprintPath);
		if (TestTrue(TEXT("The Blueprint is reported"), Entry.IsValid()))
		{
			TestTrue(TEXT("ok"), Entry->GetBoolField(TEXT("ok")));
			TestTrue(TEXT("pins_repaired is recorded and positive"), Entry->GetNumberField(TEXT("pins_repaired")) >= 1);
			const FString Status = Entry->GetStringField(TEXT("compile_status"));
			TestTrue(TEXT("compile succeeded"), Status == TEXT("up_to_date") || Status == TEXT("up_to_date_with_warnings"));
			TestTrue(TEXT("saved"), Entry->GetBoolField(TEXT("saved")));
		}
		TestEqual(TEXT("No problems"), static_cast<int32>(Result.Data->GetNumberField(TEXT("problem_count"))), 0);
		TestTrue(TEXT("Unloaded again"), FFixture::Unload({ FFixture::Reload<UDataTable>(Setup.TablePath), FFixture::Reload<UBlueprint>(Setup.BlueprintPath) }));
	}

	// Without any redirect: values and links survived the resave, nothing is orphaned.
	UBlueprint* Reloaded = FFixture::Reload<UBlueprint>(Setup.BlueprintPath);
	if (!TestNotNull(TEXT("Reloaded Blueprint"), Reloaded))
	{
		return false;
	}
	UK2Node_SetFieldsInStruct* Set = nullptr;
	UK2Node_BreakStruct* Break = nullptr;
	for (UEdGraphNode* Node : FBlueprintEditorUtils::FindEventGraph(Reloaded)->Nodes)
	{
		if (UK2Node_SetFieldsInStruct* Candidate = Cast<UK2Node_SetFieldsInStruct>(Node))
		{
			Set = Candidate;
		}
		else if (UK2Node_BreakStruct* BreakCandidate = Cast<UK2Node_BreakStruct>(Node))
		{
			Break = BreakCandidate;
		}
	}
	if (TestNotNull(TEXT("Set members node"), Set) && TestNotNull(TEXT("Break node"), Break))
	{
		TestTrue(TEXT("Set members is typed with the native struct"), Set->StructType == FCortexIssue122Row::StaticStruct());
		const UEdGraphPin* Flag = Set->FindPin(TEXT("bFlag"), EGPD_Input);
		if (TestNotNull(TEXT("The native bFlag pin exists"), Flag))
		{
			TestEqual(TEXT("The non-default value survived"), Flag->DefaultValue, FString(TEXT("true")));
			TestFalse(TEXT("... on a live pin"), Flag->bOrphanedPin);
		}
		const UEdGraphPin* Label = Break->FindPin(TEXT("Label"), EGPD_Output);
		if (TestNotNull(TEXT("The Break node's Label pin"), Label))
		{
			TestEqual(TEXT("Its link to the Print node survived"), Label->LinkedTo.Num(), 1);
		}
		for (const UEdGraphPin* Pin : Set->Pins)
		{
			TestFalse(*FString::Printf(TEXT("Set members pin %s is not orphaned"), *Pin->PinName.ToString()), Pin->bOrphanedPin);
		}
		for (const UEdGraphPin* Pin : Break->Pins)
		{
			TestFalse(*FString::Printf(TEXT("Break pin %s is not orphaned"), *Pin->PinName.ToString()), Pin->bOrphanedPin);
		}
	}
	FKismetEditorUtilities::CompileBlueprint(Reloaded);
	TestTrue(TEXT("The reloaded Blueprint compiles"), Reloaded->Status == BS_UpToDate || Reloaded->Status == BS_UpToDateWithWarnings);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122VerifyStaleUnmappedPinTest,
	"Cortex.Data.TypeReplacement.VerifyStaleUnmappedPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122VerifyStaleUnmappedPinTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;
	using namespace CortexIssue122DataTest::Verify;

	FStructSetup Setup;
	if (!PrepareStruct(*this, Setup))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("specifying non-default value no longer exists on node"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("no longer exists on node"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("Failed to find a struct member for"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0);
	UBlueprint* Blueprint = FFixture::Reload<UBlueprint>(Setup.BlueprintPath);
	FNodeGraph Nodes;
	if (!TestNotNull(TEXT("Blueprint"), Blueprint) || !BuildNodeGraph(*this, Blueprint, Setup.Uds, Nodes))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	// A stale pin no plan member explains: a linked orphan on the Break node.
	UEdGraphPin* Ghost = Nodes.Break->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("Ghost_9_00000000000000000000000000000000"));
	Ghost->bOrphanedPin = true;
	Ghost->SetSavePinIfOrphaned(true);
	FGraphNodeCreator<UK2Node_CallFunction> SecondPrint(*FBlueprintEditorUtils::FindEventGraph(Blueprint));
	UK2Node_CallFunction* Print2 = SecondPrint.CreateNode();
	Print2->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
	SecondPrint.Finalize();
	Ghost->MakeLinkTo(Print2->FindPin(TEXT("InString"), EGPD_Input));
	const FString BreakGuid = Nodes.Break->NodeGuid.ToString();
	if (!TestTrue(TEXT("Saved"), Setup.Fixture.SaveAsset(Blueprint)) || !TestTrue(TEXT("Unloaded"), FFixture::Unload({ Blueprint })))
	{
		return false;
	}
	const FString BlueprintFile = [&Setup]()
	{
		FString File;
		FPackageName::TryConvertLongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Setup.BlueprintPath), File, FPackageName::GetAssetPackageExtension());
		return File;
	}();
	const FString BlueprintHash = FileBytesHash(BlueprintFile);

	FRedirectScope Scope(MakeStructRedirects(Setup.UserPath, Setup.NativePath, Setup.Members), /*bAlreadyRegistered=*/true);
	if (!TestTrue(TEXT("The plan's lines are registered through the ini reader"), RegisterLinesThroughIni(Setup.Lines)))
	{
		return false;
	}
	const FCortexCommandResult Result = Run(TEXT("verify_type_replacement"), VerifyParams(Setup.PlanPath, true));
	if (!TestTrue(TEXT("verify runs"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	const TSharedPtr<FJsonObject> Entry = AssetEntry(Result, Setup.BlueprintPath);
	if (TestTrue(TEXT("The Blueprint is reported"), Entry.IsValid()))
	{
		TestFalse(TEXT("The Blueprint is not ok (fail closed)"), Entry->GetBoolField(TEXT("ok")));
		TestFalse(TEXT("... and not saved"), Entry->GetBoolField(TEXT("saved")));
		const TArray<FString> Unmapped = Strings(Entry, TEXT("unmapped_pins"));
		TestTrue(TEXT("... the unmapped pin is listed with its node GUID"), Unmapped.Num() == 1 && Unmapped[0].Contains(TEXT("Ghost_9_")) && Unmapped[0].Contains(BreakGuid));
		TestFalse(TEXT("... no compile was run"), Entry->HasField(TEXT("compile_status")));
	}
	TestTrue(TEXT("The Blueprint file is unchanged"), FileBytesHash(BlueprintFile) == BlueprintHash);
	return true;
}
