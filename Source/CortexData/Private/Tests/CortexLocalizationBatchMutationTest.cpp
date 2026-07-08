#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

namespace
{
	FCortexCommandRouter CreateLocalizationRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.0"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	UStringTable* CreateTestStringTable(const TCHAR* Name)
	{
		UStringTable* Table = NewObject<UStringTable>(
			GetTransientPackage(),
			FName(Name),
			RF_Public | RF_Standalone | RF_Transactional);
		if (Table != nullptr)
		{
			Table->GetMutableStringTable()->SetNamespace(TEXT("CortexLocalizationBatchMutationTest"));
		}
		return Table;
	}

	TSharedRef<FJsonObject> MakeSetOperation(const FString& Key, const FString& SourceString)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("type"), TEXT("set"));
		Operation->SetStringField(TEXT("key"), Key);
		Operation->SetStringField(TEXT("source_string"), SourceString);
		return Operation;
	}

	TSharedRef<FJsonObject> MakeRenameOperation(const FString& OldKey, const FString& NewKey)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("type"), TEXT("rename"));
		Operation->SetStringField(TEXT("old_key"), OldKey);
		Operation->SetStringField(TEXT("new_key"), NewKey);
		return Operation;
	}

	TSharedRef<FJsonObject> MakeCopyOperation(const FString& OldKey, const FString& NewKey)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("type"), TEXT("copy"));
		Operation->SetStringField(TEXT("old_key"), OldKey);
		Operation->SetStringField(TEXT("new_key"), NewKey);
		return Operation;
	}

	TSharedRef<FJsonObject> MakeDeleteOperation(const FString& Key)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("type"), TEXT("delete"));
		Operation->SetStringField(TEXT("key"), Key);
		return Operation;
	}

	TSharedRef<FJsonObject> MakeReplaceAllOperation(const FString& OldPrefix, const FString& NewPrefix)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("type"), TEXT("replace_all"));
		Operation->SetStringField(TEXT("old_prefix"), OldPrefix);
		Operation->SetStringField(TEXT("new_prefix"), NewPrefix);
		return Operation;
	}

	TSharedPtr<FJsonObject> MakeUpdateParams(
		UStringTable* Table,
		const TArray<TSharedRef<FJsonObject>>& Operations,
		const bool bDryRun,
		const bool bVerbose = false,
		const bool bAllowPartial = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("string_table_path"), Table->GetPathName());
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("verbose"), bVerbose);
		Params->SetBoolField(TEXT("allow_partial"), bAllowPartial);

		TArray<TSharedPtr<FJsonValue>> OperationValues;
		for (const TSharedRef<FJsonObject>& Operation : Operations)
		{
			OperationValues.Add(MakeShared<FJsonValueObject>(Operation));
		}
		Params->SetArrayField(TEXT("operations"), OperationValues);
		return Params;
	}

	FCortexCommandResult ExecuteUpdateStringTable(
		UStringTable* Table,
		const TArray<TSharedRef<FJsonObject>>& Operations,
		const bool bDryRun,
		const bool bVerbose = false,
		const bool bAllowPartial = false)
	{
		FCortexCommandRouter Router = CreateLocalizationRouter();
		return Router.Execute(
			TEXT("data.update_string_table"),
			MakeUpdateParams(Table, Operations, bDryRun, bVerbose, bAllowPartial));
	}

	bool GetSourceString(UStringTable* Table, const FString& Key, FString& OutSourceString)
	{
		if (Table == nullptr)
		{
			return false;
		}
		return Table->GetStringTable()->GetSourceString(Key, OutSourceString);
	}

	int32 GetArrayCount(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Array) || Array == nullptr)
		{
			return -1;
		}
		return Array->Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableDryRunSafetyTest,
	"Cortex.Data.Localization.UpdateStringTable.DryRunSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableDryRunSafetyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_DryRun"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeSetOperation(TEXT("fireball.title"), TEXT("Fireball")),
			MakeDeleteOperation(TEXT("entry.fireball.title")),
		},
		true);

	TestTrue(TEXT("dry-run update_string_table should return a response"), Result.bSuccess);
	TestTrue(TEXT("dry-run update_string_table should include data"), Result.Data.IsValid());

	FString ExistingValue;
	TestTrue(
		TEXT("dry-run should leave existing key in place"),
		GetSourceString(Table, TEXT("entry.fireball.title"), ExistingValue));
	TestEqual(TEXT("dry-run should preserve existing value"), ExistingValue, TEXT("Entry Fireball"));
	TestFalse(
		TEXT("dry-run should not add new key"),
		GetSourceString(Table, TEXT("fireball.title"), ExistingValue));

	if (Result.Data.IsValid())
	{
		bool bDryRun = false;
		bool bCompleted = false;
		double BeforeCount = -1.0;
		double AfterCount = -1.0;
		Result.Data->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Result.Data->TryGetBoolField(TEXT("completed"), bCompleted);
		Result.Data->TryGetNumberField(TEXT("before_key_count"), BeforeCount);
		Result.Data->TryGetNumberField(TEXT("after_key_count"), AfterCount);

		TestTrue(TEXT("response dry_run should be true"), bDryRun);
		TestTrue(TEXT("dry-run without blockers should complete"), bCompleted);
		TestEqual(TEXT("before count should reflect current table"), static_cast<int32>(BeforeCount), 1);
		TestEqual(TEXT("after count should reflect simulated result"), static_cast<int32>(AfterCount), 1);

		const TArray<TSharedPtr<FJsonValue>>* OperationResults = nullptr;
		TestTrue(TEXT("dry-run should include operation results"), Result.Data->TryGetArrayField(TEXT("operation_results"), OperationResults));
		if (OperationResults != nullptr)
		{
			TestEqual(TEXT("dry-run should report both operations"), OperationResults->Num(), 2);
			for (const TSharedPtr<FJsonValue>& OperationResultValue : *OperationResults)
			{
				const TSharedPtr<FJsonObject> OperationResult = OperationResultValue.IsValid() ? OperationResultValue->AsObject() : nullptr;
				TestTrue(TEXT("operation result should be an object"), OperationResult.IsValid());
				if (OperationResult.IsValid())
				{
					bool bApplied = true;
					bool bWouldApply = false;
					FString Status;
					OperationResult->TryGetBoolField(TEXT("applied"), bApplied);
					OperationResult->TryGetBoolField(TEXT("would_apply"), bWouldApply);
					OperationResult->TryGetStringField(TEXT("status"), Status);
					TestFalse(TEXT("dry-run operation should not be marked applied"), bApplied);
					TestTrue(TEXT("dry-run operation should be marked would_apply"), bWouldApply);
					TestEqual(TEXT("dry-run operation status should be would_apply"), Status, TEXT("would_apply"));
				}
			}
		}
	}

	Table->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableOrderedApplyTest,
	"Cortex.Data.Localization.UpdateStringTable.OrderedApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableOrderedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_Ordered"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));
	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.body"), TEXT("Deals fire damage"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeReplaceAllOperation(TEXT("entry."), TEXT("")),
			MakeSetOperation(TEXT("fireball.title"), TEXT("Fireball")),
			MakeCopyOperation(TEXT("fireball.body"), TEXT("fireball.body_copy")),
		},
		false,
		true);

	TestTrue(TEXT("ordered update_string_table should succeed"), Result.bSuccess);
	TestTrue(TEXT("ordered update_string_table should include data"), Result.Data.IsValid());

	FString Value;
	TestTrue(TEXT("replace_all should rename title key"), GetSourceString(Table, TEXT("fireball.title"), Value));
	TestEqual(TEXT("set after replace_all should override renamed title"), Value, TEXT("Fireball"));
	TestTrue(TEXT("replace_all should rename body key"), GetSourceString(Table, TEXT("fireball.body"), Value));
	TestEqual(TEXT("body value should be preserved"), Value, TEXT("Deals fire damage"));
	TestTrue(TEXT("copy should see the renamed body key"), GetSourceString(Table, TEXT("fireball.body_copy"), Value));
	TestEqual(TEXT("copy should preserve source value"), Value, TEXT("Deals fire damage"));
	TestFalse(TEXT("old title key should be gone"), GetSourceString(Table, TEXT("entry.fireball.title"), Value));

	if (Result.Data.IsValid())
	{
		bool bCompleted = false;
		bool bSaved = true;
		double BeforeCount = -1.0;
		double AfterCount = -1.0;
		Result.Data->TryGetBoolField(TEXT("completed"), bCompleted);
		Result.Data->TryGetBoolField(TEXT("saved"), bSaved);
		Result.Data->TryGetNumberField(TEXT("before_key_count"), BeforeCount);
		Result.Data->TryGetNumberField(TEXT("after_key_count"), AfterCount);

		TestTrue(TEXT("ordered operations should complete"), bCompleted);
		TestFalse(TEXT("save should default false"), bSaved);
		TestEqual(TEXT("before count should be 2"), static_cast<int32>(BeforeCount), 2);
		TestEqual(TEXT("after count should be 3"), static_cast<int32>(AfterCount), 3);
		TestEqual(TEXT("verbose response should include copied array"), GetArrayCount(Result.Data, TEXT("copied")), 1);
		TestEqual(TEXT("verbose response should include replaced array"), GetArrayCount(Result.Data, TEXT("replaced")), 2);
	}

	Table->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableCollisionReportingTest,
	"Cortex.Data.Localization.UpdateStringTable.CollisionReporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableCollisionReportingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_Collision"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));
	Table->GetMutableStringTable()->SetSourceString(TEXT("fireball.title"), TEXT("Existing Fireball"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeRenameOperation(TEXT("entry.fireball.title"), TEXT("fireball.title")),
		},
		false);

	TestTrue(TEXT("collision response should still include structured data"), Result.bSuccess);
	TestTrue(TEXT("collision response should include data"), Result.Data.IsValid());

	FString Value;
	TestTrue(TEXT("source key should remain after blocked collision"), GetSourceString(Table, TEXT("entry.fireball.title"), Value));
	TestEqual(TEXT("source value should remain after blocked collision"), Value, TEXT("Entry Fireball"));
	TestTrue(TEXT("target key should remain after blocked collision"), GetSourceString(Table, TEXT("fireball.title"), Value));
	TestEqual(TEXT("target value should remain after blocked collision"), Value, TEXT("Existing Fireball"));

	if (Result.Data.IsValid())
	{
		bool bHasBlockingIssues = false;
		bool bCompleted = true;
		Result.Data->TryGetBoolField(TEXT("has_blocking_issues"), bHasBlockingIssues);
		Result.Data->TryGetBoolField(TEXT("completed"), bCompleted);

		TestTrue(TEXT("collision should be blocking"), bHasBlockingIssues);
		TestFalse(TEXT("collision should prevent completion"), bCompleted);
		TestEqual(TEXT("collision should be reported"), GetArrayCount(Result.Data, TEXT("collisions")), 1);
		TestEqual(TEXT("operation result should be reported"), GetArrayCount(Result.Data, TEXT("operation_results")), 1);
	}

	Table->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableMissingKeyReportingTest,
	"Cortex.Data.Localization.UpdateStringTable.MissingKeyReporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableMissingKeyReportingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_Missing"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeCopyOperation(TEXT("entry.fireball.body"), TEXT("fireball.body_copy")),
		},
		false);

	TestTrue(TEXT("missing-key response should still include structured data"), Result.bSuccess);
	TestTrue(TEXT("missing-key response should include data"), Result.Data.IsValid());

	FString Value;
	TestTrue(TEXT("existing key should remain after blocked missing key"), GetSourceString(Table, TEXT("entry.fireball.title"), Value));
	TestFalse(TEXT("missing-key operation should not create target key"), GetSourceString(Table, TEXT("fireball.body_copy"), Value));

	if (Result.Data.IsValid())
	{
		bool bHasBlockingIssues = false;
		bool bCompleted = true;
		Result.Data->TryGetBoolField(TEXT("has_blocking_issues"), bHasBlockingIssues);
		Result.Data->TryGetBoolField(TEXT("completed"), bCompleted);

		TestTrue(TEXT("missing source key should be blocking"), bHasBlockingIssues);
		TestFalse(TEXT("missing source key should prevent completion"), bCompleted);
		TestEqual(TEXT("missing key should be reported"), GetArrayCount(Result.Data, TEXT("missing_keys")), 1);
		TestEqual(TEXT("operation result should be reported"), GetArrayCount(Result.Data, TEXT("operation_results")), 1);
	}

	Table->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableAllowPartialIssueResultsAreNonBlockingTest,
	"Cortex.Data.Localization.UpdateStringTable.AllowPartialIssueResultsAreNonBlocking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableAllowPartialIssueResultsAreNonBlockingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_AllowPartial"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeCopyOperation(TEXT("missing.key"), TEXT("copy.target")),
			MakeSetOperation(TEXT("fireball.title"), TEXT("Fireball")),
		},
		false,
		false,
		true);

	TestTrue(TEXT("allow_partial response should include structured data"), Result.bSuccess);
	TestTrue(TEXT("allow_partial response should include data"), Result.Data.IsValid());

	FString Value;
	TestTrue(TEXT("valid operation after missing key should apply"), GetSourceString(Table, TEXT("fireball.title"), Value));
	TestEqual(TEXT("valid operation value should apply"), Value, TEXT("Fireball"));

	if (Result.Data.IsValid())
	{
		bool bHasBlockingIssues = true;
		Result.Data->TryGetBoolField(TEXT("has_blocking_issues"), bHasBlockingIssues);
		TestFalse(TEXT("allow_partial should clear aggregate blockers"), bHasBlockingIssues);

		const TArray<TSharedPtr<FJsonValue>>* OperationResults = nullptr;
		TestTrue(TEXT("operation_results should exist"), Result.Data->TryGetArrayField(TEXT("operation_results"), OperationResults));
		if (OperationResults != nullptr && OperationResults->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstResult = nullptr;
			TestTrue(TEXT("first operation result should be an object"), (*OperationResults)[0]->TryGetObject(FirstResult));
			if (FirstResult != nullptr)
			{
				TestFalse(TEXT("allow_partial issue operation should not be marked blocking"),
					(*FirstResult)->GetBoolField(TEXT("blocking")));
			}
		}
	}

	Table->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUpdateStringTableStrictBlockedBatchDoesNotReportAppliedOpsTest,
	"Cortex.Data.Localization.UpdateStringTable.StrictBlockedBatchDoesNotReportAppliedOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUpdateStringTableStrictBlockedBatchDoesNotReportAppliedOpsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UStringTable* Table = CreateTestStringTable(TEXT("ST_CortexUpdateStringTable_StrictBlocked"));
	TestNotNull(TEXT("Test StringTable should be created"), Table);
	if (Table == nullptr)
	{
		return true;
	}

	Table->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Entry Fireball"), TEXT(""));

	const FCortexCommandResult Result = ExecuteUpdateStringTable(
		Table,
		{
			MakeSetOperation(TEXT("fireball.title"), TEXT("Fireball")),
			MakeCopyOperation(TEXT("missing.key"), TEXT("copy.target")),
		},
		false);

	TestTrue(TEXT("strict blocked response should include structured data"), Result.bSuccess);
	TestTrue(TEXT("strict blocked response should include data"), Result.Data.IsValid());

	FString Value;
	TestFalse(TEXT("valid operation before blocker should not mutate in strict batch"),
		GetSourceString(Table, TEXT("fireball.title"), Value));

	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("set_count should be cleared for blocked strict batch"),
			static_cast<int32>(Result.Data->GetNumberField(TEXT("set_count"))), 0);

		const TArray<TSharedPtr<FJsonValue>>* OperationResults = nullptr;
		TestTrue(TEXT("operation_results should exist"), Result.Data->TryGetArrayField(TEXT("operation_results"), OperationResults));
		if (OperationResults != nullptr && OperationResults->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstResult = nullptr;
			TestTrue(TEXT("first operation result should be an object"), (*OperationResults)[0]->TryGetObject(FirstResult));
			if (FirstResult != nullptr)
			{
				TestFalse(TEXT("valid operation before blocker should not be reported applied"),
					(*FirstResult)->GetBoolField(TEXT("applied")));
				TestEqual(TEXT("valid operation status should explain strict batch block"),
					(*FirstResult)->GetStringField(TEXT("status")), TEXT("not_applied_blocked_batch"));
			}
		}
	}

	Table->MarkAsGarbage();
	return true;
}
