#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace CortexSourcePortability
{
	// UE 5.8 keys FJsonObject::Values by UE::TSharedString. Binding an element to
	// `const TPair<FString, TSharedPtr<FJsonValue>>&` builds a temporary pair per iteration;
	// MSVC accepts it silently, Clang (Linux, macOS) rejects it with -Werror,-Wrange-loop-construct.
	// The needle is assembled from two literals so this file never matches itself.
	const FString StringKeyedPair = FString(TEXT("const TPair<FString, ")) + TEXT("TSharedPtr<FJsonValue>>&");

	/** True for a range-for that binds FJsonObject::Values elements to a FString-keyed pair. */
	bool IsStringKeyedJsonObjectLoop(const FString& Line)
	{
		const int32 PairAt = Line.Find(StringKeyedPair, ESearchCase::CaseSensitive);
		if (PairAt == INDEX_NONE || !Line.Contains(TEXT("for"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		const int32 RangeAt = Line.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, PairAt);
		if (RangeAt == INDEX_NONE)
		{
			return false;
		}
		const FString Range = Line.Mid(RangeAt);
		return Range.Contains(TEXT("->Values)"), ESearchCase::CaseSensitive)
			|| Range.Contains(TEXT(".Values)"), ESearchCase::CaseSensitive);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSourcePortabilityJsonObjectKeyLoopsTest,
	"Cortex.Core.SourcePortability.JsonObjectKeyLoops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSourcePortabilityJsonObjectKeyLoopsTest::RunTest(const FString& Parameters)
{
	using namespace CortexSourcePortability;

	// The detector itself: one known-bad form, and the forms that must stay legal.
	TestTrue(TEXT("Detects a FString-keyed loop over FJsonObject::Values"),
		IsStringKeyedJsonObjectLoop(TEXT("for (") + StringKeyedPair + TEXT(" Pair : (*Item)->Values)")));
	TestTrue(TEXT("Detects the member-access form"),
		IsStringKeyedJsonObjectLoop(TEXT("for (") + StringKeyedPair + TEXT(" Pair : Object.Values)")));
	TestFalse(TEXT("Accepts const auto& over FJsonObject::Values"),
		IsStringKeyedJsonObjectLoop(TEXT("for (const auto& Pair : (*Item)->Values)")));
	TestFalse(TEXT("Accepts a FString-keyed loop over a plain TMap"),
		IsStringKeyedJsonObjectLoop(TEXT("for (") + StringKeyedPair + TEXT(" Pair : OverrideValues)")));

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
	if (!TestTrue(TEXT("UnrealCortex plugin is registered"), Plugin.IsValid()))
	{
		return false;
	}

	const FString SourceDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"));
	if (!IFileManager::Get().DirectoryExists(*SourceDir))
	{
		AddWarning(FString::Printf(TEXT("No plugin sources at %s; nothing to scan in a binary-only install"), *SourceDir));
		return true;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.cpp"), true, false);
	IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.h"), true, false, false);
	TestTrue(TEXT("Scanned a real source tree"), Files.Num() > 100);

	TArray<FString> Offenders;
	for (const FString& File : Files)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			AddError(FString::Printf(TEXT("Could not read %s"), *File));
			continue;
		}
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			if (IsStringKeyedJsonObjectLoop(Lines[Index]))
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(SourceDir + TEXT("/")));
				Offenders.Add(FString::Printf(TEXT("%s:%d"), *Relative, Index + 1));
			}
		}
	}

	AddInfo(FString::Printf(TEXT("Scanned %d source files under %s"), Files.Num(), *SourceDir));
	for (const FString& Offender : Offenders)
	{
		AddError(FString::Printf(
			TEXT("%s iterates FJsonObject::Values as a FString-keyed pair; use `const auto&` (Clang -Wrange-loop-construct)"),
			*Offender));
	}
	return Offenders.Num() == 0;
}
