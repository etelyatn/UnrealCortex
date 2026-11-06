// #122 fixes to find_usages: matching by the member's resolved owner, node labels taken from the node, and Blueprint
// subclasses (Widget Blueprints) that are not loaded.

#include "Misc/AutomationTest.h"

#include "CortexReflectIssue122TestSupport.h"
#include "CortexReflectIssue122TestTypes.h"
#include "CortexTypes.h"

namespace CortexIssue122UsagesTest
{
	using namespace CortexIssue122ReflectTest;

	TSharedPtr<FJsonObject> FindUsage(const TSharedPtr<FJsonObject>& Data, const FString& AssetPath)
	{
		for (const TSharedPtr<FJsonObject>& Usage : Objects(Data, TEXT("usages")))
		{
			if (Usage->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Usage;
			}
		}
		return nullptr;
	}

	int32 CountReferences(const TSharedPtr<FJsonObject>& Usage, const FString& Type, const FString& NodeClass)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonObject>& Reference : Objects(Usage, TEXT("references")))
		{
			if (Reference->GetStringField(TEXT("type")) == Type && Reference->GetStringField(TEXT("node_class")) == NodeClass)
			{
				++Count;
			}
		}
		return Count;
	}

	TSharedPtr<FJsonObject> FindUsagesParams(const FString& Symbol, const FString& ClassName, const FString& Scope, const FString& PathFilter, const bool bDeepScan)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), Symbol);
		Params->SetStringField(TEXT("class_name"), ClassName);
		Params->SetStringField(TEXT("scope"), Scope);
		Params->SetStringField(TEXT("path_filter"), PathFilter);
		Params->SetBoolField(TEXT("deep_scan"), bDeepScan);
		return Params;
	}

	/** A compiled BPI with one function GetValue (double output). */
	UBlueprint* MakeValueInterface(FFixture& Fixture, const FString& Name)
	{
		UBlueprint* Interface = Fixture.MakeBlueprint(Name, UInterface::StaticClass(), BPTYPE_Interface);
		if (Interface && FFixture::AddFunctionGraphTo(Interface, TEXT("GetValue"), true))
		{
			FKismetEditorUtilities::CompileBlueprint(Interface);
		}
		return Interface;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindUsagesWidgetBlueprintTest,
	"Cortex.Reflect.FindUsagesWidgetBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindUsagesWidgetBlueprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122UsagesTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeValueInterface(Fixture, TEXT("BPI_Value"));
	UBlueprint* Widget = Fixture.MakeWidgetBlueprint(TEXT("WBP_Sender"));
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Widget Blueprint"), Widget))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	// The widget implements the interface (so the "derived" scope, which for an interface means its implementers, reaches it)
	// and sends the interface message from its event graph.
	TestTrue(TEXT("Widget implements the interface"), FBlueprintEditorUtils::ImplementNewInterface(Widget, FTopLevelAssetPath(InterfaceClass)));
	FFixture::AddMessage(FFixture::EventGraph(Widget), TEXT("GetValue"), InterfaceClass);
	TestTrue(TEXT("Interface saved"), Fixture.SaveAsset(Interface));
	TestTrue(TEXT("Widget saved"), Fixture.SaveAsset(Widget));
	Fixture.RegisterSavedFiles();

	const FString WidgetObjectPath = Fixture.AssetPath(TEXT("WBP_Sender"));
	const FString WidgetPackage = FString::Printf(TEXT("%s/WBP_Sender"), *Fixture.Dir);
	const FString InterfacePath = Interface->GetPathName();

	const TCHAR* const Scopes[] = {TEXT("all"), TEXT("derived")};
	for (const TCHAR* Scope : Scopes)
	{
		const FString Label = FString::Printf(TEXT("[scope %s] "), Scope);

		// Not loaded and no deep scan: the Widget Blueprint is a skipped candidate, reported rather than dropped.
		if (!TestTrue(*(Label + TEXT("Widget unloaded")), FFixture::Unload({Widget}))
			|| !TestNull(*(Label + TEXT("Widget not in memory")), FindObject<UBlueprint>(nullptr, *WidgetObjectPath)))
		{
			return false;
		}
		const FCortexCommandResult Skipped = Run(TEXT("find_usages"), FindUsagesParams(TEXT("GetValue"), InterfacePath, Scope, Fixture.Dir, false));
		if (!TestTrue(*(Label + TEXT("find_usages succeeds")), Skipped.bSuccess))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* NotScanned = nullptr;
		if (TestTrue(*(Label + TEXT("not_scanned reported")), Skipped.Data->TryGetObjectField(TEXT("not_scanned"), NotScanned)))
		{
			TestTrue(*(Label + TEXT("Skipped Widget Blueprint listed")), Strings(*NotScanned, TEXT("paths")).Contains(WidgetPackage));
		}

		// Deep scan loads the candidate and finds the message node.
		const FCortexCommandResult Deep = Run(TEXT("find_usages"), FindUsagesParams(TEXT("GetValue"), InterfacePath, Scope, Fixture.Dir, true));
		if (!TestTrue(*(Label + TEXT("deep find_usages succeeds")), Deep.bSuccess))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Usage = FindUsage(Deep.Data, WidgetObjectPath);
		if (TestNotNull(*(Label + TEXT("Widget Blueprint scanned")), Usage.Get()))
		{
			TestEqual(*(Label + TEXT("one message reference")), CountReferences(Usage, TEXT("message"), TEXT("UK2Node_Message")), 1);
		}

		// The deep scan loaded the widget; the next scope meets it unloaded again.
		Widget = FindObject<UBlueprint>(nullptr, *WidgetObjectPath);
		if (!TestNotNull(*(Label + TEXT("Widget loaded by the deep scan")), Widget))
		{
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindUsagesInterfaceOwnerTest,
	"Cortex.Reflect.FindUsagesInterfaceOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindUsagesInterfaceOwnerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122UsagesTest;
	FFixture Fixture;

	UBlueprint* Interface = MakeValueInterface(Fixture, TEXT("BPI_Value"));
	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_Caller"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Caller"), Caller))
	{
		return false;
	}
	UClass* InterfaceClass = Interface->GeneratedClass;
	UEdGraph* Graph = FFixture::EventGraph(Caller);
	FFixture::AddMessage(Graph, TEXT("GetValue"), InterfaceClass);                                      // (a)
	FFixture::AddTypedCall(Graph, TEXT("GetValue"), UCortexIssue122ReflectCollision::StaticClass());    // (b)
	FFixture::AddCast(Graph, InterfaceClass);                                                           // cast to the BPI
	TestTrue(TEXT("Interface saved"), Fixture.SaveAsset(Interface));
	TestTrue(TEXT("Caller saved"), Fixture.SaveAsset(Caller));
	Fixture.RegisterSavedFiles();
	const FString CallerPath = Caller->GetPathName();

	// The BPI's GetValue: exactly the message site; the same-named call on the unrelated class is not a usage.
	const FCortexCommandResult OnInterface = Run(TEXT("find_usages"), FindUsagesParams(TEXT("GetValue"), Interface->GetPathName(), TEXT("all"), Fixture.Dir, true));
	if (!TestTrue(TEXT("find_usages on the interface succeeds"), OnInterface.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> InterfaceUsage = FindUsage(OnInterface.Data, CallerPath);
	if (TestNotNull(TEXT("Caller reported for the interface"), InterfaceUsage.Get()))
	{
		TestEqual(TEXT("The message node is reported as UK2Node_Message / message"), CountReferences(InterfaceUsage, TEXT("message"), TEXT("UK2Node_Message")), 1);
		TestEqual(TEXT("No typed call is reported for the interface"), CountReferences(InterfaceUsage, TEXT("call"), TEXT("UK2Node_CallFunction")), 0);
		TestEqual(TEXT("The cast to the interface is reported"), CountReferences(InterfaceUsage, TEXT("cast"), TEXT("UK2Node_DynamicCast")), 1);
		TestEqual(TEXT("Nothing else is reported"), InterfaceUsage->GetIntegerField(TEXT("total_count")), 2);
	}
	TestTrue(TEXT("An interface response carries interface_hint"), OnInterface.Data->HasField(TEXT("interface_hint")));

	// The collision class's GetValue: exactly the typed call; the message to the interface is not its usage.
	const FCortexCommandResult OnCollision = Run(TEXT("find_usages"),
		FindUsagesParams(TEXT("GetValue"), TEXT("UCortexIssue122ReflectCollision"), TEXT("all"), Fixture.Dir, true));
	if (!TestTrue(TEXT("find_usages on the collision class succeeds"), OnCollision.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> CollisionUsage = FindUsage(OnCollision.Data, CallerPath);
	if (TestNotNull(TEXT("Caller reported for the collision class"), CollisionUsage.Get()))
	{
		TestEqual(TEXT("The typed call is reported as UK2Node_CallFunction / call"), CountReferences(CollisionUsage, TEXT("call"), TEXT("UK2Node_CallFunction")), 1);
		TestEqual(TEXT("No message is reported for the collision class"), CountReferences(CollisionUsage, TEXT("message"), TEXT("UK2Node_Message")), 0);
		TestEqual(TEXT("Nothing else is reported"), CollisionUsage->GetIntegerField(TEXT("total_count")), 1);
	}
	TestFalse(TEXT("A class response has no interface_hint"), OnCollision.Data->HasField(TEXT("interface_hint")));
	return true;
}
