// reflect.find_interface_implementers (implementers) and the interface branch of find_overrides.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/ARFilter.h"
#include "CortexReflectIssue122TestSupport.h"
#include "CortexReflectIssue122TestTypes.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"

namespace CortexIssue122ImplementersTest
{
	using namespace CortexIssue122ReflectTest;

	TSharedPtr<FJsonObject> FindImplementer(const TSharedPtr<FJsonObject>& Data, const FString& AssetPath)
	{
		for (const TSharedPtr<FJsonObject>& Implementer : Objects(Data, TEXT("implementers")))
		{
			if (Implementer->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Implementer;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindFunction(const TSharedPtr<FJsonObject>& Implementer, const FString& Name)
	{
		for (const TSharedPtr<FJsonObject>& Function : Objects(Implementer, TEXT("functions")))
		{
			if (Function->GetStringField(TEXT("function")) == Name)
			{
				return Function;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> Params(const FString& Interface, const FString& Prefix)
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

	const FBPInterfaceDescription* FindDescription(const UBlueprint* Blueprint, const UClass* Interface)
	{
		return Blueprint->ImplementedInterfaces.FindByPredicate(
			[Interface](const FBPInterfaceDescription& Description) { return Description.Interface.Get() == Interface; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceImplementersTest,
	"Cortex.Reflect.FindInterfaceImplementers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceImplementersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122ImplementersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_A"), AActor::StaticClass());
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_B"), AActor::StaticClass());
	UBlueprint* D = Fixture.MakeBlueprint(TEXT("BP_D"), AActor::StaticClass());
	UBlueprint* Widget = Fixture.MakeWidgetBlueprint(TEXT("WBP_W"));
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("D"), D) || !TestNotNull(TEXT("Widget"), Widget))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	const FTopLevelAssetPath InterfacePath(InterfaceClass);
	TestTrue(TEXT("A implements"), FBlueprintEditorUtils::ImplementNewInterface(A, InterfacePath));
	TestTrue(TEXT("B implements"), FBlueprintEditorUtils::ImplementNewInterface(B, InterfacePath));
	TestTrue(TEXT("Widget implements"), FBlueprintEditorUtils::ImplementNewInterface(Widget, InterfacePath));
	FKismetEditorUtilities::CompileBlueprint(A);

	// C derives from A (created after A compiled, before A receives nodes).
	UBlueprint* C = Fixture.MakeBlueprint(TEXT("BP_C"), A->GeneratedClass);
	if (!TestNotNull(TEXT("C"), C))
	{
		return false;
	}

	// A: GetValue as a function graph (made by ImplementNewInterface) and Notify as an event with a linked body.
	UK2Node_Event* NotifyEvent = FFixture::AddInterfaceEvent(FFixture::EventGraph(A), TEXT("Notify"), InterfaceClass, true);
	UK2Node_Event* WidgetEvent = FFixture::AddInterfaceEvent(FFixture::EventGraph(Widget), TEXT("Notify"), InterfaceClass, false);

	for (UBlueprint* Asset : {Interface, A, B, C, D, Widget})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	// The widget's event names the interface through its skeleton class (in memory, after the save): it must not be reported as unimplemented.
	FFixture::SetEventParentRaw(WidgetEvent, Interface->SkeletonGeneratedClass);
	TestTrue(TEXT("The widget event really holds the skeleton class"), WidgetEvent->EventReference.GetMemberParentClass() == Interface->SkeletonGeneratedClass);
	TestTrue(TEXT("... which is not the interface class itself"), Interface->SkeletonGeneratedClass != InterfaceClass);

	const FString InterfaceAsset = Interface->GetPathName();
	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), Params(InterfaceAsset, Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Data = Result.Data;

	// The interface itself: class, native flag, per-function style by the engine's own rule.
	TestEqual(TEXT("interface_class"), Data->GetStringField(TEXT("interface_class")), InterfaceClass->GetClassPathName().ToString());
	TestFalse(TEXT("A Blueprint interface is not native"), Data->GetBoolField(TEXT("native")));
	TMap<FString, FString> Styles;
	for (const TSharedPtr<FJsonObject>& Function : Objects(Data, TEXT("functions")))
	{
		Styles.Add(Function->GetStringField(TEXT("name")), Function->GetStringField(TEXT("style")));
	}
	TestEqual(TEXT("GetValue (output) is function-style"), Styles.FindRef(TEXT("GetValue")), FString(TEXT("function")));
	TestEqual(TEXT("Notify (void) is event-style"), Styles.FindRef(TEXT("Notify")), FString(TEXT("event")));

	// Implementers: A, B, C (inherited), the Widget Blueprint; never D. Sorted by asset path.
	TArray<FString> Paths;
	for (const TSharedPtr<FJsonObject>& Implementer : Objects(Data, TEXT("implementers")))
	{
		Paths.Add(Implementer->GetStringField(TEXT("asset_path")));
	}
	const TArray<FString> ExpectedPaths = {A->GetPathName(), B->GetPathName(), C->GetPathName(), Widget->GetPathName()};
	TestEqual(TEXT("Implementers are A, B, C, Widget in path order"), FString::Join(Paths, TEXT(",")), FString::Join(ExpectedPaths, TEXT(",")));
	TestEqual(TEXT("implementer_count"), static_cast<int32>(Data->GetNumberField(TEXT("implementer_count"))), 4);

	// A: function graph for GetValue (guids and node count read back from the Blueprint), event with a body for Notify.
	const TSharedPtr<FJsonObject> RecordA = FindImplementer(Data, A->GetPathName());
	if (TestNotNull(TEXT("A reported"), RecordA.Get()))
	{
		TestTrue(TEXT("A is direct"), RecordA->GetBoolField(TEXT("direct")));
		TestFalse(TEXT("A has no via"), RecordA->HasField(TEXT("via")));
		const FBPInterfaceDescription* Description = FindDescription(A, InterfaceClass);
		const TSharedPtr<FJsonObject> GetValue = FindFunction(RecordA, TEXT("GetValue"));
		if (TestNotNull(TEXT("A GetValue"), GetValue.Get()) && TestNotNull(TEXT("A interface description"), Description)
			&& TestEqual(TEXT("A has one interface graph"), Description->Graphs.Num(), 1))
		{
			const UEdGraph* Graph = Description->Graphs[0];
			TestEqual(TEXT("A GetValue is a function_graph"), GetValue->GetStringField(TEXT("implementation")), FString(TEXT("function_graph")));
			TestEqual(TEXT("A GetValue graph_name"), GetValue->GetStringField(TEXT("graph_name")), FString(TEXT("GetValue")));
			TestEqual(TEXT("A GetValue graph_guid"), GetValue->GetStringField(TEXT("graph_guid")), Graph->GraphGuid.ToString());
			TestEqual(TEXT("A GetValue node_count"), static_cast<int32>(GetValue->GetNumberField(TEXT("node_count"))), Graph->Nodes.Num());
			FString ExpectedEntry;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (ExpectedEntry.IsEmpty() && Cast<UK2Node_FunctionEntry>(Node))
				{
					ExpectedEntry = Node->NodeGuid.ToString();
				}
			}
			TestFalse(TEXT("The function graph has an entry node"), ExpectedEntry.IsEmpty());
			TestEqual(TEXT("A GetValue entry_node_guid"), GetValue->GetStringField(TEXT("entry_node_guid")), ExpectedEntry);
		}
		const TSharedPtr<FJsonObject> Notify = FindFunction(RecordA, TEXT("Notify"));
		if (TestNotNull(TEXT("A Notify"), Notify.Get()))
		{
			TestEqual(TEXT("A Notify is an event"), Notify->GetStringField(TEXT("implementation")), FString(TEXT("event")));
			TestEqual(TEXT("A Notify entry_node_guid"), Notify->GetStringField(TEXT("entry_node_guid")), NotifyEvent->NodeGuid.ToString());
			TestEqual(TEXT("A Notify graph_guid"), Notify->GetStringField(TEXT("graph_guid")), FFixture::EventGraph(A)->GraphGuid.ToString());
			TestTrue(TEXT("A Notify has a body"), Notify->GetBoolField(TEXT("has_body")));
		}
		const TArray<TSharedPtr<FJsonObject>> Functions = Objects(RecordA, TEXT("functions"));
		TestEqual(TEXT("A lists both interface functions, sorted"),
			Functions.Num() == 2 ? Functions[0]->GetStringField(TEXT("function")) + TEXT(",") + Functions[1]->GetStringField(TEXT("function")) : FString(),
			FString(TEXT("GetValue,Notify")));
	}

	// B: no event, so Notify is none.
	const TSharedPtr<FJsonObject> RecordB = FindImplementer(Data, B->GetPathName());
	if (TestNotNull(TEXT("B reported"), RecordB.Get()))
	{
		const TSharedPtr<FJsonObject> Notify = FindFunction(RecordB, TEXT("Notify"));
		if (TestNotNull(TEXT("B Notify"), Notify.Get()))
		{
			TestEqual(TEXT("B Notify is none"), Notify->GetStringField(TEXT("implementation")), FString(TEXT("none")));
		}
		const TSharedPtr<FJsonObject> GetValue = FindFunction(RecordB, TEXT("GetValue"));
		if (TestNotNull(TEXT("B GetValue"), GetValue.Get()))
		{
			TestEqual(TEXT("B GetValue is a function_graph"), GetValue->GetStringField(TEXT("implementation")), FString(TEXT("function_graph")));
		}
	}

	// C: inherits the interface from A.
	const TSharedPtr<FJsonObject> RecordC = FindImplementer(Data, C->GetPathName());
	if (TestNotNull(TEXT("C reported"), RecordC.Get()))
	{
		TestFalse(TEXT("C is not direct"), RecordC->GetBoolField(TEXT("direct")));
		TestEqual(TEXT("C comes via A"), RecordC->GetStringField(TEXT("via")), A->GetPathName());
	}

	// Widget Blueprint: loaded as a UBlueprint subclass; its event names the interface by the skeleton class and is still an event.
	const TSharedPtr<FJsonObject> RecordWidget = FindImplementer(Data, Widget->GetPathName());
	if (TestNotNull(TEXT("Widget Blueprint reported"), RecordWidget.Get()))
	{
		const TSharedPtr<FJsonObject> Notify = FindFunction(RecordWidget, TEXT("Notify"));
		if (TestNotNull(TEXT("Widget Notify"), Notify.Get()))
		{
			TestEqual(TEXT("A skeleton-parent event is an event, not none"), Notify->GetStringField(TEXT("implementation")), FString(TEXT("event")));
			TestFalse(TEXT("The widget event has no body"), Notify->GetBoolField(TEXT("has_body")));
		}
	}
	TestNull(TEXT("D does not implement the interface"), FindImplementer(Data, D->GetPathName()).Get());

	// The function filter keeps one function per implementer.
	TSharedPtr<FJsonObject> Filtered = Params(InterfaceAsset, Fixture.Dir);
	Filtered->SetStringField(TEXT("function"), TEXT("Notify"));
	const FCortexCommandResult FilteredResult = Run(TEXT("find_interface_implementers"), Filtered);
	if (TestTrue(TEXT("Filtered request succeeds"), FilteredResult.bSuccess))
	{
		const TSharedPtr<FJsonObject> FilteredA = FindImplementer(FilteredResult.Data, A->GetPathName());
		if (TestNotNull(TEXT("A in the filtered result"), FilteredA.Get()))
		{
			TestEqual(TEXT("Only Notify remains"), Objects(FilteredA, TEXT("functions")).Num(), 1);
		}
	}
	TSharedPtr<FJsonObject> UnknownFunction = Params(InterfaceAsset, Fixture.Dir);
	UnknownFunction->SetStringField(TEXT("function"), TEXT("NoSuchFunction"));
	TestEqual(TEXT("An unknown function is refused"), Run(TEXT("find_interface_implementers"), UnknownFunction).ErrorCode, CortexErrorCodes::InvalidField);

	// The registry-tag prefilter must find what a full load of every Blueprint finds.
	TSet<FString> FromCommand;
	for (const TSharedPtr<FJsonObject>& Implementer : Objects(Data, TEXT("implementers")))
	{
		if (Implementer->GetBoolField(TEXT("direct")))
		{
			FromCommand.Add(Implementer->GetStringField(TEXT("asset_path")));
		}
	}
	TSet<FString> FromFullLoad;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*Fixture.Dir));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssets(Filter, Assets);
	TestEqual(TEXT("All six Blueprints are in the registry"), Assets.Num(), 6);
	for (const FAssetData& Asset : Assets)
	{
		const UBlueprint* Loaded = Cast<UBlueprint>(Asset.GetAsset());
		if (Loaded && FindDescription(Loaded, InterfaceClass))
		{
			FromFullLoad.Add(Loaded->GetPathName());
		}
	}
	TestEqual(TEXT("The tag prefilter returns the same direct implementers as a full load"), FromCommand.Num(), FromFullLoad.Num());
	TestTrue(TEXT("... and the same set"), FromCommand.Includes(FromFullLoad) && FromFullLoad.Includes(FromCommand));

	// Refusals: a class that is no interface, and the required prefixes.
	FCortexCommandResult NotInterface = Run(TEXT("find_interface_implementers"), Params(TEXT("AActor"), Fixture.Dir));
	TestFalse(TEXT("A non-interface class is refused"), NotInterface.bSuccess);
	TestEqual(TEXT("... as INVALID_OPERATION"), NotInterface.ErrorCode, CortexErrorCodes::InvalidOperation);
	TSharedPtr<FJsonObject> NoPrefixes = MakeShared<FJsonObject>();
	NoPrefixes->SetStringField(TEXT("interface"), InterfaceAsset);
	const FCortexCommandResult NoPrefixesResult = Run(TEXT("find_interface_implementers"), NoPrefixes);
	TestFalse(TEXT("path_prefixes is required"), NoPrefixesResult.bSuccess);
	TestEqual(TEXT("... as INVALID_FIELD"), NoPrefixesResult.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceImplementersNativeTest,
	"Cortex.Reflect.FindInterfaceImplementersNative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceImplementersNativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122ImplementersTest;
	FFixture Fixture;

	UBlueprint* Implementer = Fixture.MakeBlueprint(TEXT("BP_NativeImpl"), AActor::StaticClass());
	UBlueprint* Other = Fixture.MakeBlueprint(TEXT("BP_NativeOther"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Implementer"), Implementer) || !TestNotNull(TEXT("Other"), Other))
	{
		return false;
	}
	UClass* InterfaceClass = UCortexIssue122ReflectInterface::StaticClass();
	TestTrue(TEXT("Implements the native interface"), FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(InterfaceClass)));
	// Notify(int32) is event-style: the Blueprint implements it with an event node (no body here).
	UK2Node_Event* NotifyEvent = FFixture::AddInterfaceEvent(FFixture::EventGraph(Implementer), TEXT("Notify"), InterfaceClass, false);
	TestTrue(TEXT("Implementer saved"), Fixture.SaveAsset(Implementer));
	TestTrue(TEXT("Other saved"), Fixture.SaveAsset(Other));
	Fixture.RegisterSavedFiles();

	// The U class name, the C++ I spelling and the script path all resolve to the same interface.
	const TCHAR* const Spellings[] = {
		TEXT("UCortexIssue122ReflectInterface"), TEXT("ICortexIssue122ReflectInterface"), TEXT("/Script/CortexReflect.CortexIssue122ReflectInterface")};
	for (const TCHAR* Spelling : Spellings)
	{
		const FString Label = FString::Printf(TEXT("[%s] "), Spelling);
		const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), Params(Spelling, Fixture.Dir));
		if (!TestTrue(*(Label + TEXT("succeeds")), Result.bSuccess))
		{
			continue;
		}
		TestTrue(*(Label + TEXT("native interface")), Result.Data->GetBoolField(TEXT("native")));
		TestEqual(*(Label + TEXT("interface_class")), Result.Data->GetStringField(TEXT("interface_class")), InterfaceClass->GetClassPathName().ToString());
		TestEqual(*(Label + TEXT("one implementer")), Objects(Result.Data, TEXT("implementers")).Num(), 1);
		const TSharedPtr<FJsonObject> Record = FindImplementer(Result.Data, Implementer->GetPathName());
		if (!TestNotNull(*(Label + TEXT("implementer listed")), Record.Get()))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Notify = FindFunction(Record, TEXT("Notify"));
		if (TestNotNull(*(Label + TEXT("Notify")), Notify.Get()))
		{
			TestEqual(*(Label + TEXT("Notify is an event")), Notify->GetStringField(TEXT("implementation")), FString(TEXT("event")));
			TestEqual(*(Label + TEXT("Notify entry")), Notify->GetStringField(TEXT("entry_node_guid")), NotifyEvent->NodeGuid.ToString());
			TestFalse(*(Label + TEXT("Notify has no body")), Notify->GetBoolField(TEXT("has_body")));
		}
		const TSharedPtr<FJsonObject> GetValue = FindFunction(Record, TEXT("GetValue"));
		if (TestNotNull(*(Label + TEXT("GetValue")), GetValue.Get()))
		{
			TestEqual(*(Label + TEXT("GetValue is a function_graph")), GetValue->GetStringField(TEXT("implementation")), FString(TEXT("function_graph")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindOverridesInterfaceTest,
	"Cortex.Reflect.FindOverridesInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindOverridesInterfaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122ImplementersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Actor = Fixture.MakeBlueprint(TEXT("BP_OverrideActor"), AActor::StaticClass());
	UBlueprint* Widget = Fixture.MakeWidgetBlueprint(TEXT("WBP_OverrideWidget"));
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Actor"), Actor) || !TestNotNull(TEXT("Widget"), Widget))
	{
		return false;
	}
	const FTopLevelAssetPath InterfacePath(Interface->GeneratedClass.Get());
	TestTrue(TEXT("Actor implements"), FBlueprintEditorUtils::ImplementNewInterface(Actor, InterfacePath));
	TestTrue(TEXT("Widget implements"), FBlueprintEditorUtils::ImplementNewInterface(Widget, InterfacePath));
	for (UBlueprint* Asset : {Interface, Actor, Widget})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	// With an explicit prefix and with the default /Game prefix both implementers are found.
	for (const bool bExplicitPrefix : {true, false})
	{
		const FString Label = bExplicitPrefix ? TEXT("[path_prefixes] ") : TEXT("[default prefix] ");
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("class_name"), Interface->GetPathName());
		if (bExplicitPrefix)
		{
			Request->SetArrayField(TEXT("path_prefixes"), JsonValues({Fixture.Dir}));
		}
		const FCortexCommandResult Result = Run(TEXT("find_overrides"), Request);
		if (!TestTrue(*(Label + TEXT("find_overrides succeeds")), Result.bSuccess))
		{
			continue;
		}
		TestNotNull(*(Label + TEXT("Actor implementer")), FindImplementer(Result.Data, Actor->GetPathName()).Get());
		TestNotNull(*(Label + TEXT("Widget implementer")), FindImplementer(Result.Data, Widget->GetPathName()).Get());
		TestTrue(*(Label + TEXT("note names find_interface_implementers")), Result.Data->GetStringField(TEXT("note")).Contains(TEXT("find_interface_implementers")));
	}

	// A native base class still returns its derived Blueprints, unchanged and without implementers.
	TSharedPtr<FJsonObject> Native = MakeShared<FJsonObject>();
	Native->SetStringField(TEXT("class_name"), TEXT("AActor"));
	Native->SetNumberField(TEXT("limit"), 100000);
	const FCortexCommandResult NativeResult = Run(TEXT("find_overrides"), Native);
	if (TestTrue(TEXT("find_overrides on a native class succeeds"), NativeResult.bSuccess))
	{
		TestFalse(TEXT("A native class response has no implementers"), NativeResult.Data->HasField(TEXT("implementers")));
		bool bFoundActor = false;
		for (const TSharedPtr<FJsonObject>& Child : Objects(NativeResult.Data, TEXT("children")))
		{
			bFoundActor |= Child->GetStringField(TEXT("name")) == TEXT("BP_OverrideActor");
		}
		TestTrue(TEXT("The derived Blueprint is still listed"), bFoundActor);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceImplementersCollapsedEventTest,
	"Cortex.Reflect.FindInterfaceImplementersCollapsedEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceImplementersCollapsedEventTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122ImplementersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Implementer = Fixture.MakeBlueprint(TEXT("BP_Collapsed"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get()) || !TestNotNull(TEXT("Implementer"), Implementer))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	TestTrue(TEXT("Implements"), FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(InterfaceClass)));
	// The interface event was collapsed: it lives in a composite subgraph of the event graph.
	UK2Node_Composite* Composite = FFixture::AddComposite(FFixture::EventGraph(Implementer));
	if (!TestNotNull(TEXT("Composite subgraph"), Composite->BoundGraph.Get()))
	{
		return false;
	}
	UK2Node_Event* Event = FFixture::AddInterfaceEvent(Composite->BoundGraph, TEXT("Notify"), InterfaceClass, true);
	TestTrue(TEXT("Implementer saved"), Fixture.SaveAsset(Implementer));
	TestTrue(TEXT("Interface saved"), Fixture.SaveAsset(Interface));
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), Params(Interface->GetPathName(), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Record = FindImplementer(Result.Data, Implementer->GetPathName());
	if (!TestNotNull(TEXT("Implementer reported"), Record.Get()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Notify = FindFunction(Record, TEXT("Notify"));
	if (TestNotNull(TEXT("Notify"), Notify.Get()))
	{
		TestEqual(TEXT("A collapsed interface event is an event, not none"), Notify->GetStringField(TEXT("implementation")), FString(TEXT("event")));
		TestEqual(TEXT("... entry node"), Notify->GetStringField(TEXT("entry_node_guid")), Event->NodeGuid.ToString());
		TestEqual(TEXT("... top-level graph"), Notify->GetStringField(TEXT("graph_guid")), FFixture::EventGraph(Implementer)->GraphGuid.ToString());
		TestEqual(TEXT("... subgraph_path"), Notify->GetStringField(TEXT("subgraph_path")), Composite->BoundGraph->GetName());
		TestTrue(TEXT("... with its body"), Notify->GetBoolField(TEXT("has_body")));
	}
	return true;
}
