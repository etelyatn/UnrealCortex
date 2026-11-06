// reflect.find_interface_implementers (callers): sites matched by the resolved owner of the member.

#include "Misc/AutomationTest.h"

#include "CortexReflectIssue122TestSupport.h"
#include "CortexReflectIssue122TestTypes.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CortexIssue122CallersTest
{
	using namespace CortexIssue122ReflectTest;

	TSharedPtr<FJsonObject> RequestParams(const FString& Interface, const FString& Prefix)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("interface"), Interface);
		Result->SetArrayField(TEXT("path_prefixes"), JsonValues({Prefix}));
		return Result;
	}

	/** The sites of one asset in a `callers` array. */
	TArray<TSharedPtr<FJsonObject>> SitesOf(const TArray<TSharedPtr<FJsonObject>>& Sites, const FString& AssetPath)
	{
		return Sites.FilterByPredicate([&AssetPath](const TSharedPtr<FJsonObject>& Site) { return Site->GetStringField(TEXT("asset_path")) == AssetPath; });
	}

	TSharedPtr<FJsonObject> SiteOfNode(const TArray<TSharedPtr<FJsonObject>>& Sites, const FGuid& NodeGuid)
	{
		for (const TSharedPtr<FJsonObject>& Site : Sites)
		{
			if (Site->GetStringField(TEXT("node_guid")) == NodeGuid.ToString())
			{
				return Site;
			}
		}
		return nullptr;
	}

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceCallersTest,
	"Cortex.Reflect.FindInterfaceCallers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceCallersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122CallersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Messenger = Fixture.MakeBlueprint(TEXT("BP_Messenger"), AActor::StaticClass());
	UBlueprint* Typed = Fixture.MakeBlueprint(TEXT("BP_Typed"), AActor::StaticClass());
	UBlueprint* Caster = Fixture.MakeBlueprint(TEXT("BP_Caster"), AActor::StaticClass());
	UBlueprint* Widget = Fixture.MakeWidgetBlueprint(TEXT("WBP_Caller"));
	UBlueprint* Nested = Fixture.MakeBlueprint(TEXT("BP_Nested"), AActor::StaticClass());
	UBlueprint* Unresolved = Fixture.MakeBlueprint(TEXT("BP_Unresolved"), AActor::StaticClass());
	UBlueprint* Many = Fixture.MakeBlueprint(TEXT("BP_Many"), AActor::StaticClass());
	UBlueprint* Plain = Fixture.MakeBlueprint(TEXT("BP_Plain"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Messenger"), Messenger) || !TestNotNull(TEXT("Typed"), Typed) || !TestNotNull(TEXT("Caster"), Caster)
		|| !TestNotNull(TEXT("Widget"), Widget) || !TestNotNull(TEXT("Nested"), Nested) || !TestNotNull(TEXT("Unresolved"), Unresolved)
		|| !TestNotNull(TEXT("Many"), Many) || !TestNotNull(TEXT("Plain"), Plain))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	UClass* CollisionClass = UCortexIssue122ReflectCollision::StaticClass();

	// (a) message nodes, (d) a collision-named call on an unrelated class in the same Blueprint, (g) no node is reported twice.
	UK2Node_Message* MessageGetValue = FFixture::AddMessage(FFixture::EventGraph(Messenger), TEXT("GetValue"), InterfaceClass);
	UK2Node_Message* MessageNotify = FFixture::AddMessage(FFixture::EventGraph(Messenger), TEXT("Notify"), InterfaceClass);
	FFixture::AddTypedCall(FFixture::EventGraph(Messenger), TEXT("GetValue"), CollisionClass);
	// (b) a typed call whose member parent is the interface.
	UK2Node_CallFunction* TypedCall = FFixture::AddTypedCall(FFixture::EventGraph(Typed), TEXT("GetValue"), InterfaceClass);
	// (c) a cast to the interface.
	UK2Node_DynamicCast* Cast = FFixture::AddCast(FFixture::EventGraph(Caster), InterfaceClass);
	// (f) message and cast sites in a Widget Blueprint.
	UK2Node_Message* WidgetMessage = FFixture::AddMessage(FFixture::EventGraph(Widget), TEXT("GetValue"), InterfaceClass);
	UK2Node_DynamicCast* WidgetCast = FFixture::AddCast(FFixture::EventGraph(Widget), InterfaceClass);
	// (e) a message node in a nested composite graph, in an asset that is unloaded after it is saved.
	UK2Node_Composite* Composite = FFixture::AddComposite(FFixture::EventGraph(Nested));
	if (!TestNotNull(TEXT("Composite subgraph"), Composite->BoundGraph.Get()))
	{
		return false;
	}
	UK2Node_Message* NestedMessage = FFixture::AddMessage(Composite->BoundGraph, TEXT("GetValue"), InterfaceClass);
	const FString NestedPath = Nested->GetPathName();
	const FString NestedGraphName = FFixture::EventGraph(Nested)->GetName();
	const FString NestedGraphGuid = FFixture::EventGraph(Nested)->GraphGuid.ToString();
	const FString NestedSubgraph = Composite->BoundGraph->GetName();
	const FString NestedNodeGuid = NestedMessage->NodeGuid.ToString();
	// A cast keeps the Blueprint a candidate; the message node's owner is then lost, as when its interface class is gone.
	UK2Node_DynamicCast* UnresolvedCast = FFixture::AddCast(FFixture::EventGraph(Unresolved), InterfaceClass);
	UK2Node_Message* UnresolvedMessage = FFixture::AddMessage(FFixture::EventGraph(Unresolved), TEXT("GetValue"), InterfaceClass);
	FFixture::SetMemberParentRaw(UnresolvedMessage->FunctionReference, nullptr);
	// More sites than a compact out_path summary keeps.
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FFixture::AddMessage(FFixture::EventGraph(Many), TEXT("GetValue"), InterfaceClass);
	}

	for (UBlueprint* Asset : {Interface, Messenger, Typed, Caster, Widget, Nested, Unresolved, Many, Plain})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();
	if (!TestTrue(TEXT("The nested Blueprint unloads"), FFixture::Unload({Nested}))
		|| !TestNull(TEXT("... and is not in memory"), FindObject<UBlueprint>(nullptr, *NestedPath)))
	{
		return false;
	}

	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(Interface->GetPathName(), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Sites = Objects(Result.Data, TEXT("callers"));
	TestEqual(TEXT("caller_count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("caller_count"))), 12);
	TestEqual(TEXT("callers length"), Sites.Num(), 12);
	TestEqual(TEXT("Only Blueprints that reference the interface are scanned"), static_cast<int32>(Result.Data->GetNumberField(TEXT("caller_assets_scanned"))), 7);

	// (a) + (g): two message nodes, each exactly once, labelled by their own class; (d) the collision call is absent.
	const TArray<TSharedPtr<FJsonObject>> MessengerSites = SitesOf(Sites, Messenger->GetPathName());
	TestEqual(TEXT("The messenger has exactly its two message sites"), MessengerSites.Num(), 2);
	for (UK2Node_Message* Node : {MessageGetValue, MessageNotify})
	{
		const TSharedPtr<FJsonObject> Site = SiteOfNode(MessengerSites, Node->NodeGuid);
		if (TestNotNull(*FString::Printf(TEXT("Message %s reported"), *Node->FunctionReference.GetMemberName().ToString()), Site.Get()))
		{
			TestEqual(TEXT("... as a message"), Site->GetStringField(TEXT("kind")), FString(TEXT("message")));
			TestEqual(TEXT("... of class UK2Node_Message"), Site->GetStringField(TEXT("node_class")), FString(TEXT("UK2Node_Message")));
			TestEqual(TEXT("... with its function"), Site->GetStringField(TEXT("function")), Node->FunctionReference.GetMemberName().ToString());
			TestEqual(TEXT("... in the event graph"), Site->GetStringField(TEXT("graph_guid")), FFixture::EventGraph(Messenger)->GraphGuid.ToString());
			TestEqual(TEXT("... at the top level"), Site->GetStringField(TEXT("subgraph_path")), FString());
		}
	}

	// (b) typed call.
	const TSharedPtr<FJsonObject> TypedSite = SiteOfNode(SitesOf(Sites, Typed->GetPathName()), TypedCall->NodeGuid);
	if (TestNotNull(TEXT("Typed call reported"), TypedSite.Get()))
	{
		TestEqual(TEXT("... as typed_call"), TypedSite->GetStringField(TEXT("kind")), FString(TEXT("typed_call")));
		TestEqual(TEXT("... of class UK2Node_CallFunction"), TypedSite->GetStringField(TEXT("node_class")), FString(TEXT("UK2Node_CallFunction")));
	}

	// (c) cast.
	const TSharedPtr<FJsonObject> CastSite = SiteOfNode(SitesOf(Sites, Caster->GetPathName()), Cast->NodeGuid);
	if (TestNotNull(TEXT("Cast reported"), CastSite.Get()))
	{
		TestEqual(TEXT("... as interface_cast"), CastSite->GetStringField(TEXT("kind")), FString(TEXT("interface_cast")));
		TestEqual(TEXT("... of class UK2Node_DynamicCast"), CastSite->GetStringField(TEXT("node_class")), FString(TEXT("UK2Node_DynamicCast")));
		TestFalse(TEXT("... without a function"), CastSite->HasField(TEXT("function")));
	}

	// (f) Widget Blueprint.
	const TArray<TSharedPtr<FJsonObject>> WidgetSites = SitesOf(Sites, Widget->GetPathName());
	TestEqual(TEXT("The Widget Blueprint has a message and a cast"), WidgetSites.Num(), 2);
	TestNotNull(TEXT("Widget message"), SiteOfNode(WidgetSites, WidgetMessage->NodeGuid).Get());
	TestNotNull(TEXT("Widget cast"), SiteOfNode(WidgetSites, WidgetCast->NodeGuid).Get());

	// (e) the nested site in the unloaded asset: top-level graph plus the dot path of the composite subgraph.
	const TArray<TSharedPtr<FJsonObject>> NestedSites = SitesOf(Sites, NestedPath);
	if (TestEqual(TEXT("The nested Blueprint has one site"), NestedSites.Num(), 1))
	{
		TestEqual(TEXT("nested node_guid"), NestedSites[0]->GetStringField(TEXT("node_guid")), NestedNodeGuid);
		TestEqual(TEXT("nested graph_name is the top-level graph"), NestedSites[0]->GetStringField(TEXT("graph_name")), NestedGraphName);
		TestEqual(TEXT("nested graph_guid"), NestedSites[0]->GetStringField(TEXT("graph_guid")), NestedGraphGuid);
		TestEqual(TEXT("nested subgraph_path"), NestedSites[0]->GetStringField(TEXT("subgraph_path")), NestedSubgraph);
		TestEqual(TEXT("nested kind"), NestedSites[0]->GetStringField(TEXT("kind")), FString(TEXT("message")));
	}

	// A message whose owner cannot be resolved is listed apart, never dropped and never counted as a caller.
	const TArray<TSharedPtr<FJsonObject>> UnresolvedSites = Objects(Result.Data, TEXT("unresolved_callers"));
	if (TestEqual(TEXT("One unresolved caller"), UnresolvedSites.Num(), 1))
	{
		TestEqual(TEXT("... the message node"), UnresolvedSites[0]->GetStringField(TEXT("node_guid")), UnresolvedMessage->NodeGuid.ToString());
		TestEqual(TEXT("... of the unresolved asset"), UnresolvedSites[0]->GetStringField(TEXT("asset_path")), Unresolved->GetPathName());
		TestEqual(TEXT("... with its function"), UnresolvedSites[0]->GetStringField(TEXT("function")), FString(TEXT("GetValue")));
	}
	const TArray<TSharedPtr<FJsonObject>> UnresolvedAssetSites = SitesOf(Sites, Unresolved->GetPathName());
	TestEqual(TEXT("The unresolved asset still reports its resolvable cast"), UnresolvedAssetSites.Num(), 1);
	TestNotNull(TEXT("... which is the cast"), SiteOfNode(UnresolvedAssetSites, UnresolvedCast->NodeGuid).Get());
	TestEqual(TEXT("A Blueprint without any node on the interface is not a caller"), SitesOf(Sites, Plain->GetPathName()).Num(), 0);

	// Sorted by asset path, then graph, then node.
	bool bSorted = true;
	for (int32 Index = 1; Index < Sites.Num(); ++Index)
	{
		bSorted &= Sites[Index - 1]->GetStringField(TEXT("asset_path")).Compare(Sites[Index]->GetStringField(TEXT("asset_path")), ESearchCase::CaseSensitive) <= 0;
	}
	TestTrue(TEXT("Callers are sorted by asset path"), bSorted);

	// function filter: message and typed-call sites of that function (casts are not function-specific and stay).
	TSharedPtr<FJsonObject> Filtered = RequestParams(Interface->GetPathName(), Fixture.Dir);
	Filtered->SetStringField(TEXT("function"), TEXT("Notify"));
	const FCortexCommandResult FilteredResult = Run(TEXT("find_interface_implementers"), Filtered);
	if (TestTrue(TEXT("Filtered request succeeds"), FilteredResult.bSuccess))
	{
		TArray<FString> CalledFunctions;
		for (const TSharedPtr<FJsonObject>& Site : Objects(FilteredResult.Data, TEXT("callers")))
		{
			if (Site->GetStringField(TEXT("kind")) != TEXT("interface_cast"))
			{
				CalledFunctions.Add(Site->GetStringField(TEXT("function")));
			}
		}
		TestEqual(TEXT("Only the Notify message remains"), FString::Join(CalledFunctions, TEXT(",")), FString(TEXT("Notify")));
		TestEqual(TEXT("The three casts remain"),
			Objects(FilteredResult.Data, TEXT("callers")).FilterByPredicate([](const TSharedPtr<FJsonObject>& Site) { return Site->GetStringField(TEXT("kind")) == TEXT("interface_cast"); }).Num(), 3);
		TestEqual(TEXT("The unresolved GetValue message is filtered out too"), Objects(FilteredResult.Data, TEXT("unresolved_callers")).Num(), 0);
	}

	// include_callers:false omits the whole caller section.
	TSharedPtr<FJsonObject> WithoutCallers = RequestParams(Interface->GetPathName(), Fixture.Dir);
	WithoutCallers->SetBoolField(TEXT("include_callers"), false);
	const FCortexCommandResult WithoutResult = Run(TEXT("find_interface_implementers"), WithoutCallers);
	if (TestTrue(TEXT("include_callers:false succeeds"), WithoutResult.bSuccess))
	{
		TestFalse(TEXT("No callers list"), WithoutResult.Data->HasField(TEXT("callers")));
		TestFalse(TEXT("No caller_count"), WithoutResult.Data->HasField(TEXT("caller_count")));
		TestFalse(TEXT("No unresolved_callers"), WithoutResult.Data->HasField(TEXT("unresolved_callers")));
	}

	// out_path: the file holds everything, the response only counts and the first 10 entries.
	TSharedPtr<FJsonObject> ToFile = RequestParams(Interface->GetPathName(), Fixture.Dir);
	const FString ReportPath = Fixture.ReportPath(TEXT("Callers"));
	ToFile->SetStringField(TEXT("out_path"), ReportPath);
	const FCortexCommandResult FileResult = Run(TEXT("find_interface_implementers"), ToFile);
	if (TestTrue(TEXT("out_path request succeeds"), FileResult.bSuccess))
	{
		TestEqual(TEXT("The summary keeps the first 10 callers"), Objects(FileResult.Data, TEXT("callers")).Num(), 10);
		TestEqual(TEXT("... and totals all 12"), static_cast<int32>(FileResult.Data->GetNumberField(TEXT("callers_total"))), 12);
		FString Text;
		TSharedPtr<FJsonObject> Written;
		TestTrue(TEXT("The report file is written"), FFileHelper::LoadFileToString(Text, *ReportPath));
		TestTrue(TEXT("... and parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Written) && Written.IsValid());
		if (Written.IsValid())
		{
			TestEqual(TEXT("The file holds every caller"), Objects(Written, TEXT("callers")).Num(), 12);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceCallersNativeTest,
	"Cortex.Reflect.FindInterfaceCallersNative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceCallersNativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122CallersTest;
	FFixture Fixture;

	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_NativeCaller"), AActor::StaticClass());
	UBlueprint* Untouched = Fixture.MakeBlueprint(TEXT("BP_NativeUntouched"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Caller"), Caller) || !TestNotNull(TEXT("Untouched"), Untouched))
	{
		return false;
	}
	UClass* InterfaceClass = UCortexIssue122ReflectInterface::StaticClass();
	UK2Node_Message* Message = FFixture::AddMessage(FFixture::EventGraph(Caller), TEXT("GetValue"), InterfaceClass);
	UK2Node_DynamicCast* Cast = FFixture::AddCast(FFixture::EventGraph(Caller), InterfaceClass);
	TestTrue(TEXT("Caller saved"), Fixture.SaveAsset(Caller));
	TestTrue(TEXT("Untouched saved"), Fixture.SaveAsset(Untouched));
	Fixture.RegisterSavedFiles();

	// A native interface has no content package: every Blueprint under the prefixes is loaded and scanned.
	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(TEXT("UCortexIssue122ReflectInterface"), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Both Blueprints are scanned"), static_cast<int32>(Result.Data->GetNumberField(TEXT("caller_assets_scanned"))), 2);
	const TArray<TSharedPtr<FJsonObject>> Sites = Objects(Result.Data, TEXT("callers"));
	TestEqual(TEXT("Two caller sites"), Sites.Num(), 2);
	const TSharedPtr<FJsonObject> MessageSite = SiteOfNode(Sites, Message->NodeGuid);
	if (TestNotNull(TEXT("Message reported"), MessageSite.Get()))
	{
		TestEqual(TEXT("... as a message"), MessageSite->GetStringField(TEXT("kind")), FString(TEXT("message")));
		TestEqual(TEXT("... of the caller"), MessageSite->GetStringField(TEXT("asset_path")), Caller->GetPathName());
	}
	const TSharedPtr<FJsonObject> CastSite = SiteOfNode(Sites, Cast->NodeGuid);
	if (TestNotNull(TEXT("Cast reported"), CastSite.Get()))
	{
		TestEqual(TEXT("... as interface_cast"), CastSite->GetStringField(TEXT("kind")), FString(TEXT("interface_cast")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindInterfaceCallersSubGraphsTest,
	"Cortex.Reflect.FindInterfaceCallersSubGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindInterfaceCallersSubGraphsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122CallersTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeInterface(Fixture, TEXT("BPI_Ping"));
	UBlueprint* Holder = Fixture.MakeBlueprint(TEXT("BP_SubGraphs"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get()) || !TestNotNull(TEXT("Holder"), Holder))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	// Graphs that live only in SubGraphs (animation state, transition and blend-space graphs are kept like this).
	UEdGraph* State = FFixture::AddSubGraph(FFixture::EventGraph(Holder), TEXT("State"));
	UEdGraph* Rule = State ? FFixture::AddSubGraph(State, TEXT("Rule")) : nullptr;
	if (!TestNotNull(TEXT("State graph"), State) || !TestNotNull(TEXT("Nested rule graph"), Rule))
	{
		return false;
	}
	UK2Node_Message* InState = FFixture::AddMessage(State, TEXT("GetValue"), InterfaceClass);
	UK2Node_DynamicCast* InRule = FFixture::AddCast(Rule, InterfaceClass);
	TestTrue(TEXT("Interface saved"), Fixture.SaveAsset(Interface));
	TestTrue(TEXT("Holder saved"), Fixture.SaveAsset(Holder));
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Result = Run(TEXT("find_interface_implementers"), RequestParams(Interface->GetPathName(), Fixture.Dir));
	if (!TestTrue(TEXT("find_interface_implementers succeeds"), Result.bSuccess))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Sites = Objects(Result.Data, TEXT("callers"));
	TestEqual(TEXT("Both nested sites are found"), Sites.Num(), 2);
	const TSharedPtr<FJsonObject> StateSite = SiteOfNode(Sites, InState->NodeGuid);
	if (TestNotNull(TEXT("The message in the State subgraph"), StateSite.Get()))
	{
		TestEqual(TEXT("... top-level graph"), StateSite->GetStringField(TEXT("graph_guid")), FFixture::EventGraph(Holder)->GraphGuid.ToString());
		TestEqual(TEXT("... subgraph_path"), StateSite->GetStringField(TEXT("subgraph_path")), FString(TEXT("State")));
	}
	const TSharedPtr<FJsonObject> RuleSite = SiteOfNode(Sites, InRule->NodeGuid);
	if (TestNotNull(TEXT("The cast in the nested Rule subgraph"), RuleSite.Get()))
	{
		TestEqual(TEXT("... subgraph_path"), RuleSite->GetStringField(TEXT("subgraph_path")), FString(TEXT("State.Rule")));
	}
	return true;
}
