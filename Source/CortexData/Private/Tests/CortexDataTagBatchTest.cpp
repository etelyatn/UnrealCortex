#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Operations/CortexGameplayTagOps.h"
#include "UObject/Package.h"

// Batch registration writes each target ini once and reports within-batch duplicates as
// already_existed (issue #224 item 5). The fixture ini lives under Saved so the repo's tracked
// Config/Tags files are never touched; the runtime tag tree does not pick the file up, which is
// the documented post-restart registration path, not an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRegisterGameplayTagsBatchTest,
	"Cortex.Data.RegisterGameplayTagsBatch.SingleIniWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRegisterGameplayTagsBatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString IniDir = FPaths::ProjectSavedDir() / TEXT("CortexTagBatchTest");
	IFileManager::Get().MakeDirectory(*IniDir, true);
	const FString IniFile = IniDir / FString::Printf(TEXT("Batch_%s.ini"), *Suffix);
	IFileManager::Get().Delete(*IniFile, false, true);

	const FString TagA = FString::Printf(TEXT("Cortex224.Alpha%s"), *Suffix);
	const FString TagB = FString::Printf(TEXT("Cortex224.Beta%s"), *Suffix);
	const FString TagC = FString::Printf(TEXT("Cortex224.Gamma%s"), *Suffix);

	auto MakeTagEntry = [](const FString& Tag, const FString& Ini) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag"), Tag);
		Entry->SetStringField(TEXT("ini_file"), Ini);
		return MakeShared<FJsonValueObject>(Entry.ToSharedRef());
	};

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Tags;
	Tags.Add(MakeTagEntry(TagA, IniFile));
	Tags.Add(MakeTagEntry(TagB, IniFile));
	Tags.Add(MakeTagEntry(TagC, IniFile));
	// Intra-batch duplicate of TagA: must not produce a second file line and must be
	// reported as already_existed.
	Tags.Add(MakeTagEntry(TagA, IniFile));
	Params->SetArrayField(TEXT("tags"), Tags);

	// The fixture ini lives outside the config hierarchy, so the refreshed runtime tree does not
	// load it; each newly written tag reports the documented post-restart path at warning level.
	AddExpectedMessagePlain(
		FString::Printf(TEXT("Tag '%s' was written to .ini"), *TagA),
		ELogVerbosity::Warning);
	AddExpectedMessagePlain(
		FString::Printf(TEXT("Tag '%s' was written to .ini"), *TagB),
		ELogVerbosity::Warning);
	AddExpectedMessagePlain(
		FString::Printf(TEXT("Tag '%s' was written to .ini"), *TagC),
		ELogVerbosity::Warning);

	const FCortexCommandResult Result = FCortexDataGameplayTagOps::RegisterGameplayTags(Params);

	const bool bSucceeded = TestTrue(TEXT("register_gameplay_tags should succeed"), Result.bSuccess)
		&& Result.Data.IsValid();
	if (!bSucceeded)
	{
		IFileManager::Get().Delete(*IniFile, false, true);
		return false;
	}

	double Registered = -1.0;
	double AlreadyExisted = -1.0;
	double Failed = -1.0;
	Result.Data->TryGetNumberField(TEXT("registered"), Registered);
	Result.Data->TryGetNumberField(TEXT("already_existed"), AlreadyExisted);
	Result.Data->TryGetNumberField(TEXT("failed"), Failed);
	TestEqual(TEXT("Three distinct tags registered"), static_cast<int32>(Registered), 3);
	TestEqual(TEXT("Intra-batch duplicate reported as already_existed"), static_cast<int32>(AlreadyExisted), 1);
	TestEqual(TEXT("No tag failed"), static_cast<int32>(Failed), 0);

	FString IniContents;
	TestTrue(TEXT("Fixture ini readable"), FFileHelper::LoadFileToString(IniContents, *IniFile));
	int32 LineCount = 0;
	TArray<FString> IniLines;
	IniContents.ParseIntoArrayLines(IniLines);
	for (const FString& IniLine : IniLines)
	{
		if (IniLine.Contains(TEXT("+GameplayTagList")))
		{
			++LineCount;
		}
	}
	TestEqual(TEXT("One ini line per distinct tag"), LineCount, 3);

	IFileManager::Get().Delete(*IniFile, false, true);
	return true;
}
