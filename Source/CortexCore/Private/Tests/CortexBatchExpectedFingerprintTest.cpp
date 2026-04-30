#include "Misc/AutomationTest.h"
#include "CortexBatchMutation.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
TSharedPtr<FJsonObject> MakeFingerprintJson(const FString& SavedHash)
{
	TSharedPtr<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(TEXT("package_saved_hash"), SavedHash);
	Fingerprint->SetBoolField(TEXT("is_dirty"), false);
	Fingerprint->SetStringField(TEXT("dirty_epoch"), TEXT("7"));
	Fingerprint->SetBoolField(TEXT("not_ready"), false);
	return Fingerprint;
}

TSharedPtr<FJsonObject> MakeFingerprintJsonWithOptionalField(
	const FString& SavedHash,
	double CompiledSignatureCrc)
{
	TSharedPtr<FJsonObject> Fingerprint = MakeFingerprintJson(SavedHash);
	Fingerprint->SetNumberField(TEXT("compiled_signature_crc"), CompiledSignatureCrc);
	return Fingerprint;
}

TSharedPtr<FJsonObject> MakeBatchParamsWithItems()
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("target"), TEXT("/Game/Test/Asset.Asset"));
	Item->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	Item->SetObjectField(TEXT("expected_fingerprint"), MakeFingerprintJson(TEXT("stale-hash")));

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(Item));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("items"), Items);
	return Params;
}

TSharedPtr<FJsonObject> MakeEmptyItemsParams()
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("items"), {});
	return Params;
}

TSharedPtr<FJsonObject> MakeMalformedItemsParams()
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("items"), TEXT("not-an-array"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/Fallback.Asset"));
	return Params;
}

TSharedPtr<FJsonObject> MakeMalformedBatchItemExpectedFingerprintParams()
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("target"), TEXT("/Game/Test/Asset.Asset"));
	Item->SetStringField(TEXT("expected_fingerprint"), TEXT("not-an-object"));

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(Item));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("items"), Items);
	return Params;
}

