#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Operations/CortexReflectAssetEnumeration.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> SummaryTestNumbers(const int32 Count)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values.Add(MakeShared<FJsonValueNumber>(Index));
		}
		return Values;
	}

	TArray<FString> SummaryTestStringArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
	{
		TArray<FString> Strings;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Json.IsValid() && Json->TryGetArrayField(Field, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Strings.Add(Value->AsString());
			}
		}
		return Strings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSummaryTruncationFlagsTest,
	"Cortex.Reflect.OutPathSummary.TruncationFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectSummaryTruncationFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString OutPathText = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Cortex120"),
		FString::Printf(TEXT("SummaryTruncation_%s.json"), *FGuid::NewGuid().ToString().Right(8))));
	FCortexResolvedFilePath OutPath;
	FString ErrorCode;
	FString ErrorMessage;
	if (!TestTrue(*FString::Printf(TEXT("out_path resolves (%s)"), *ErrorMessage),
			FCortexSafeFileContract::ResolveWritePath(OutPathText, OutPath, ErrorCode, ErrorMessage)))
	{
		return false;
	}

	const int32 Limit = CortexReflectAssetEnumeration::SummaryEntryLimit;
	const TArray<FString> Capped = {TEXT("long"), TEXT("exact"), TEXT("short")};

	// One array over the limit, one exactly at it, one below it, one that is not listed for capping.
	{
		TSharedRef<FJsonObject> Full = MakeShared<FJsonObject>();
		Full->SetArrayField(TEXT("long"), SummaryTestNumbers(Limit + 2));
		Full->SetArrayField(TEXT("exact"), SummaryTestNumbers(Limit));
		Full->SetArrayField(TEXT("short"), SummaryTestNumbers(2));
		Full->SetArrayField(TEXT("untouched"), SummaryTestNumbers(Limit + 5));

		const FCortexCommandResult Result = CortexReflectAssetEnumeration::WriteReportWithSummary(OutPath, Full, Capped);
		if (TestTrue(TEXT("capped summary succeeds"), Result.bSuccess) && TestTrue(TEXT("data"), Result.Data.IsValid()))
		{
			bool bTruncated = false;
			TestTrue(TEXT("truncated is present"), Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestTrue(TEXT("a capped array sets truncated"), bTruncated);
			TestEqual(TEXT("only the array that lost entries is listed"),
				FString::Join(SummaryTestStringArray(Result.Data, TEXT("truncated_arrays")), TEXT(",")), FString(TEXT("long")));

			const TArray<TSharedPtr<FJsonValue>>* Long = nullptr;
			if (TestTrue(TEXT("long array present"), Result.Data->TryGetArrayField(TEXT("long"), Long)))
			{
				TestEqual(TEXT("long array keeps the first entries"), Long->Num(), Limit);
			}
			TestEqual(TEXT("long array total is the full count"),
				static_cast<int32>(Result.Data->GetNumberField(TEXT("long_total"))), Limit + 2);
			TestEqual(TEXT("an array not named for capping is copied whole"),
				static_cast<int32>(Result.Data->GetArrayField(TEXT("untouched")).Num()), Limit + 5);
			TestEqual(TEXT("out_path is reported"), Result.Data->GetStringField(TEXT("out_path")), OutPath.AbsolutePath);
		}
	}

	// Nothing over the limit: truncated is false and the list is empty.
	{
		TSharedRef<FJsonObject> Full = MakeShared<FJsonObject>();
		Full->SetArrayField(TEXT("exact"), SummaryTestNumbers(Limit));
		Full->SetArrayField(TEXT("short"), SummaryTestNumbers(2));

		const FCortexCommandResult Result = CortexReflectAssetEnumeration::WriteReportWithSummary(OutPath, Full, Capped);
		if (TestTrue(TEXT("uncapped summary succeeds"), Result.bSuccess) && TestTrue(TEXT("data"), Result.Data.IsValid()))
		{
			bool bTruncated = true;
			TestTrue(TEXT("truncated is present"), Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestFalse(TEXT("no capped array leaves truncated false"), bTruncated);
			const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
			if (TestTrue(TEXT("truncated_arrays is present"), Result.Data->TryGetArrayField(TEXT("truncated_arrays"), Listed)))
			{
				TestEqual(TEXT("truncated_arrays is empty"), Listed->Num(), 0);
			}
		}
	}

	IFileManager::Get().Delete(*OutPathText);
	return true;
}
