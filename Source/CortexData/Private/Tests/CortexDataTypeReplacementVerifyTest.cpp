// #122 data.verify_type_replacement: redirects must be active (never a vacuous pass), load/compile/resave, the normalized baseline diff
// never masks a real difference, remaining referencers are reported.

#include "Misc/AutomationTest.h"

#include "CortexDataTypeReplacementTestSupport.h"
#include "CortexDataTypeReplacementTestTypes.h"
#include "CortexTypes.h"

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
