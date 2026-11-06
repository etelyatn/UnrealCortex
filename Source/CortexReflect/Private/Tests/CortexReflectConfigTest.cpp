#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"
#include "Operations/CortexReflectConfigScan.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"

namespace
{
/** Writes a uniquely named ini into the project Config dir and removes it (plus an optional content dir) on scope exit. */
struct FTempConfigFixture
{
	FString IniName;
	FString IniPath;
	FString ContentDir;

	explicit FTempConfigFixture(const FString& Body)
	{
		IniName = FString::Printf(TEXT("CortexTest120_%s.ini"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), IniName);
		bWritten = FFileHelper::SaveStringToFile(Body, *IniPath);
	}

	~FTempConfigFixture()
	{
		IFileManager::Get().Delete(*IniPath, false, true, true);
		if (!ContentDir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*ContentDir, false, true);
		}
	}

	bool bWritten = false;
};

TArray<CortexReflectConfigScan::FConfigReference> RefsFromFile(const FString& IniName)
{
	TArray<CortexReflectConfigScan::FConfigReference> Out;
	for (const CortexReflectConfigScan::FConfigReference& Ref : CortexReflectConfigScan::Scan())
	{
		if (Ref.File.EndsWith(IniName))
		{
			Out.Add(Ref);
		}
	}
	return Out;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigScanFormatsTest,
	"Cortex.Reflect.Config.ScanFormats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigScanFormatsTest::RunTest(const FString& Parameters)
{
	FTempConfigFixture Fixture(
		TEXT("; /Game/Commented/BP_Skip.BP_Skip\n")
		TEXT("# /Game/Commented2/Skip\n")
		TEXT("[/Script/Test.Settings]\n")
		TEXT("+Items=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\n")
		TEXT("Map=(Path=\"/Game/X/Y\")\n")
		TEXT("Dir=/Game/Missing120/\n")
		TEXT("Native=/Script/Engine.Actor\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}

	const TArray<CortexReflectConfigScan::FConfigReference> Refs = RefsFromFile(Fixture.IniName);
	if (!TestEqual(TEXT("comments and /Script/ tokens are skipped"), Refs.Num(), 3))
	{
		return false;
	}

	TestEqual(TEXT("item line"), Refs[0].Line, 4);
	TestEqual(TEXT("item section"), Refs[0].Section, FString(TEXT("/Script/Test.Settings")));
	TestEqual(TEXT("item key strips + prefix"), Refs[0].Key, FString(TEXT("Items")));
	TestEqual(TEXT("item path"), Refs[0].Path, FString(TEXT("/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C")));
	TestEqual(TEXT("item package"), Refs[0].PackageName, FString(TEXT("/Game/Blueprints/BP_SimpleActor")));
	TestFalse(TEXT("item is a package"), Refs[0].bDirectory);

	TestEqual(TEXT("map line"), Refs[1].Line, 5);
	TestEqual(TEXT("map key"), Refs[1].Key, FString(TEXT("Map")));
	TestEqual(TEXT("map path trimmed of quotes/parens"), Refs[1].Path, FString(TEXT("/Game/X/Y")));
	TestTrue(TEXT("missing dotless path is directory-like"), Refs[1].bDirectory);

	TestEqual(TEXT("dir line"), Refs[2].Line, 6);
	TestEqual(TEXT("dir package drops trailing slash"), Refs[2].PackageName, FString(TEXT("/Game/Missing120")));
	TestTrue(TEXT("trailing slash is directory"), Refs[2].bDirectory);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigReferencersTest,
	"Cortex.Reflect.Config.ReferencersIncludeConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigReferencersTest::RunTest(const FString& Parameters)
{
	FTempConfigFixture Fixture(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("+Items=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}

	auto FindConfigRow = [&Fixture](const FCortexCommandResult& Result, int32& OutConfigRows) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Found;
		OutConfigRows = 0;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("referencers"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				const TSharedPtr<FJsonObject> Obj = Row->AsObject();
				if (Obj.IsValid() && Obj->GetStringField(TEXT("type")) == TEXT("config"))
				{
					++OutConfigRows;
					if (Obj->GetStringField(TEXT("file")).EndsWith(Fixture.IniName))
					{
						Found = Obj;
					}
				}
			}
		}
		return Found;
	};

	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_SimpleActor"));
	Params->SetNumberField(TEXT("limit"), 5000);

	const FCortexCommandResult With = Handler.Execute(TEXT("get_referencers"), Params);
	if (!TestTrue(TEXT("get_referencers succeeds"), With.bSuccess))
	{
		return false;
	}
	int32 ConfigRows = 0;
	const TSharedPtr<FJsonObject> Row = FindConfigRow(With, ConfigRows);
	if (TestTrue(TEXT("config row for the temp ini"), Row.IsValid()))
	{
		TestEqual(TEXT("row line"), static_cast<int32>(Row->GetNumberField(TEXT("line"))), 2);
		TestEqual(TEXT("row section"), Row->GetStringField(TEXT("section")), FString(TEXT("/Script/Test.Settings")));
		TestEqual(TEXT("row key"), Row->GetStringField(TEXT("key")), FString(TEXT("Items")));
		TestEqual(TEXT("row package is the ini file"), Row->GetStringField(TEXT("package")), Row->GetStringField(TEXT("file")));
	}
	TestTrue(TEXT("total includes config rows"), With.Data->GetNumberField(TEXT("total")) >= ConfigRows);
	TestTrue(TEXT("total_unfiltered includes config rows"), With.Data->GetNumberField(TEXT("total_unfiltered")) >= ConfigRows);

	// limit counts config rows too.
	Params->SetNumberField(TEXT("limit"), 1);
	const FCortexCommandResult Limited = Handler.Execute(TEXT("get_referencers"), Params);
	const TArray<TSharedPtr<FJsonValue>>* LimitedRows = nullptr;
	if (TestTrue(TEXT("limited call succeeds"), Limited.bSuccess)
		&& Limited.Data->TryGetArrayField(TEXT("referencers"), LimitedRows))
	{
		TestTrue(TEXT("limit applies to all rows"), LimitedRows->Num() <= 1);
		if (LimitedRows->Num() == 1)
		{
			TestEqual(TEXT("config rows are listed first so limit keeps them"), (*LimitedRows)[0]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("config")));
		}
	}

	Params->SetNumberField(TEXT("limit"), 5000);
	Params->SetBoolField(TEXT("include_config"), false);
	const FCortexCommandResult Without = Handler.Execute(TEXT("get_referencers"), Params);
	if (TestTrue(TEXT("include_config=false succeeds"), Without.bSuccess))
	{
		int32 Dummy = 0;
		TestFalse(TEXT("no config rows when include_config=false"), FindConfigRow(Without, Dummy).IsValid());
		TestEqual(TEXT("no config rows at all"), Dummy, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigBrokenPathsTest,
	"Cortex.Reflect.Config.BrokenPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigBrokenPathsTest::RunTest(const FString& Parameters)
{
	const FString EmptyDirName = FString::Printf(TEXT("CortexTest120Empty_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FTempConfigFixture Fixture(FString::Printf(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("Good=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\n")
		TEXT("MissingPkg=/Game/Missing120/BP_Gone.BP_Gone_C\n")
		TEXT("MissingDir=/Game/Missing120Dir/\n")
		TEXT("EmptyDir=/Game/%s/\n"), *EmptyDirName));
	Fixture.ContentDir = FPaths::Combine(FPaths::ProjectContentDir(), EmptyDirName);
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten)
		|| !TestTrue(TEXT("empty content dir created"), IFileManager::Get().MakeDirectory(*Fixture.ContentDir, true)))
	{
		return false;
	}

	FCortexReflectCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("find_broken_config_paths"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("find_broken_config_paths succeeds"), Result.bSuccess) || !Result.Data.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("scanned_files reported"), Result.Data->GetNumberField(TEXT("scanned_files")) >= 1.0);
	TestTrue(TEXT("checked reported"), Result.Data->GetNumberField(TEXT("checked")) >= 4.0);

	TMap<FString, FString> BrokenKindByKey;
	const TArray<TSharedPtr<FJsonValue>>* Broken = nullptr;
	if (!TestTrue(TEXT("broken array present"), Result.Data->TryGetArrayField(TEXT("broken"), Broken)))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Broken)
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (Obj.IsValid() && Obj->GetStringField(TEXT("file")).EndsWith(Fixture.IniName))
		{
			BrokenKindByKey.Add(Obj->GetStringField(TEXT("key")), Obj->GetStringField(TEXT("kind")));
			TestTrue(TEXT("row has line"), Obj->HasField(TEXT("line")));
			TestTrue(TEXT("row has section"), Obj->HasField(TEXT("section")));
			TestTrue(TEXT("row has value"), Obj->HasField(TEXT("value")));
			TestTrue(TEXT("row has path"), Obj->HasField(TEXT("path")));
		}
	}

	TestEqual(TEXT("exactly the missing package and directory are broken"), BrokenKindByKey.Num(), 2);
	TestEqual(TEXT("missing package kind"), BrokenKindByKey.FindRef(TEXT("MissingPkg")), FString(TEXT("package")));
	TestEqual(TEXT("missing directory kind"), BrokenKindByKey.FindRef(TEXT("MissingDir")), FString(TEXT("directory")));
	TestFalse(TEXT("existing package not reported"), BrokenKindByKey.Contains(TEXT("Good")));
	TestFalse(TEXT("existing empty disk directory not reported"), BrokenKindByKey.Contains(TEXT("EmptyDir")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigTokenAndSkipTest,
	"Cortex.Reflect.Config.PlusWildcardAndRedirects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigTokenAndSkipTest::RunTest(const FString& Parameters)
{
	// The first line is the real DefaultEditor.ini shape that was truncated at '+'.
	const FString CubeMapPath = TEXT("/Engine/EditorMaterials/AssetViewer/EpicQuadPanorama_CC+EV1.EpicQuadPanorama_CC+EV1");
	FTempConfigFixture Fixture(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("EnvironmentCubeMapPath=\"") + CubeMapPath + TEXT("\"\n")
		TEXT("Wild=/Game/Maps/Test_*\n")
		TEXT("WildPair=(Path=\"/Game/Missing120Wild/*.*\")\n")
		TEXT("WildQuestion=/Game/Missing120Wild/BP_?\n")
		TEXT("+PackageRedirects=(OldName=\"/Game/Old120/Gone\",NewName=\"/Game/Blueprints/BP_SimpleActor\")\n")
		TEXT("[CoreRedirects]\n")
		TEXT("+ClassRedirects=(OldName=\"/Game/Old120/Gone.Gone_C\",NewName=\"/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\")\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}

	const TArray<CortexReflectConfigScan::FConfigReference> Refs = RefsFromFile(Fixture.IniName);
	if (!TestEqual(TEXT("cube map + two redirect targets; wildcards and redirect Old* skipped"), Refs.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("'+' stays inside the token"), Refs[0].Path, CubeMapPath);
	TestEqual(TEXT("package name keeps '+'"), Refs[0].PackageName, FString(TEXT("/Engine/EditorMaterials/AssetViewer/EpicQuadPanorama_CC+EV1")));
	TestEqual(TEXT("package redirect target"), Refs[1].Path, FString(TEXT("/Game/Blueprints/BP_SimpleActor")));
	TestEqual(TEXT("core redirect target"), Refs[2].Path, FString(TEXT("/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C")));

	// The cube map exists in the engine, so find_broken_config_paths must not report this line.
	FCortexReflectCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("find_broken_config_paths"), MakeShared<FJsonObject>());
	if (TestTrue(TEXT("find_broken_config_paths succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Broken = nullptr;
		if (TestTrue(TEXT("broken array present"), Result.Data->TryGetArrayField(TEXT("broken"), Broken)))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Broken)
			{
				const TSharedPtr<FJsonObject> Obj = Value->AsObject();
				TestFalse(TEXT("nothing from the fixture ini is broken"), Obj.IsValid() && Obj->GetStringField(TEXT("file")).EndsWith(Fixture.IniName));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigPackageFolderCharsTest,
	"Cortex.Reflect.Config.PackageFolderCharacters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigPackageFolderCharsTest::RunTest(const FString& Parameters)
{
	// Every character FPackageName accepts in a long package name stays inside the token: it only rejects
	// \ : * ? " < > | ' , . & ! ~ @ # and whitespace (INVALID_LONGPACKAGE_CHARACTERS), so parentheses, braces, brackets,
	// '=', ';', '^', '%', '$' and '`' are legal folder characters. An unbalanced closing bracket still ends a token,
	// which keeps unquoted struct syntax such as (Map=/Game/Maps/Arena) working.
	FTempConfigFixture Fixture(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("Paren=\"/Game/Folder(Variant)/BP_A.BP_A_C\"\n")
		TEXT("Brace=\"/Game/Set{One}/BP_B.BP_B_C\"\n")
		TEXT("Bracket=(Path=\"/Game/Dir[2]/BP_C.BP_C_C\")\n")
		TEXT("Odd=\"/Game/Odd^Folder%100$x`y/BP_D.BP_D_C\"\n")
		TEXT("Struct=(Map=/Game/Maps/Arena)\n")
		TEXT("Nested=(Map=/Game/Maps(Alt)/Arena)\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}

	const TArray<CortexReflectConfigScan::FConfigReference> Refs = RefsFromFile(Fixture.IniName);
	if (!TestEqual(TEXT("one reference per line"), Refs.Num(), 6))
	{
		return false;
	}
	TestEqual(TEXT("parentheses inside a folder name"), Refs[0].Path, FString(TEXT("/Game/Folder(Variant)/BP_A.BP_A_C")));
	TestEqual(TEXT("package of the parenthesised folder"), Refs[0].PackageName, FString(TEXT("/Game/Folder(Variant)/BP_A")));
	TestEqual(TEXT("braces inside a folder name"), Refs[1].Path, FString(TEXT("/Game/Set{One}/BP_B.BP_B_C")));
	TestEqual(TEXT("brackets inside a folder name"), Refs[2].Path, FString(TEXT("/Game/Dir[2]/BP_C.BP_C_C")));
	TestEqual(TEXT("^ % $ and ` inside a folder name"), Refs[3].Path, FString(TEXT("/Game/Odd^Folder%100$x`y/BP_D.BP_D_C")));
	TestEqual(TEXT("an unbalanced ) ends an unquoted token"), Refs[4].Path, FString(TEXT("/Game/Maps/Arena")));
	TestEqual(TEXT("a balanced pair stays inside an unquoted token"), Refs[5].Path, FString(TEXT("/Game/Maps(Alt)/Arena")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigUnreadableTest,
	"Cortex.Reflect.Config.UnreadableFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigUnreadableTest::RunTest(const FString& Parameters)
{
	FTempConfigFixture Fixture(TEXT("[/Script/Test.Settings]\nGood=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}

	FCortexReflectCommandHandler Handler;
	const FCortexCommandResult Baseline = Handler.Execute(TEXT("find_broken_config_paths"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("baseline succeeds"), Baseline.bSuccess) || !Baseline.Data.IsValid())
	{
		return false;
	}
	const int32 BaselineScanned = static_cast<int32>(Baseline.Data->GetNumberField(TEXT("scanned_files")));

	// An open exclusive writer makes the ini unreadable for the scan.
	TUniquePtr<FArchive> Lock(IFileManager::Get().CreateFileWriter(*Fixture.IniPath, FILEWRITE_Append));
	if (!TestNotNull(TEXT("lock acquired"), Lock.Get()))
	{
		return false;
	}
	AddExpectedMessage(TEXT("Config scan: could not read"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FCortexCommandResult Locked = Handler.Execute(TEXT("find_broken_config_paths"), MakeShared<FJsonObject>());
	if (TestTrue(TEXT("locked run succeeds"), Locked.bSuccess) && Locked.Data.IsValid())
	{
		TestEqual(TEXT("scanned_files counts files actually read"), static_cast<int32>(Locked.Data->GetNumberField(TEXT("scanned_files"))), BaselineScanned - 1);
		const TArray<TSharedPtr<FJsonValue>>* Unreadable = nullptr;
		if (TestTrue(TEXT("unreadable_files present"), Locked.Data->TryGetArrayField(TEXT("unreadable_files"), Unreadable)))
		{
			bool bNamed = false;
			for (const TSharedPtr<FJsonValue>& Value : *Unreadable)
			{
				bNamed |= Value->AsString().EndsWith(Fixture.IniName);
			}
			TestTrue(TEXT("the locked ini is listed"), bNamed);
		}
	}
	Lock.Reset();

	const FCortexCommandResult Free = Handler.Execute(TEXT("find_broken_config_paths"), MakeShared<FJsonObject>());
	if (TestTrue(TEXT("unlocked run succeeds"), Free.bSuccess) && Free.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Unreadable = nullptr;
		if (TestTrue(TEXT("unreadable_files present when empty"), Free.Data->TryGetArrayField(TEXT("unreadable_files"), Unreadable)))
		{
			TestEqual(TEXT("nothing unreadable"), Unreadable->Num(), 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigQuotedBracketsTest,
	"Cortex.Reflect.Config.QuotedUnmatchedBrackets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigQuotedBracketsTest::RunTest(const FString& Parameters)
{
	// Inside quotes only the closing quote ends the path: an unmatched closer is part of the folder name.
	FTempConfigFixture Fixture(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("Paren=\"/Game/Folder)Variant/BP_A.BP_A_C\"\n")
		TEXT("Bracket=\"/Game/Dir]2/BP_B.BP_B_C\"\n")
		TEXT("Brace=\"/Game/Set}One/BP_C.BP_C_C\"\n")
		TEXT("Struct=(Map=/Game/Maps/Arena)\n")
		TEXT("QuotedStruct=(Map=\"/Game/Maps/Arena\")\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}
	const TArray<CortexReflectConfigScan::FConfigReference> Refs = RefsFromFile(Fixture.IniName);
	if (!TestEqual(TEXT("one reference per line"), Refs.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("quoted unmatched )"), Refs[0].Path, FString(TEXT("/Game/Folder)Variant/BP_A.BP_A_C")));
	TestEqual(TEXT("quoted unmatched ]"), Refs[1].Path, FString(TEXT("/Game/Dir]2/BP_B.BP_B_C")));
	TestEqual(TEXT("quoted unmatched }"), Refs[2].Path, FString(TEXT("/Game/Set}One/BP_C.BP_C_C")));
	TestEqual(TEXT("unquoted struct closer still ends the token"), Refs[3].Path, FString(TEXT("/Game/Maps/Arena")));
	TestEqual(TEXT("quoted path in a struct ends at the quote"), Refs[4].Path, FString(TEXT("/Game/Maps/Arena")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectConfigEscapedQuotesTest,
	"Cortex.Reflect.Config.EscapedQuotes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectConfigEscapedQuotesTest::RunTest(const FString& Parameters)
{
	// A backslash-escaped quote does not close or open a quoted string.
	FTempConfigFixture Fixture(
		TEXT("[/Script/Test.Settings]\n")
		TEXT("Rule=(Label=\"He said \\\"Hi\",Map=/Game/Maps/Arena)\n")
		TEXT("Val=\"say \\\" then /Game/Maps)Alt/Arena\"\n"));
	if (!TestTrue(TEXT("fixture ini written"), Fixture.bWritten))
	{
		return false;
	}
	const TArray<CortexReflectConfigScan::FConfigReference> Refs = RefsFromFile(Fixture.IniName);
	if (!TestEqual(TEXT("one reference per line"), Refs.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("escaped quote in an earlier field leaves the unquoted path correct"), Refs[0].Path, FString(TEXT("/Game/Maps/Arena")));
	TestEqual(TEXT("escaped quote before a path keeps the quoted context"), Refs[1].Path, FString(TEXT("/Game/Maps)Alt/Arena")));
	return true;
}