TSharedPtr<FJsonObject> MakeMalformedSingleExpectedFingerprintParams()
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/Single.Asset"));
	Params->SetStringField(TEXT("expected_fingerprint"), TEXT("not-an-object"));
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchExpectedFingerprintTest,
	"Cortex.Core.Batch.ExpectedFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchExpectedFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexCommandResult ParseError;
	TestTrue(
		TEXT("Shared parser accepts items payload"),
		FCortexBatchMutation::ParseRequest(MakeBatchParamsWithItems(), TEXT("asset_path"), Request, ParseError));

	bool bCommitRan = false;
	bool bPreflightRan = false;

	const FCortexBatchMutationResult Result = FCortexBatchMutation::Run(
		Request,
		[&bPreflightRan](const FCortexBatchMutationItem& Item)
		{
			bPreflightRan = true;
			return FCortexBatchPreflightResult::Success(MakeFingerprintJson(TEXT("current-hash")));
		},
		[&bCommitRan](const FCortexBatchMutationItem& Item)
		{
			bCommitRan = true;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		});

	TestTrue(TEXT("preflight executed"), bPreflightRan);
	TestEqual(TEXT("status"), Result.Status, TEXT("preflight_failed"));
	TestFalse(TEXT("commit blocked"), bCommitRan);
	TestEqual(TEXT("error code"), Result.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestEqual(TEXT("per-item count"), Result.PerItem.Num(), 1);
	if (Result.PerItem.Num() == 1)
	{
		TestFalse(TEXT("per-item not ok"), Result.PerItem[0].bOk);
		TestEqual(TEXT("per-item target"), Result.PerItem[0].Target, TEXT("/Game/Test/Asset.Asset"));
		TestEqual(TEXT("per-item error code"), Result.PerItem[0].Result.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchExpectedFingerprintOptionalFieldToleranceTest,
	"Cortex.Core.Batch.ExpectedFingerprint.OptionalFieldTolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchExpectedFingerprintOptionalFieldToleranceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexBatchMutationItem Item;
	Item.Target = TEXT("/Game/Test/OptionalFingerprint.Asset");
	Item.Params = MakeShared<FJsonObject>();
	Item.ExpectedFingerprint = MakeFingerprintJsonWithOptionalField(TEXT("shared-hash"), 1337.0);
	Request.Items.Add(Item);

	bool bCommitRan = false;
	const FCortexBatchMutationResult Result = FCortexBatchMutation::Run(
		Request,
		[](const FCortexBatchMutationItem& InItem)
		{
			return FCortexBatchPreflightResult::Success(MakeFingerprintJson(TEXT("shared-hash")));
		},
		[&bCommitRan](const FCortexBatchMutationItem& InItem)
		{
			bCommitRan = true;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		});

	TestEqual(TEXT("status"), Result.Status, TEXT("committed"));
	TestTrue(TEXT("commit allowed when optional field omitted"), bCommitRan);
	TestEqual(TEXT("per-item count"), Result.PerItem.Num(), 1);
	if (Result.PerItem.Num() == 1)
	{
		TestTrue(TEXT("per-item ok"), Result.PerItem[0].bOk);
		TestTrue(TEXT("per-item result success"), Result.PerItem[0].Result.bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchParseItemsRequestTest,
	"Cortex.Core.Batch.ParseItemsRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchParseItemsRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexCommandResult ParseError;

	const bool bParsed = FCortexBatchMutation::ParseRequest(
		MakeBatchParamsWithItems(),
		TEXT("asset_path"),
		Request,
		ParseError);

	TestTrue(TEXT("items request parsed"), bParsed);
	TestTrue(TEXT("parse error empty"), bParsed ? ParseError.ErrorCode.IsEmpty() : !ParseError.ErrorCode.IsEmpty());
	TestEqual(TEXT("item count"), Request.Items.Num(), 1);
	if (Request.Items.Num() == 1)
	{
		TestEqual(TEXT("target parsed"), Request.Items[0].Target, TEXT("/Game/Test/Asset.Asset"));
		TestTrue(TEXT("expected fingerprint parsed"), Request.Items[0].ExpectedFingerprint.IsValid());
		TestTrue(TEXT("item params parsed"), Request.Items[0].Params.IsValid() && Request.Items[0].Params->HasField(TEXT("properties")));
		TestFalse(TEXT("target removed from item params"), Request.Items[0].Params->HasField(TEXT("target")));
		TestFalse(TEXT("expected fingerprint removed from item params"), Request.Items[0].Params->HasField(TEXT("expected_fingerprint")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchParseEmptyItemsRequestTest,
	"Cortex.Core.Batch.ParseEmptyItemsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchParseEmptyItemsRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexCommandResult ParseError;

	const bool bParsed = FCortexBatchMutation::ParseRequest(
		MakeEmptyItemsParams(),
		TEXT("asset_path"),
		Request,
		ParseError);

	TestFalse(TEXT("empty items rejected"), bParsed);
	TestEqual(TEXT("parse error code"), ParseError.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("no items parsed"), Request.Items.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchParseMalformedItemsRequestTest,
	"Cortex.Core.Batch.ParseMalformedItemsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchParseMalformedItemsRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	Request.Items = {
		{ TEXT("/Game/Test/Partial.Asset"), MakeShared<FJsonObject>(), nullptr }
	};

	FCortexCommandResult ParseError;
	const bool bParsed = FCortexBatchMutation::ParseRequest(
		MakeMalformedItemsParams(),
		TEXT("asset_path"),
		Request,
		ParseError);

	TestFalse(TEXT("non-array items rejected"), bParsed);
	TestEqual(TEXT("parse error code"), ParseError.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("request reset on failure"), Request.Items.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchParseMalformedBatchExpectedFingerprintTest,
	"Cortex.Core.Batch.ParseMalformedBatchExpectedFingerprintRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchParseMalformedBatchExpectedFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexCommandResult ParseError;

	const bool bParsed = FCortexBatchMutation::ParseRequest(
		MakeMalformedBatchItemExpectedFingerprintParams(),
		TEXT("asset_path"),
		Request,
		ParseError);

	TestFalse(TEXT("malformed batch expected_fingerprint rejected"), bParsed);
	TestEqual(TEXT("parse error code"), ParseError.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("request reset on failure"), Request.Items.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchParseMalformedSingleExpectedFingerprintTest,
	"Cortex.Core.Batch.ParseMalformedSingleExpectedFingerprintRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchParseMalformedSingleExpectedFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	FCortexCommandResult ParseError;

	const bool bParsed = FCortexBatchMutation::ParseRequest(
		MakeMalformedSingleExpectedFingerprintParams(),
		TEXT("asset_path"),
		Request,
		ParseError);

	TestFalse(TEXT("malformed single expected_fingerprint rejected"), bParsed);
	TestEqual(TEXT("parse error code"), ParseError.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("request reset on failure"), Request.Items.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchPreflightFailedUnwrittenTargetsTest,
	"Cortex.Core.Batch.PreflightFailedUnwrittenTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchPreflightFailedUnwrittenTargetsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	Request.Items = {
		{ TEXT("/Game/Test/AssetA.AssetA"), MakeShared<FJsonObject>(), nullptr },
		{ TEXT("/Game/Test/AssetB.AssetB"), MakeShared<FJsonObject>(), nullptr },
		{ TEXT("/Game/Test/AssetC.AssetC"), MakeShared<FJsonObject>(), nullptr }
	};

	int32 PreflightCount = 0;
	bool bCommitRan = false;

	const FCortexBatchMutationResult Result = FCortexBatchMutation::Run(
		Request,
		[&PreflightCount](const FCortexBatchMutationItem& Item)
		{
			++PreflightCount;
			if (Item.Target == TEXT("/Game/Test/AssetB.AssetB"))
			{
				return FCortexBatchPreflightResult::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Simulated preflight failure"));
			}

			return FCortexBatchPreflightResult::Success();
		},
		[&bCommitRan](const FCortexBatchMutationItem& Item)
		{
			bCommitRan = true;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		});

	TestEqual(TEXT("status"), Result.Status, TEXT("preflight_failed"));
	TestEqual(TEXT("all items preflighted before abort"), PreflightCount, 3);
	TestFalse(TEXT("commit not started"), bCommitRan);
	TestEqual(TEXT("written targets remain empty"), Result.WrittenTargets.Num(), 0);
	TestEqual(TEXT("unwritten target count"), Result.UnwrittenTargets.Num(), 3);
	if (Result.UnwrittenTargets.Num() == 3)
	{
		TestEqual(TEXT("first target unwritten"), Result.UnwrittenTargets[0], TEXT("/Game/Test/AssetA.AssetA"));
		TestEqual(TEXT("failing target included"), Result.UnwrittenTargets[1], TEXT("/Game/Test/AssetB.AssetB"));
		TestEqual(TEXT("remaining target included"), Result.UnwrittenTargets[2], TEXT("/Game/Test/AssetC.AssetC"));
	}
	TestEqual(TEXT("per-item results cover whole batch"), Result.PerItem.Num(), 3);
	if (Result.PerItem.Num() == 3)
	{
		TestFalse(TEXT("first item was not committed"), Result.PerItem[0].bOk);
		TestFalse(TEXT("first item result not successful"), Result.PerItem[0].Result.bSuccess);
		TestEqual(TEXT("first item blocked error code"), Result.PerItem[0].Result.ErrorCode, CortexErrorCodes::CompositeWriteBlocked);
		TestFalse(TEXT("second item failed"), Result.PerItem[1].bOk);
		TestEqual(TEXT("second item error code"), Result.PerItem[1].Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestFalse(TEXT("third item was not committed"), Result.PerItem[2].bOk);
		TestEqual(TEXT("third item blocked error code"), Result.PerItem[2].Result.ErrorCode, CortexErrorCodes::CompositeWriteBlocked);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchPartialCommitPerItemContractTest,
	"Cortex.Core.Batch.PartialCommitPerItemContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchPartialCommitPerItemContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBatchMutationRequest Request;
	Request.Items = {
		{ TEXT("/Game/Test/AssetA.AssetA"), MakeShared<FJsonObject>(), nullptr },
		{ TEXT("/Game/Test/AssetB.AssetB"), MakeShared<FJsonObject>(), nullptr },
		{ TEXT("/Game/Test/AssetC.AssetC"), MakeShared<FJsonObject>(), nullptr }
	};

	int32 CommitCount = 0;
	const FCortexBatchMutationResult Result = FCortexBatchMutation::Run(
		Request,
		[](const FCortexBatchMutationItem& Item)
		{
			return FCortexBatchPreflightResult::Success();
		},
		[&CommitCount](const FCortexBatchMutationItem& Item)
		{
			++CommitCount;
			if (Item.Target == TEXT("/Game/Test/AssetB.AssetB"))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, TEXT("Simulated commit failure"));
			}

			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		});

	TestEqual(TEXT("status"), Result.Status, TEXT("partial_commit"));
	TestEqual(TEXT("commit count stops at failure"), CommitCount, 2);
	TestEqual(TEXT("written target count"), Result.WrittenTargets.Num(), 1);
	if (Result.WrittenTargets.Num() == 1)
	{
		TestEqual(TEXT("written target"), Result.WrittenTargets[0], TEXT("/Game/Test/AssetA.AssetA"));
	}
	TestEqual(TEXT("unwritten target count"), Result.UnwrittenTargets.Num(), 2);
	if (Result.UnwrittenTargets.Num() == 2)
	{
		TestEqual(TEXT("failed target included"), Result.UnwrittenTargets[0], TEXT("/Game/Test/AssetB.AssetB"));
		TestEqual(TEXT("remaining target included"), Result.UnwrittenTargets[1], TEXT("/Game/Test/AssetC.AssetC"));
	}
	TestEqual(TEXT("per-item count"), Result.PerItem.Num(), 3);
	if (Result.PerItem.Num() == 3)
	{
		TestTrue(TEXT("first item committed"), Result.PerItem[0].bOk);
		TestTrue(TEXT("first result success"), Result.PerItem[0].Result.bSuccess);
		TestFalse(TEXT("second item failed commit"), Result.PerItem[1].bOk);
		TestEqual(TEXT("second error code"), Result.PerItem[1].Result.ErrorCode, CortexErrorCodes::SaveFailed);
		TestFalse(TEXT("third item skipped"), Result.PerItem[2].bOk);
		TestFalse(TEXT("third result not success"), Result.PerItem[2].Result.bSuccess);
		TestEqual(TEXT("third skipped error code"), Result.PerItem[2].Result.ErrorCode, CortexErrorCodes::CompositeWriteBlocked);
	}

	return true;
}
