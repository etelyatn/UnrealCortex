// blueprint.compare_snapshots diffs two snapshot files by section and record identity. A member moved to a
// native parent with an equal value is reported `moved`; mismatched format versions and sections present on one
// side only are refused; report_path writes the full report and omits the inline list.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPSnapshotTestSupport.h"
#include "CortexBPSnapshotTestTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	FCortexCommandResult CompareSnapshotFiles(const FString& Before, const FString& After, const FString* ReportPath = nullptr)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("before"), Before);
		Params->SetStringField(TEXT("after"), After);
		if (ReportPath)
		{
			Params->SetStringField(TEXT("report_path"), *ReportPath);
		}
		return Handler.Execute(TEXT("compare_snapshots"), Params);
	}

	bool WriteJsonFile(const FString& Path, const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return FFileHelper::SaveStringToFile(Text, *Path);
	}

	int32 CountOf(const TSharedPtr<FJsonObject>& Data, const TCHAR* Section, const TCHAR* Kind)
	{
		const TSharedPtr<FJsonObject>* Counts = nullptr;
		const TSharedPtr<FJsonObject>* SectionCounts = nullptr;
		double Value = -1.0;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("counts"), Counts)
			&& (*Counts)->TryGetObjectField(Section, SectionCounts))
		{
			(*SectionCounts)->TryGetNumberField(Kind, Value);
		}
		return static_cast<int32>(Value);
	}

	TSharedPtr<FJsonObject> FindDifference(const TArray<TSharedPtr<FJsonValue>>& Differences, const FString& Section, const FString& Key)
	{
		for (const TSharedPtr<FJsonValue>& Value : Differences)
		{
			const TSharedPtr<FJsonObject> Difference = Value->AsObject();
			if (Difference.IsValid() && Difference->GetStringField(TEXT("section")) == Section
				&& Difference->GetStringField(TEXT("key")) == Key)
			{
				return Difference;
			}
		}
		return nullptr;
	}

	/** Snapshot document holding only a `variables` section of RecordCount records. */
	TSharedRef<FJsonObject> MakeSyntheticSnapshot(const int32 RecordCount, const int32 FormatVersion = 1)
	{
		TArray<TSharedPtr<FJsonValue>> Records;
		for (int32 Index = 0; Index < RecordCount; ++Index)
		{
			TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("key"), FString::Printf(TEXT("Variable_%04d"), Index));
			Record->SetStringField(TEXT("type"), TEXT("int"));
			Record->SetStringField(TEXT("default"), FString::Printf(TEXT("a reasonably long default value number %d"), Index));
			Records.Add(MakeShared<FJsonValueObject>(Record));
		}
		TSharedRef<FJsonObject> Sections = MakeShared<FJsonObject>();
		Sections->SetArrayField(TEXT("variables"), Records);
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		Snapshot->SetNumberField(TEXT("format_version"), FormatVersion);
		Snapshot->SetStringField(TEXT("asset_path"), TEXT("/Temp/Synthetic"));
		Snapshot->SetStringField(TEXT("parent_class"), TEXT("/Script/Engine.Actor"));
		Snapshot->SetObjectField(TEXT("sections"), Sections);
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompareSnapshotsTest,
	"Cortex.Blueprint.CompareSnapshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCompareSnapshotsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPSnapshotTestSupport;

	const FString Suffix = NewSuffix();
	TStrongObjectPtr<UBlueprint> Actor(MakeBlueprint(FString::Printf(TEXT("CortexCompare_%s"), *Suffix), AActor::StaticClass()));
	if (!TestNotNull(TEXT("actor Blueprint created"), Actor.Get()))
	{
		return false;
	}

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("variables added"),
		FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("MovedValue"), IntType)
		&& FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("Untouched"), IntType)))
	{
		Retire(Actor.Get());
		return false;
	}
	for (FBPVariableDescription& Variable : Actor->NewVariables)
	{
		if (Variable.VarName == TEXT("MovedValue"))
		{
			Variable.DefaultValue = TEXT("7"); // equal to ACortexBPSnapshotMoveParent::MovedValue
		}
	}
	FKismetEditorUtilities::CompileBlueprint(Actor.Get());

	const FString BeforePath = OutputPath(TEXT("cmp_before"), Suffix);
	const FString BeforeAgainPath = OutputPath(TEXT("cmp_before_again"), Suffix);
	const FString AfterPath = OutputPath(TEXT("cmp_after"), Suffix);
	const FString ReportPath = OutputPath(TEXT("cmp_report"), Suffix);
	TestTrue(TEXT("before export"), Export(Actor->GetPathName(), BeforePath).bSuccess);
	TestTrue(TEXT("second before export"), Export(Actor->GetPathName(), BeforeAgainPath).bSuccess);

	// Identical snapshots: zero differences.
	{
		const FCortexCommandResult Same = CompareSnapshotFiles(BeforePath, BeforeAgainPath);
		if (TestTrue(*FString::Printf(TEXT("identical compare succeeds (%s: %s)"), *Same.ErrorCode, *Same.ErrorMessage),
			Same.bSuccess && Same.Data.IsValid()))
		{
			TestTrue(TEXT("identical flag"), Same.Data->GetBoolField(TEXT("identical")));
			TestEqual(TEXT("zero differences"), static_cast<int32>(Same.Data->GetNumberField(TEXT("difference_count"))), 0);
			TestEqual(TEXT("differences list empty"), Same.Data->GetArrayField(TEXT("differences")).Num(), 0);
			const TSharedPtr<FJsonObject>* Totals = nullptr;
			if (TestTrue(TEXT("totals object"), Same.Data->TryGetObjectField(TEXT("totals"), Totals)))
			{
				for (const TCHAR* Kind : {TEXT("added"), TEXT("removed"), TEXT("changed"), TEXT("moved")})
				{
					TestEqual(*FString::Printf(TEXT("identical total %s"), Kind),
						static_cast<int32>((*Totals)->GetNumberField(Kind)), 0);
				}
			}
		}
	}

	// Mutate: add a variable, drop MovedValue and reparent to a native parent that declares it with the same
	// value, then change one CDO value.
	FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("Added"), IntType);
	FBlueprintEditorUtils::RemoveMemberVariable(Actor.Get(), TEXT("MovedValue"));
	Actor->ParentClass = ACortexBPSnapshotMoveParent::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Actor.Get());
	FBlueprintEditorUtils::MarkBlueprintAsModified(Actor.Get());
	FKismetEditorUtilities::CompileBlueprint(Actor.Get());
	UClass* GeneratedClass = Actor->GeneratedClass;
	if (!TestNotNull(TEXT("generated class after reparent"), GeneratedClass))
	{
		Retire(Actor.Get());
		return false;
	}
	GeneratedClass->GetDefaultObject<AActor>()->InitialLifeSpan = 5.0f;
	TestTrue(TEXT("after export"), Export(Actor->GetPathName(), AfterPath).bSuccess);

	const FCortexCommandResult Result = CompareSnapshotFiles(BeforePath, AfterPath);
	if (TestTrue(*FString::Printf(TEXT("compare succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage),
		Result.bSuccess && Result.Data.IsValid()))
	{
		// variables: Added is new, MovedValue left the Blueprint. A variable also appears in the CDO, so the
		// new variable is additionally an added CDO property.
		TestEqual(TEXT("variables added"), CountOf(Result.Data, TEXT("variables"), TEXT("added")), 1);
		TestEqual(TEXT("variables removed"), CountOf(Result.Data, TEXT("variables"), TEXT("removed")), 1);
		TestEqual(TEXT("variables changed"), CountOf(Result.Data, TEXT("variables"), TEXT("changed")), 0);
		TestEqual(TEXT("variables moved"), CountOf(Result.Data, TEXT("variables"), TEXT("moved")), 0);
		TestEqual(TEXT("cdo added"), CountOf(Result.Data, TEXT("cdo"), TEXT("added")), 1);
		TestEqual(TEXT("cdo removed"), CountOf(Result.Data, TEXT("cdo"), TEXT("removed")), 0);
		TestEqual(TEXT("cdo changed"), CountOf(Result.Data, TEXT("cdo"), TEXT("changed")), 1);
		TestEqual(TEXT("cdo moved"), CountOf(Result.Data, TEXT("cdo"), TEXT("moved")), 1);
		for (const TCHAR* Quiet : {TEXT("components"), TEXT("widgets"), TEXT("bindings"), TEXT("animations"), TEXT("text"),
			TEXT("interfaces"), TEXT("graph_entries")})
		{
			for (const TCHAR* Kind : {TEXT("added"), TEXT("removed"), TEXT("changed"), TEXT("moved")})
			{
				TestEqual(*FString::Printf(TEXT("%s %s"), Quiet, Kind), CountOf(Result.Data, Quiet, Kind), 0);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>& Differences = Result.Data->GetArrayField(TEXT("differences"));
		TestEqual(TEXT("exactly five differences"), Differences.Num(), 5);
		TestEqual(TEXT("difference_count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("difference_count"))), 5);

		const TSharedPtr<FJsonObject> AddedVariable = FindDifference(Differences, TEXT("variables"), TEXT("Added"));
		if (TestTrue(TEXT("variables/Added"), AddedVariable.IsValid()))
		{
			TestEqual(TEXT("variables/Added kind"), AddedVariable->GetStringField(TEXT("kind")), FString(TEXT("added")));
			TestTrue(TEXT("added carries the after record"), AddedVariable->HasTypedField<EJson::Object>(TEXT("after")));
			TestFalse(TEXT("added carries no before record"), AddedVariable->HasField(TEXT("before")));
		}
		const TSharedPtr<FJsonObject> RemovedVariable = FindDifference(Differences, TEXT("variables"), TEXT("MovedValue"));
		if (TestTrue(TEXT("variables/MovedValue"), RemovedVariable.IsValid()))
		{
			TestEqual(TEXT("variables/MovedValue kind"), RemovedVariable->GetStringField(TEXT("kind")), FString(TEXT("removed")));
		}
		const TSharedPtr<FJsonObject> AddedCdo = FindDifference(Differences, TEXT("cdo"), TEXT("Added"));
		if (TestTrue(TEXT("cdo/Added"), AddedCdo.IsValid()))
		{
			TestEqual(TEXT("cdo/Added kind"), AddedCdo->GetStringField(TEXT("kind")), FString(TEXT("added")));
		}
		const TSharedPtr<FJsonObject> ChangedCdo = FindDifference(Differences, TEXT("cdo"), TEXT("InitialLifeSpan"));
		if (TestTrue(TEXT("cdo/InitialLifeSpan"), ChangedCdo.IsValid()))
		{
			TestEqual(TEXT("cdo/InitialLifeSpan kind"), ChangedCdo->GetStringField(TEXT("kind")), FString(TEXT("changed")));
			const TArray<TSharedPtr<FJsonValue>>& Fields = ChangedCdo->GetArrayField(TEXT("fields"));
			if (TestEqual(TEXT("one changed field"), Fields.Num(), 1))
			{
				TestEqual(TEXT("the value changed"), Fields[0]->AsString(), FString(TEXT("value")));
			}
			TestEqual(TEXT("before value"), static_cast<float>(ChangedCdo->GetObjectField(TEXT("before"))->GetNumberField(TEXT("value"))), 0.0f);
			TestEqual(TEXT("after value"), static_cast<float>(ChangedCdo->GetObjectField(TEXT("after"))->GetNumberField(TEXT("value"))), 5.0f);
		}
		const TSharedPtr<FJsonObject> MovedCdo = FindDifference(Differences, TEXT("cdo"), TEXT("MovedValue"));
		if (TestTrue(TEXT("cdo/MovedValue"), MovedCdo.IsValid()))
		{
			TestEqual(TEXT("cdo/MovedValue is moved, not changed"), MovedCdo->GetStringField(TEXT("kind")), FString(TEXT("moved")));
			TestEqual(TEXT("moved from the Blueprint"),
				MovedCdo->GetObjectField(TEXT("before"))->GetStringField(TEXT("declared_in")), GeneratedClass->GetPathName());
			TestEqual(TEXT("moved to the native parent"),
				MovedCdo->GetObjectField(TEXT("after"))->GetStringField(TEXT("declared_in")),
				ACortexBPSnapshotMoveParent::StaticClass()->GetPathName());
			TestEqual(TEXT("moved value is unchanged"),
				static_cast<int32>(MovedCdo->GetObjectField(TEXT("after"))->GetNumberField(TEXT("value"))), 7);
		}

		const TSharedPtr<FJsonObject>* ParentClass = nullptr;
		if (TestTrue(TEXT("parent_class summary"), Result.Data->TryGetObjectField(TEXT("parent_class"), ParentClass)))
		{
			TestEqual(TEXT("parent before"), (*ParentClass)->GetStringField(TEXT("before")), AActor::StaticClass()->GetPathName());
			TestEqual(TEXT("parent after"), (*ParentClass)->GetStringField(TEXT("after")),
				ACortexBPSnapshotMoveParent::StaticClass()->GetPathName());
		}
	}

	// report_path: full report on disk, no inline list.
	{
		const FCortexCommandResult Reported = CompareSnapshotFiles(BeforePath, AfterPath, &ReportPath);
		if (TestTrue(*FString::Printf(TEXT("report compare succeeds (%s: %s)"), *Reported.ErrorCode, *Reported.ErrorMessage),
			Reported.bSuccess && Reported.Data.IsValid()))
		{
			TestFalse(TEXT("inline differences omitted"), Reported.Data->HasField(TEXT("differences")));
			TestTrue(TEXT("report_path echoed"), Reported.Data->GetStringField(TEXT("report_path")).EndsWith(FString::Printf(TEXT("cmp_report_%s.json"), *Suffix)));
			TestEqual(TEXT("summary still counts every difference"),
				static_cast<int32>(Reported.Data->GetNumberField(TEXT("difference_count"))), 5);
		}
		const TSharedPtr<FJsonObject> Report = ReadJsonFile(ReportPath);
		if (TestTrue(TEXT("report file parses"), Report.IsValid()))
		{
			TestEqual(TEXT("report holds the full list"), Report->GetArrayField(TEXT("differences")).Num(), 5);
		}
	}

	// A section recorded on one side only is refused, never read as empty.
	{
		const FString SubsetPath = OutputPath(TEXT("cmp_subset"), Suffix);
		const TArray<FString> Subset = {TEXT("variables")};
		TestTrue(TEXT("subset export"), Export(Actor->GetPathName(), SubsetPath, &Subset).bSuccess);
		const FCortexCommandResult Missing = CompareSnapshotFiles(SubsetPath, AfterPath);
		TestFalse(TEXT("missing section refused"), Missing.bSuccess);
		TestEqual(TEXT("missing section code"), Missing.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("refusal names the section"), Missing.ErrorMessage.Contains(TEXT("'cdo'")));
		IFileManager::Get().Delete(*SubsetPath);
	}

	// format_version mismatch is refused.
	{
		const TSharedPtr<FJsonObject> Original = ReadJsonFile(AfterPath);
		if (TestTrue(TEXT("after snapshot reread"), Original.IsValid()))
		{
			Original->SetNumberField(TEXT("format_version"), 2);
			const FString FutureAfter = OutputPath(TEXT("cmp_future"), Suffix);
			TestTrue(TEXT("future snapshot written"), WriteJsonFile(FutureAfter, Original.ToSharedRef()));
			const FCortexCommandResult Mismatch = CompareSnapshotFiles(BeforePath, FutureAfter);
			TestFalse(TEXT("format_version mismatch refused"), Mismatch.bSuccess);
			TestEqual(TEXT("format_version mismatch code"), Mismatch.ErrorCode, CortexErrorCodes::InvalidField);
			TestTrue(TEXT("refusal names format_version"), Mismatch.ErrorMessage.Contains(TEXT("format_version")));
			IFileManager::Get().Delete(*FutureAfter);
		}
	}

	// Same file, malformed file and an out-of-roots report are refused.
	{
		const FCortexCommandResult SameFile = CompareSnapshotFiles(BeforePath, BeforePath);
		TestFalse(TEXT("same file refused"), SameFile.bSuccess);
		TestEqual(TEXT("same file code"), SameFile.ErrorCode, CortexErrorCodes::InvalidFilePath);

		const FString NotSnapshot = OutputPath(TEXT("cmp_notsnapshot"), Suffix);
		TestTrue(TEXT("non snapshot written"), FFileHelper::SaveStringToFile(TEXT("{\"hello\":1}"), *NotSnapshot));
		const FCortexCommandResult NotASnapshot = CompareSnapshotFiles(BeforePath, NotSnapshot);
		TestFalse(TEXT("non snapshot refused"), NotASnapshot.bSuccess);
		TestEqual(TEXT("non snapshot code"), NotASnapshot.ErrorCode, CortexErrorCodes::InvalidField);
		IFileManager::Get().Delete(*NotSnapshot);

		const FString Outside = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("Source"), FString::Printf(TEXT("CortexCompareOutside_%s.json"), *Suffix)));
		const FCortexCommandResult OutsideReport = CompareSnapshotFiles(BeforePath, AfterPath, &Outside);
		TestFalse(TEXT("report outside allowed roots refused"), OutsideReport.bSuccess);
		TestEqual(TEXT("outside report code"), OutsideReport.ErrorCode, CortexErrorCodes::InvalidFilePath);
		TestFalse(TEXT("nothing written outside allowed roots"), IFileManager::Get().FileExists(*Outside));
	}

	// An oversize inline report is refused with RESPONSE_TOO_LARGE; report_path carries it.
	{
		const FString BigBefore = OutputPath(TEXT("cmp_big_before"), Suffix);
		const FString BigAfter = OutputPath(TEXT("cmp_big_after"), Suffix);
		const FString BigReport = OutputPath(TEXT("cmp_big_report"), Suffix);
		TestTrue(TEXT("big before written"), WriteJsonFile(BigBefore, MakeSyntheticSnapshot(0)));
		TestTrue(TEXT("big after written"), WriteJsonFile(BigAfter, MakeSyntheticSnapshot(800)));
		const FCortexCommandResult Large = CompareSnapshotFiles(BigBefore, BigAfter);
		if (TestTrue(TEXT("oversize inline returns an envelope"), Large.bSuccess && Large.Data.IsValid()))
		{
			FString Error;
			Large.Data->TryGetStringField(TEXT("_error"), Error);
			TestEqual(TEXT("oversize inline reports RESPONSE_TOO_LARGE"), Error, FString(TEXT("RESPONSE_TOO_LARGE")));
			TestFalse(TEXT("oversize refusal carries no differences"), Large.Data->HasField(TEXT("differences")));
			TestEqual(TEXT("oversize refusal still counts"), CountOf(Large.Data, TEXT("variables"), TEXT("added")), 800);
		}
		const FCortexCommandResult LargeReported = CompareSnapshotFiles(BigBefore, BigAfter, &BigReport);
		TestTrue(TEXT("oversize compare succeeds with report_path"), LargeReported.bSuccess);
		const TSharedPtr<FJsonObject> LargeReport = ReadJsonFile(BigReport);
		if (TestTrue(TEXT("oversize report parses"), LargeReport.IsValid()))
		{
			TestEqual(TEXT("oversize report holds every difference"), LargeReport->GetArrayField(TEXT("differences")).Num(), 800);
		}
		IFileManager::Get().Delete(*BigBefore);
		IFileManager::Get().Delete(*BigAfter);
		IFileManager::Get().Delete(*BigReport);
	}

	IFileManager::Get().Delete(*BeforePath);
	IFileManager::Get().Delete(*BeforeAgainPath);
	IFileManager::Get().Delete(*AfterPath);
	IFileManager::Get().Delete(*ReportPath);
	Retire(Actor.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompareSnapshotsEdgeCasesTest,
	"Cortex.Blueprint.CompareSnapshotsEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCompareSnapshotsEdgeCasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPSnapshotTestSupport;

	const FString Suffix = NewSuffix();

	// A member moved from a Blueprint double to a native float with the same value is `moved`, never `changed`,
	// and the type change is recorded on the difference. 0.1 differs between double and float only past float precision.
	{
		TStrongObjectPtr<UBlueprint> Actor(MakeBlueprint(FString::Printf(TEXT("CortexCompareFloat_%s"), *Suffix), AActor::StaticClass()));
		if (!TestNotNull(TEXT("actor Blueprint created"), Actor.Get()))
		{
			return false;
		}
		FEdGraphPinType DoubleType;
		DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
		DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		if (!TestTrue(TEXT("double variable added"), FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("MovedFloat"), DoubleType)))
		{
			Retire(Actor.Get());
			return false;
		}
		for (FBPVariableDescription& Variable : Actor->NewVariables)
		{
			if (Variable.VarName == TEXT("MovedFloat"))
			{
				Variable.DefaultValue = TEXT("0.1");
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Actor.Get());
		const FString BeforePath = OutputPath(TEXT("edge_before"), Suffix);
		const FString AfterPath = OutputPath(TEXT("edge_after"), Suffix);
		TestTrue(TEXT("before export"), Export(Actor->GetPathName(), BeforePath).bSuccess);

		// The "after" side is a Blueprint on the native parent that declares the float (reparenting the live Blueprint
		// would make the engine reinstance the double CDO into a float and warn about the conversion).
		TStrongObjectPtr<UBlueprint> Native(MakeBlueprint(
			FString::Printf(TEXT("CortexCompareFloatNative_%s"), *Suffix), ACortexBPSnapshotFloatMoveParent::StaticClass()));
		if (!TestNotNull(TEXT("native-parent Blueprint created"), Native.Get()))
		{
			Retire(Actor.Get());
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(Native.Get());
		TestTrue(TEXT("after export"), Export(Native->GetPathName(), AfterPath).bSuccess);

		const FCortexCommandResult Result = CompareSnapshotFiles(BeforePath, AfterPath);
		if (TestTrue(*FString::Printf(TEXT("compare succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage),
			Result.bSuccess && Result.Data.IsValid()))
		{
			const TSharedPtr<FJsonObject> Moved = FindDifference(Result.Data->GetArrayField(TEXT("differences")), TEXT("cdo"), TEXT("MovedFloat"));
			if (TestTrue(TEXT("cdo/MovedFloat difference"), Moved.IsValid()))
			{
				TestEqual(TEXT("double -> float with an equal value is moved, not changed"),
					Moved->GetStringField(TEXT("kind")), FString(TEXT("moved")));
				const TSharedPtr<FJsonObject>* TypeChange = nullptr;
				if (TestTrue(TEXT("the type change is recorded"), Moved->TryGetObjectField(TEXT("type_change"), TypeChange)))
				{
					TestEqual(TEXT("type before"), (*TypeChange)->GetStringField(TEXT("before")), FString(TEXT("double")));
					TestEqual(TEXT("type after"), (*TypeChange)->GetStringField(TEXT("after")), FString(TEXT("float")));
				}
			}
		}
		IFileManager::Get().Delete(*BeforePath);
		IFileManager::Get().Delete(*AfterPath);
		Retire(Actor.Get());
		Retire(Native.Get());
	}

	// Two snapshots that record no section at all compare nothing and are refused, never reported identical.
	{
		const auto WriteSnapshot = [&Suffix](const TCHAR* Label, const TSharedRef<FJsonObject>& Sections) -> FString
		{
			TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
			Snapshot->SetNumberField(TEXT("format_version"), 1);
			Snapshot->SetStringField(TEXT("asset_path"), TEXT("/Temp/Synthetic"));
			Snapshot->SetStringField(TEXT("parent_class"), TEXT("/Script/Engine.Actor"));
			Snapshot->SetObjectField(TEXT("sections"), Sections);
			const FString Path = OutputPath(Label, Suffix);
			WriteJsonFile(Path, Snapshot);
			return Path;
		};
		const FString EmptyA = WriteSnapshot(TEXT("edge_empty_a"), MakeShared<FJsonObject>());
		const FString EmptyB = WriteSnapshot(TEXT("edge_empty_b"), MakeShared<FJsonObject>());
		const FCortexCommandResult Empty = CompareSnapshotFiles(EmptyA, EmptyB);
		TestFalse(TEXT("an empty compared set is refused"), Empty.bSuccess);
		TestEqual(TEXT("empty compared set code"), Empty.ErrorCode, CortexErrorCodes::InvalidField);
		IFileManager::Get().Delete(*EmptyA);
		IFileManager::Get().Delete(*EmptyB);

		// A duplicate record key would be silently overwritten; it is refused instead.
		const TSharedRef<FJsonObject> Duplicated = MakeSyntheticSnapshot(1);
		TArray<TSharedPtr<FJsonValue>> Records = Duplicated->GetObjectField(TEXT("sections"))->GetArrayField(TEXT("variables"));
		TSharedPtr<FJsonObject> Twin = MakeShared<FJsonObject>();
		Twin->SetStringField(TEXT("key"), TEXT("Variable_0000"));
		Twin->SetStringField(TEXT("type"), TEXT("float"));
		Records.Add(MakeShared<FJsonValueObject>(Twin));
		Duplicated->GetObjectField(TEXT("sections"))->SetArrayField(TEXT("variables"), Records);
		const FString DuplicatePath = OutputPath(TEXT("edge_duplicate"), Suffix);
		const FString CleanPath = OutputPath(TEXT("edge_clean"), Suffix);
		TestTrue(TEXT("duplicate snapshot written"), WriteJsonFile(DuplicatePath, Duplicated));
		TestTrue(TEXT("clean snapshot written"), WriteJsonFile(CleanPath, MakeSyntheticSnapshot(1)));
		const FCortexCommandResult Duplicate = CompareSnapshotFiles(CleanPath, DuplicatePath);
		TestFalse(TEXT("a duplicate record key is refused"), Duplicate.bSuccess);
		TestEqual(TEXT("duplicate key code"), Duplicate.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("refusal names the duplicate key"), Duplicate.ErrorMessage.Contains(TEXT("Variable_0000")));
		IFileManager::Get().Delete(*DuplicatePath);
		IFileManager::Get().Delete(*CleanPath);
	}
	return true;
}
