// reflect.find_interface_implementers (#277 item 7): the `referencers` split and the optional `implementer` narrowing.

#include "Misc/AutomationTest.h"

#include "Algo/Sort.h"
#include "CortexReflectIssue122TestSupport.h"
#include "CortexReflectIssue122TestTypes.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CortexIssue277ReferencersTest
{
	using namespace CortexIssue122ReflectTest;

	TSharedPtr<FJsonObject> RequestParams(const FString& Interface, const FString& Prefix)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("interface"), Interface);
		Result->SetArrayField(TEXT("path_prefixes"), JsonValues({Prefix}));
		return Result;
	}

	/** A compiled BPI: GetValue (double output, function-style) and Notify (void, event-style). */
	UBlueprint* MakeInterface(FFixture& Fixture, const FString& Name)
	{
		UBlueprint* Interface = Fixture.MakeBlueprint(Name, UInterface::StaticClass(), BPTYPE_Interface);
		if (!Interface || !FFixture::AddFunctionGraphTo(Interface, TEXT("GetValue"), true) || !FFixture::AddFunctionGraphTo(Interface, TEXT("Notify"), false))
		{
			return nullptr;
		}
		FKismetEditorUtilities::CompileBlueprint(Interface);
		return Interface;
	}

	bool PathLess(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	}

	TArray<FString> Sorted(TArray<FString> Paths)
	{
		Algo::Sort(Paths, PathLess);
		return Paths;
	}

	/** One bucket of `referencers` (an array of asset_path strings); bOutPresent is false when the object or the bucket is missing. */
	TArray<FString> Bucket(const TSharedPtr<FJsonObject>& Data, const TCHAR* Name, bool& bOutPresent)
	{
		const TSharedPtr<FJsonObject>* Referencers = nullptr;
		bOutPresent = Data.IsValid() && Data->TryGetObjectField(TEXT("referencers"), Referencers) && Referencers && Referencers->IsValid()
			&& (*Referencers)->HasTypedField<EJson::Array>(Name);
		return bOutPresent ? Strings(*Referencers, Name) : TArray<FString>();
	}

	/** Asserts the three buckets exactly (as sets) and that each one is sorted by asset path (case-sensitive). */
	void ExpectReferencers(
		FAutomationTestBase& Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& Data,
		const TArray<FString>& Implements,
		const TArray<FString>& CallsOrMessages,
		const TArray<FString>& Other)
	{
		const struct
		{
			const TCHAR* Name;
			const TArray<FString>* Expected;
		} Buckets[] = {{TEXT("implements"), &Implements}, {TEXT("calls_or_messages"), &CallsOrMessages}, {TEXT("other"), &Other}};
		for (const auto& Expected : Buckets)
		{
			bool bPresent = false;
			const TArray<FString> Actual = Bucket(Data, Expected.Name, bPresent);
			if (!Test.TestTrue(*FString::Printf(TEXT("%sreferencers.%s is an array"), *Label, Expected.Name), bPresent))
			{
				continue;
			}
			Test.TestEqual(*FString::Printf(TEXT("%sreferencers.%s"), *Label, Expected.Name),
				FString::Join(Actual, TEXT(",")), FString::Join(Sorted(*Expected.Expected), TEXT(",")));
			Test.TestEqual(*FString::Printf(TEXT("%sreferencers.%s is sorted by asset path"), *Label, Expected.Name),
				FString::Join(Actual, TEXT(",")), FString::Join(Sorted(Actual), TEXT(",")));
		}
	}

	/** The interface function entry (top-level `functions`) named Name. */
	TSharedPtr<FJsonObject> InterfaceFunction(const TSharedPtr<FJsonObject>& Data, const FString& Name)
	{
		for (const TSharedPtr<FJsonObject>& Function : Objects(Data, TEXT("functions")))
		{
			if (Function->GetStringField(TEXT("name")) == Name)
			{
				return Function;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceReferencersTest,
	"Cortex.Reflect.FindInterfaceReferencers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceReferencersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue277ReferencersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Impl = Fixture.MakeBlueprint(TEXT("BP_Impl"), AActor::StaticClass());
	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_Caller"), AActor::StaticClass());
	UBlueprint* Both = Fixture.MakeBlueprint(TEXT("BP_Both"), AActor::StaticClass());
	UBlueprint* Holder = Fixture.MakeBlueprint(TEXT("BP_Holder"), AActor::StaticClass());
	UBlueprint* Plain = Fixture.MakeBlueprint(TEXT("BP_Plain"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Impl"), Impl) || !TestNotNull(TEXT("Caller"), Caller) || !TestNotNull(TEXT("Both"), Both)
		|| !TestNotNull(TEXT("Holder"), Holder) || !TestNotNull(TEXT("Plain"), Plain))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	const FTopLevelAssetPath InterfacePath(InterfaceClass);

	// Impl implements only; its Notify is an event with a body, its GetValue the function graph made by ImplementNewInterface.
	TestTrue(TEXT("Impl implements"), FBlueprintEditorUtils::ImplementNewInterface(Impl, InterfacePath));
	FKismetEditorUtilities::CompileBlueprint(Impl);
	// Child inherits the interface from Impl; it holds no reference to the interface package itself.
	UBlueprint* Child = Fixture.MakeBlueprint(TEXT("BP_Child"), Impl->GeneratedClass);
	if (!TestNotNull(TEXT("Child"), Child))
	{
		return false;
	}
	FFixture::AddInterfaceEvent(FFixture::EventGraph(Impl), TEXT("Notify"), InterfaceClass, true);

	// Caller messages the interface and does not implement it.
	FFixture::AddMessage(FFixture::EventGraph(Caller), TEXT("GetValue"), InterfaceClass);

	// Both implements the interface (no event bodies) and messages it: it belongs in two buckets.
	TestTrue(TEXT("Both implements"), FBlueprintEditorUtils::ImplementNewInterface(Both, InterfacePath));
	FFixture::AddMessage(FFixture::EventGraph(Both), TEXT("Notify"), InterfaceClass);

	// Holder only holds a variable typed by the interface: a hard referencer with neither implementer nor caller evidence.
	FEdGraphPinType InterfaceVariable;
	InterfaceVariable.PinCategory = UEdGraphSchema_K2::PC_Interface;
	InterfaceVariable.PinSubCategoryObject = InterfaceClass;
	TestTrue(TEXT("Holder variable added"), FBlueprintEditorUtils::AddMemberVariable(Holder, TEXT("Target"), InterfaceVariable));
	FKismetEditorUtilities::CompileBlueprint(Holder);

	for (UBlueprint* Asset : {Interface, Impl, Child, Caller, Both, Holder, Plain})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	// Fixture precondition, read from the registry itself: the hard referencers of the interface package.
	TSet<FName> RegistryReferencers;
	{
		TArray<FName> Names;
		IAssetRegistry::GetChecked().GetReferencers(Interface->GetOutermost()->GetFName(), Names,
			UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));
		for (const FName Name : Names)
		{
			if (Name.ToString().StartsWith(Fixture.Dir))
			{
				RegistryReferencers.Add(Name);
			}
		}
	}
	for (const UBlueprint* Referencer : {Impl, Caller, Both, Holder})
	{
		TestTrue(*FString::Printf(TEXT("Fixture: %s hard-references the interface package"), *Referencer->GetName()),
			RegistryReferencers.Contains(Referencer->GetOutermost()->GetFName()));
	}
	for (const UBlueprint* NotReferencer : {Child, Plain})
	{
		TestFalse(*FString::Printf(TEXT("Fixture: %s does not reference the interface package"), *NotReferencer->GetName()),
			RegistryReferencers.Contains(NotReferencer->GetOutermost()->GetFName()));
	}

	const FString InterfaceAsset = Interface->GetPathName();
	const TArray<FString> ExpectedImplements = {Impl->GetPathName(), Both->GetPathName()};
	const TArray<FString> ExpectedCalls = {Caller->GetPathName(), Both->GetPathName()};
	const TArray<FString> ExpectedOther = {Holder->GetPathName()};

	// Default request: implementer tag -> implements, resolved caller sites -> calls_or_messages, the rest -> other.
	// BP_Both is in both implements and calls_or_messages; Plain and the inherited Child are in no bucket (not hard referencers).
	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(InterfaceAsset, Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	ExpectReferencers(*this, TEXT("[default] "), Result.Data, ExpectedImplements, ExpectedCalls, ExpectedOther);
	TestFalse(TEXT("[default] No implementer_handles without the implementer parameter"),
		InterfaceFunction(Result.Data, TEXT("GetValue")).IsValid() && InterfaceFunction(Result.Data, TEXT("GetValue"))->HasField(TEXT("implementer_handles")));

	// Every in-scope hard referencer of the interface package appears in at least one bucket, and nothing else does.
	{
		TSet<FName> Reported;
		for (const TCHAR* Name : {TEXT("implements"), TEXT("calls_or_messages"), TEXT("other")})
		{
			bool bPresent = false;
			for (const FString& Path : Bucket(Result.Data, Name, bPresent))
			{
				Reported.Add(FName(*FSoftObjectPath(Path).GetLongPackageName()));
			}
		}
		TestTrue(TEXT("[default] Every registry hard referencer is classified"), Reported.Includes(RegistryReferencers));
		TestTrue(TEXT("[default] ... and nothing but the hard referencers is reported"), RegistryReferencers.Includes(Reported));
	}

	// The callers list is unchanged by the split.
	TestEqual(TEXT("[default] caller_count (Caller GetValue, Both Notify)"), static_cast<int32>(Result.Data->GetNumberField(TEXT("caller_count"))), 2);

	// The function filter narrows `callers` only: referencers classify by every caller site, whatever function it calls.
	TSharedPtr<FJsonObject> Filtered = RequestParams(InterfaceAsset, Fixture.Dir);
	Filtered->SetStringField(TEXT("function"), TEXT("Notify"));
	const FCortexCommandResult FilteredResult = Run(TEXT("find_interface_implementers"), Filtered);
	if (TestTrue(TEXT("[function] Filtered request succeeds"), FilteredResult.bSuccess))
	{
		TArray<FString> CallerAssets;
		for (const TSharedPtr<FJsonObject>& Site : Objects(FilteredResult.Data, TEXT("callers")))
		{
			CallerAssets.Add(Site->GetStringField(TEXT("asset_path")));
		}
		TestEqual(TEXT("[function] Only the Notify caller remains in callers"), FString::Join(CallerAssets, TEXT(",")), Both->GetPathName());
		ExpectReferencers(*this, TEXT("[function] "), FilteredResult.Data, ExpectedImplements, ExpectedCalls, ExpectedOther);
	}

	// include_callers:false skips the caller scan, so the split (which needs it) is omitted rather than reported wrong.
	TSharedPtr<FJsonObject> WithoutCallers = RequestParams(InterfaceAsset, Fixture.Dir);
	WithoutCallers->SetBoolField(TEXT("include_callers"), false);
	const FCortexCommandResult WithoutResult = Run(TEXT("find_interface_implementers"), WithoutCallers);
	if (TestTrue(TEXT("[no callers] include_callers:false succeeds"), WithoutResult.bSuccess))
	{
		TestFalse(TEXT("[no callers] No referencers object"), WithoutResult.Data->HasField(TEXT("referencers")));
	}

	// Referencers honour path_prefixes: a folder holding none of the assets reports empty buckets (present, not missing).
	const FCortexCommandResult ElsewhereResult = Run(TEXT("find_interface_implementers"), RequestParams(InterfaceAsset, Fixture.Dir + TEXT("/NoSuchFolder")));
	if (TestTrue(TEXT("[prefix] A prefix matching nothing succeeds"), ElsewhereResult.bSuccess))
	{
		ExpectReferencers(*this, TEXT("[prefix] "), ElsewhereResult.Data, {}, {}, {});
	}

	// out_path: the report file holds the referencers.
	TSharedPtr<FJsonObject> ToFile = RequestParams(InterfaceAsset, Fixture.Dir);
	const FString ReportPath = Fixture.ReportPath(TEXT("Referencers"));
	ToFile->SetStringField(TEXT("out_path"), ReportPath);
	const FCortexCommandResult FileResult = Run(TEXT("find_interface_implementers"), ToFile);
	if (TestTrue(TEXT("[out_path] out_path request succeeds"), FileResult.bSuccess))
	{
		// The inline summary carries counts only (the buckets could exceed the response budget); the file keeps every path.
		const TSharedPtr<FJsonObject>* SummaryReferencers = nullptr;
		if (TestTrue(TEXT("[out_path] The summary has a referencers object"), FileResult.Data->TryGetObjectField(TEXT("referencers"), SummaryReferencers) && SummaryReferencers && SummaryReferencers->IsValid()))
		{
			const struct
			{
				const TCHAR* Name;
				int32 Expected;
			} Counts[] = {{TEXT("implements_count"), ExpectedImplements.Num()}, {TEXT("calls_or_messages_count"), ExpectedCalls.Num()}, {TEXT("other_count"), ExpectedOther.Num()}};
			for (const auto& Count : Counts)
			{
				if (TestTrue(*FString::Printf(TEXT("[out_path] summary referencers.%s is a number"), Count.Name), (*SummaryReferencers)->HasTypedField<EJson::Number>(Count.Name)))
				{
					TestEqual(*FString::Printf(TEXT("[out_path] summary referencers.%s"), Count.Name), static_cast<int32>((*SummaryReferencers)->GetNumberField(Count.Name)), Count.Expected);
				}
			}
			for (const TCHAR* Bucket : {TEXT("implements"), TEXT("calls_or_messages"), TEXT("other")})
			{
				TestFalse(*FString::Printf(TEXT("[out_path] summary referencers has no %s array"), Bucket), (*SummaryReferencers)->HasField(Bucket));
			}
		}
		FString Text;
		TSharedPtr<FJsonObject> Written;
		TestTrue(TEXT("[out_path] The report file is written"), FFileHelper::LoadFileToString(Text, *ReportPath));
		if (TestTrue(TEXT("[out_path] ... and parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Written) && Written.IsValid()))
		{
			ExpectReferencers(*this, TEXT("[out_path] file "), Written, ExpectedImplements, ExpectedCalls, ExpectedOther);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceImplementerNarrowingTest,
	"Cortex.Reflect.FindInterfaceImplementerNarrowing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceImplementerNarrowingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue277ReferencersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Impl = Fixture.MakeBlueprint(TEXT("BP_Impl"), AActor::StaticClass());
	UBlueprint* Both = Fixture.MakeBlueprint(TEXT("BP_Both"), AActor::StaticClass());
	UBlueprint* Plain = Fixture.MakeBlueprint(TEXT("BP_Plain"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Impl"), Impl) || !TestNotNull(TEXT("Both"), Both) || !TestNotNull(TEXT("Plain"), Plain))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	const FTopLevelAssetPath InterfacePath(InterfaceClass);
	TestTrue(TEXT("Impl implements"), FBlueprintEditorUtils::ImplementNewInterface(Impl, InterfacePath));
	TestTrue(TEXT("Both implements"), FBlueprintEditorUtils::ImplementNewInterface(Both, InterfacePath));
	FKismetEditorUtilities::CompileBlueprint(Impl);
	UBlueprint* Child = Fixture.MakeBlueprint(TEXT("BP_Child"), Impl->GeneratedClass);
	if (!TestNotNull(TEXT("Child"), Child))
	{
		return false;
	}
	// Impl handles Notify with an event that has a body; Both leaves Notify unimplemented. Both handle GetValue with a function graph.
	FFixture::AddInterfaceEvent(FFixture::EventGraph(Impl), TEXT("Notify"), InterfaceClass, true);
	// A call site so the global callers list is non-empty.
	FFixture::AddMessage(FFixture::EventGraph(Plain), TEXT("GetValue"), InterfaceClass);

	for (UBlueprint* Asset : {Interface, Impl, Both, Child, Plain})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	const FString InterfaceAsset = Interface->GetPathName();
	const FCortexCommandResult Unnarrowed = Run(TEXT("find_interface_implementers"), RequestParams(InterfaceAsset, Fixture.Dir));
	if (!TestTrue(TEXT("The unnarrowed request succeeds"), Unnarrowed.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Unnarrowed: Impl, Both and the inherited Child"), static_cast<int32>(Unnarrowed.Data->GetNumberField(TEXT("implementer_count"))), 3);

	const auto NarrowedTo = [&](const FString& Asset)
	{
		TSharedPtr<FJsonObject> Request = RequestParams(InterfaceAsset, Fixture.Dir);
		Request->SetStringField(TEXT("implementer"), Asset);
		return Run(TEXT("find_interface_implementers"), Request);
	};
	const auto HandlesOf = [](const FCortexCommandResult& Narrowed, const TCHAR* Function, bool& bOutPresent)
	{
		const TSharedPtr<FJsonObject> Entry = InterfaceFunction(Narrowed.Data, Function);
		bOutPresent = Entry.IsValid() && Entry->HasTypedField<EJson::Boolean>(TEXT("implementer_handles"));
		return bOutPresent && Entry->GetBoolField(TEXT("implementer_handles"));
	};

	// A direct implementer with a Notify event: implementers is that asset alone and both functions are handled.
	const FCortexCommandResult ImplResult = NarrowedTo(Impl->GetPathName());
	if (TestTrue(TEXT("[Impl] Narrowed request succeeds"), ImplResult.bSuccess))
	{
		const TArray<TSharedPtr<FJsonObject>> Implementers = Objects(ImplResult.Data, TEXT("implementers"));
		if (TestEqual(TEXT("[Impl] implementers holds one entry"), Implementers.Num(), 1))
		{
			TestEqual(TEXT("[Impl] ... the requested asset"), Implementers[0]->GetStringField(TEXT("asset_path")), Impl->GetPathName());
		}
		TestEqual(TEXT("[Impl] implementer_count"), static_cast<int32>(ImplResult.Data->GetNumberField(TEXT("implementer_count"))), 1);
		bool bPresent = false;
		TestTrue(TEXT("[Impl] GetValue (function graph) is handled"), HandlesOf(ImplResult, TEXT("GetValue"), bPresent));
		TestTrue(TEXT("[Impl] GetValue reports implementer_handles"), bPresent);
		TestTrue(TEXT("[Impl] Notify (event) is handled"), HandlesOf(ImplResult, TEXT("Notify"), bPresent));
		TestTrue(TEXT("[Impl] Notify reports implementer_handles"), bPresent);
	}

	// A direct implementer with no Notify event: that function is reported as not handled.
	const FCortexCommandResult BothResult = NarrowedTo(Both->GetPathName());
	if (TestTrue(TEXT("[Both] Narrowed request succeeds"), BothResult.bSuccess))
	{
		TestEqual(TEXT("[Both] implementers holds one entry"), Objects(BothResult.Data, TEXT("implementers")).Num(), 1);
		bool bPresent = false;
		TestTrue(TEXT("[Both] GetValue (function graph) is handled"), HandlesOf(BothResult, TEXT("GetValue"), bPresent));
		TestTrue(TEXT("[Both] GetValue reports implementer_handles"), bPresent);
		TestFalse(TEXT("[Both] Notify (no event) is not handled"), HandlesOf(BothResult, TEXT("Notify"), bPresent));
		TestTrue(TEXT("[Both] Notify reports implementer_handles false, not a missing field"), bPresent);
	}

	// The callers list and referencers stay global: dispatch targets are runtime values, so narrowing does not filter them.
	if (ImplResult.bSuccess)
	{
		TestEqual(TEXT("[Impl] callers stay global"), Objects(ImplResult.Data, TEXT("callers")).Num(), Objects(Unnarrowed.Data, TEXT("callers")).Num());
		TestEqual(TEXT("[Impl] caller_count stays global"),
			static_cast<int32>(ImplResult.Data->GetNumberField(TEXT("caller_count"))), static_cast<int32>(Unnarrowed.Data->GetNumberField(TEXT("caller_count"))));
		ExpectReferencers(*this, TEXT("[Impl] "), ImplResult.Data,
			{Impl->GetPathName(), Both->GetPathName()}, {Plain->GetPathName()}, {});
	}

	// An inherited implementer is narrowed to as well; implementer_handles is the effective implementation through its via chain.
	const FCortexCommandResult ChildResult = NarrowedTo(Child->GetPathName());
	if (TestTrue(TEXT("[Child] Narrowed request succeeds"), ChildResult.bSuccess))
	{
		const TArray<TSharedPtr<FJsonObject>> Implementers = Objects(ChildResult.Data, TEXT("implementers"));
		if (TestEqual(TEXT("[Child] implementers holds one entry"), Implementers.Num(), 1))
		{
			TestEqual(TEXT("[Child] ... the inherited implementer"), Implementers[0]->GetStringField(TEXT("asset_path")), Child->GetPathName());
			TestEqual(TEXT("[Child] ... via Impl"), Implementers[0]->GetStringField(TEXT("via")), Impl->GetPathName());
		}
		bool bPresent = false;
		TestTrue(TEXT("[Child] GetValue (Impl's function graph) is handled"), HandlesOf(ChildResult, TEXT("GetValue"), bPresent));
		TestTrue(TEXT("[Child] GetValue reports implementer_handles"), bPresent);
		TestTrue(TEXT("[Child] Notify (Impl's event) is handled"), HandlesOf(ChildResult, TEXT("Notify"), bPresent));
		TestTrue(TEXT("[Child] Notify reports implementer_handles"), bPresent);
	}

	// An asset that does not implement the interface narrows to nothing (an answer, not an error).
	const FCortexCommandResult PlainResult = NarrowedTo(Plain->GetPathName());
	if (TestTrue(TEXT("[Plain] A non-implementing asset succeeds"), PlainResult.bSuccess))
	{
		TestEqual(TEXT("[Plain] No implementers"), Objects(PlainResult.Data, TEXT("implementers")).Num(), 0);
		TestEqual(TEXT("[Plain] implementer_count"), static_cast<int32>(PlainResult.Data->GetNumberField(TEXT("implementer_count"))), 0);
		const TSharedPtr<FJsonObject> GetValue = InterfaceFunction(PlainResult.Data, TEXT("GetValue"));
		TestFalse(TEXT("[Plain] No implementer_handles"), GetValue.IsValid() && GetValue->HasField(TEXT("implementer_handles")));
	}

	// Together with the function filter, only that function's entry of the implementer remains.
	TSharedPtr<FJsonObject> WithFunction = RequestParams(InterfaceAsset, Fixture.Dir);
	WithFunction->SetStringField(TEXT("implementer"), Impl->GetPathName());
	WithFunction->SetStringField(TEXT("function"), TEXT("Notify"));
	const FCortexCommandResult FunctionResult = Run(TEXT("find_interface_implementers"), WithFunction);
	if (TestTrue(TEXT("[function] implementer with function succeeds"), FunctionResult.bSuccess))
	{
		const TArray<TSharedPtr<FJsonObject>> Implementers = Objects(FunctionResult.Data, TEXT("implementers"));
		if (TestEqual(TEXT("[function] One implementer"), Implementers.Num(), 1))
		{
			TestEqual(TEXT("[function] ... with only Notify"), Objects(Implementers[0], TEXT("functions")).Num(), 1);
		}
	}

	// Refusals: implementer must be a non-empty string.
	TSharedPtr<FJsonObject> Empty = RequestParams(InterfaceAsset, Fixture.Dir);
	Empty->SetStringField(TEXT("implementer"), TEXT(""));
	const FCortexCommandResult EmptyResult = Run(TEXT("find_interface_implementers"), Empty);
	TestFalse(TEXT("[refusal] An empty implementer is refused"), EmptyResult.bSuccess);
	TestEqual(TEXT("[refusal] ... as INVALID_FIELD"), EmptyResult.ErrorCode, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> WrongType = RequestParams(InterfaceAsset, Fixture.Dir);
	WrongType->SetNumberField(TEXT("implementer"), 3);
	const FCortexCommandResult WrongTypeResult = Run(TEXT("find_interface_implementers"), WrongType);
	TestFalse(TEXT("[refusal] A non-string implementer is refused"), WrongTypeResult.bSuccess);
	TestEqual(TEXT("[refusal] ... as INVALID_FIELD"), WrongTypeResult.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceReferencersNativeTest,
	"Cortex.Reflect.FindInterfaceReferencersNative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceReferencersNativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue277ReferencersTest;
	FFixture Fixture;

	UBlueprint* Implementer = Fixture.MakeBlueprint(TEXT("BP_NativeImpl"), AActor::StaticClass());
	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_NativeCaller"), AActor::StaticClass());
	UBlueprint* Untouched = Fixture.MakeBlueprint(TEXT("BP_NativeUntouched"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Implementer"), Implementer) || !TestNotNull(TEXT("Caller"), Caller) || !TestNotNull(TEXT("Untouched"), Untouched))
	{
		return false;
	}
	UClass* InterfaceClass = UCortexIssue122ReflectInterface::StaticClass();
	TestTrue(TEXT("Implements the native interface"), FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(InterfaceClass)));
	FFixture::AddMessage(FFixture::EventGraph(Caller), TEXT("GetValue"), InterfaceClass);
	for (UBlueprint* Asset : {Implementer, Caller, Untouched})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	// A native interface lives in a script package that every Blueprint of the module may reference, so its registry
	// referencers say nothing about the interface: the split is the evidence only (implementers, caller sites), other stays empty.
	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(TEXT("UCortexIssue122ReflectInterface"), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	ExpectReferencers(*this, TEXT("[native] "), Result.Data, {Implementer->GetPathName()}, {Caller->GetPathName()}, {});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceInheritedHandlesTest,
	"Cortex.Reflect.FindInterfaceInheritedHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceInheritedHandlesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue277ReferencersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Impl = Fixture.MakeBlueprint(TEXT("BP_Impl"), AActor::StaticClass());
	UBlueprint* Both = Fixture.MakeBlueprint(TEXT("BP_Both"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Impl"), Impl) || !TestNotNull(TEXT("Both"), Both))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	const FTopLevelAssetPath InterfacePath(InterfaceClass);
	TestTrue(TEXT("Impl implements"), FBlueprintEditorUtils::ImplementNewInterface(Impl, InterfacePath));
	TestTrue(TEXT("Both implements"), FBlueprintEditorUtils::ImplementNewInterface(Both, InterfacePath));
	FKismetEditorUtilities::CompileBlueprint(Impl);
	FKismetEditorUtilities::CompileBlueprint(Both);
	UBlueprint* Child = Fixture.MakeBlueprint(TEXT("BP_Child"), Impl->GeneratedClass);
	UBlueprint* ChildOfBoth = Fixture.MakeBlueprint(TEXT("BP_ChildOfBoth"), Both->GeneratedClass);
	if (!TestNotNull(TEXT("Child"), Child) || !TestNotNull(TEXT("ChildOfBoth"), ChildOfBoth))
	{
		return false;
	}
	UBlueprint* Grandchild = Fixture.MakeBlueprint(TEXT("BP_Grandchild"), Child->GeneratedClass);
	if (!TestNotNull(TEXT("Grandchild"), Grandchild))
	{
		return false;
	}
	// Impl handles Notify with an event; Both leaves it unimplemented. Both handle GetValue with a function graph.
	FFixture::AddInterfaceEvent(FFixture::EventGraph(Impl), TEXT("Notify"), InterfaceClass, true);
	// Children of Both that implement Notify themselves: with an event node, and with an override function graph.
	UBlueprint* ChildEvent = Fixture.MakeBlueprint(TEXT("BP_ChildEvent"), Both->GeneratedClass);
	UBlueprint* ChildGraph = Fixture.MakeBlueprint(TEXT("BP_ChildGraph"), Both->GeneratedClass);
	if (!TestNotNull(TEXT("ChildEvent"), ChildEvent) || !TestNotNull(TEXT("ChildGraph"), ChildGraph))
	{
		return false;
	}
	FFixture::AddInterfaceEvent(FFixture::EventGraph(ChildEvent), TEXT("Notify"), InterfaceClass, true);
	TestNotNull(TEXT("ChildGraph override graph"), FFixture::AddFunctionGraphTo(ChildGraph, TEXT("Notify"), false));
	// A child of ChildEvent implements nothing itself: the chain still reaches the event.
	UBlueprint* BelowChildEvent = Fixture.MakeBlueprint(TEXT("BP_BelowChildEvent"), ChildEvent->GeneratedClass);
	if (!TestNotNull(TEXT("BelowChildEvent"), BelowChildEvent))
	{
		return false;
	}
	for (UBlueprint* Asset : {Interface, Impl, Both, Child, ChildOfBoth, Grandchild, ChildEvent, ChildGraph, BelowChildEvent})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	const struct
	{
		const TCHAR* Label;
		UBlueprint* Subject;
		UBlueprint* Via;
		bool bExpectedGetValue;
		bool bExpectedNotify;
	} Cases[] = {
		{TEXT("[Child]"), Child, Impl, true, true},
		{TEXT("[Grandchild]"), Grandchild, Impl, true, true},
		{TEXT("[ChildOfBoth]"), ChildOfBoth, Both, true, false},
		{TEXT("[ChildEvent]"), ChildEvent, Both, true, true},
		{TEXT("[ChildGraph]"), ChildGraph, Both, true, true},
		{TEXT("[BelowChildEvent]"), BelowChildEvent, Both, true, true},
	};
	for (const auto& Case : Cases)
	{
		TSharedPtr<FJsonObject> Request = RequestParams(Interface->GetPathName(), Fixture.Dir);
		Request->SetStringField(TEXT("implementer"), Case.Subject->GetPathName());
		const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), Request);
		if (!TestTrue(*FString::Printf(TEXT("%s Narrowed request succeeds"), Case.Label), Result.bSuccess))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonObject>> Implementers = Objects(Result.Data, TEXT("implementers"));
		if (TestEqual(*FString::Printf(TEXT("%s one implementer"), Case.Label), Implementers.Num(), 1))
		{
			TestEqual(*FString::Printf(TEXT("%s comes via its nearest declaring ancestor"), Case.Label), Implementers[0]->GetStringField(TEXT("via")), Case.Via->GetPathName());
		}
		const struct
		{
			const TCHAR* Function;
			bool bExpected;
		} Functions[] = {{TEXT("GetValue"), Case.bExpectedGetValue}, {TEXT("Notify"), Case.bExpectedNotify}};
		for (const auto& Function : Functions)
		{
			const TSharedPtr<FJsonObject> Entry = InterfaceFunction(Result.Data, Function.Function);
			const bool bPresent = Entry.IsValid() && Entry->HasTypedField<EJson::Boolean>(TEXT("implementer_handles"));
			if (TestTrue(*FString::Printf(TEXT("%s %s reports implementer_handles"), Case.Label, Function.Function), bPresent))
			{
				TestEqual(*FString::Printf(TEXT("%s %s handled by the parent chain"), Case.Label, Function.Function), Entry->GetBoolField(TEXT("implementer_handles")), Function.bExpected);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceReferencersMultiAssetPackageTest,
	"Cortex.Reflect.FindInterfaceReferencersMultiAssetPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceReferencersMultiAssetPackageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue277ReferencersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get()))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;

	// Two Blueprints share one package: the first registry asset of that package must not decide both of them.
	UPackage* Shared = CreatePackage(*FString::Printf(TEXT("%s/Shared"), *Fixture.Dir));
	UBlueprint* Implementer = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Shared, TEXT("BP_SharedImpl"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	UBlueprint* Caller = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Shared, TEXT("BP_SharedCaller"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	if (!TestNotNull(TEXT("Implementer"), Implementer) || !TestNotNull(TEXT("Caller"), Caller))
	{
		return false;
	}
	TestTrue(TEXT("Implementer implements"), FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(InterfaceClass)));
	FFixture::AddMessage(FFixture::EventGraph(Caller), TEXT("Notify"), InterfaceClass);
	TestTrue(TEXT("Interface saved"), Fixture.SaveAsset(Interface));
	TestTrue(TEXT("Shared package saved"), Fixture.SaveAsset(Implementer));
	Fixture.RegisterSavedFiles();

	TArray<FAssetData> SharedAssets;
	IAssetRegistry::GetChecked().GetAssetsByPackageName(Shared->GetFName(), SharedAssets);
	TestEqual(TEXT("Fixture: the registry lists both Blueprints of the shared package"), SharedAssets.Num(), 2);

	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(Interface->GetPathName(), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	// Classified by evidence: each proven asset lands in its bucket; the package gets no fallback entry in other.
	ExpectReferencers(*this, TEXT("[shared package] "), Result.Data, {Implementer->GetPathName()}, {Caller->GetPathName()}, {});
	return true;
}
