// Issue 277 review corrections for the graph commands (G1-G9): split_getter quarantine, split struct pins and orphaned
// outputs, get_authoring_context name validation, add_node placement overflow and the remove_nodes override pre-flight.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "CortexAssetMutationGuard.h"
#include "CortexGraphIssue277TestTypes.h"
#include "CortexGraphTreeTestTypes.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetMathLibrary.h"
#include "Operations/CortexGraphNodeOps.h"
#include "UObject/CoreRedirects.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SavePackage.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphIssue277ReviewTest
{
	FCortexCommandResult ReviewSplit(FCortexCommandRouter& Router, const FString& AssetPath, const FString& NodeId, const bool bDryRun)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		return Router.Execute(TEXT("graph.split_getter"), Params);
	}

	FCortexCommandResult ReviewRemove(FCortexCommandRouter& Router, const UBlueprint* Blueprint, const UEdGraph* Graph, const FString& NodeId)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), Graph->GetName());
		Params->SetArrayField(TEXT("node_ids"), CortexGraphIssue277Test::StringArray({ NodeId }));
		Params->SetBoolField(TEXT("dry_run"), true);
		return Router.Execute(TEXT("graph.remove_nodes"), Params);
	}

	TSharedPtr<FJsonObject> FindOverrideEntry(const FCortexCommandResult& Result, const TCHAR* Function)
	{
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("override_events"), Events) || Events == nullptr)
		{
			return nullptr;
		}
		return CortexGraphIssue277Test::FindObjectByField(*Events, TEXT("function"), Function);
	}

	/** An override K2Node_Event of a native hook added through the editor helper, as a real override is. */
	UK2Node_Event* AddOverride(UBlueprint* Blueprint, UEdGraph* Graph, const TCHAR* Function, UClass* Declaring)
	{
		int32 PosY = 0;
		return FKismetEditorUtilities::AddDefaultEventNode(Blueprint, Graph, FName(Function), Declaring, PosY);
	}

	/** Registers a core redirect for the lifetime of the scope. */
	struct FReviewRedirectScope
	{
		TArray<FCoreRedirect> Redirects;

		explicit FReviewRedirectScope(const TCHAR* OldFunction, const TCHAR* NewFunction)
		{
			const FName ClassName(TEXT("CortexGraphIssue277Actor"));
			const FName PackageName(TEXT("/Script/CortexGraph"));
			Redirects.Emplace(ECoreRedirectFlags::Type_Function,
				FCoreRedirectObjectName(FName(OldFunction), ClassName, PackageName),
				FCoreRedirectObjectName(FName(NewFunction), ClassName, PackageName));
			FCoreRedirects::AddRedirectList(Redirects, TEXT("Cortex.Graph.Issue277.Review"));
		}

		~FReviewRedirectScope()
		{
			FCoreRedirects::RemoveRedirectList(Redirects, TEXT("Cortex.Graph.Issue277.Review"));
		}
	};
}

// ---------------------------------------------------------------------------
// G1: split_getter rechecks quarantine on the loaded Blueprint (a redirector aliases a blocked asset)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewSplitRedirectorTest,
	"Cortex.Graph.Issue277.Review.SplitGetterRedirectorBlocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewSplitRedirectorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewRedirector"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bAliasFlag"), BoolType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bAliasFlag")));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	if (!TestTrue(TEXT("the getter exists"), Getter && Getter->GetValuePin())) { Cleanup(Package, Blueprint); return false; }
	const FString ValuePinName = Getter->GetValuePin()->PinName.ToString();
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdB, TEXT("bPrintToScreen"));

	// A redirector in another package points at the Blueprint, so the requested path is not the blocked one.
	UPackage* AliasPackage = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexGraphFixture_%s/AliasBP"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UObjectRedirector* Redirector = NewObject<UObjectRedirector>(AliasPackage, TEXT("AliasBP"), RF_Public | RF_Standalone);
	Redirector->DestinationObject = Blueprint;
	const FString AliasPath = AliasPackage->GetName() + TEXT(".AliasBP");

	FCortexCommandResult ProbeError;
	const bool bAliasResolves = FCortexGraphNodeOps::LoadBlueprint(AliasPath, ProbeError) == Blueprint;
	if (TestTrue(TEXT("fixture precondition: the loader follows the redirector to the Blueprint"), bAliasResolves))
	{
		FCortexAssetMutationGuard::Block(Blueprint, TEXT("Cortex.Graph.Issue277.Review.SplitGetterRedirectorBlocked fixture"));
		const int32 NodesBefore = Graph->Nodes.Num();
		const FCortexCommandResult Blocked = ReviewSplit(Router, AliasPath, GetterId, false);
		TestFalse(TEXT("a redirector to a blocked Blueprint is refused"), Blocked.bSuccess);
		TestEqual(TEXT("the refusal is INVALID_OPERATION"), Blocked.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestEqual(TEXT("nothing was added"), Graph->Nodes.Num(), NodesBefore);
		TestEqual(TEXT("every link stays on the original getter"), Getter->GetValuePin()->LinkedTo.Num(), 2);
		FCortexAssetMutationGuard::ClearPackage(Package->GetName());
	}

	Redirector->ClearFlags(RF_Standalone);
	Redirector->MarkAsGarbage();
	AliasPackage->MarkAsGarbage();
	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// G2: a split struct output is refused, not reported as zero consumers
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewSplitStructPinTest,
	"Cortex.Graph.Issue277.Review.SplitGetterSplitStructPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewSplitStructPinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewSplitStruct"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("SplitVec"), VectorType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("SplitVec")));
	const FString AbsX = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), StringParams(TEXT("function_name"), TEXT("KismetMathLibrary.Abs")));
	const FString AbsY = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), StringParams(TEXT("function_name"), TEXT("KismetMathLibrary.Abs")));
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	UEdGraphNode* NodeX = FindNode(Graph, AbsX);
	UEdGraphNode* NodeY = FindNode(Graph, AbsY);
	if (!TestTrue(TEXT("the fixture nodes exist"), Getter && NodeX && NodeY && Getter->GetValuePin())) { Cleanup(Package, Blueprint); return false; }

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* ValuePin = Getter->GetValuePin();
	Schema->SplitPin(ValuePin, true);
	if (!TestTrue(TEXT("the vector output is split into sub pins"), ValuePin->SubPins.Num() >= 2)) { Cleanup(Package, Blueprint); return false; }
	const bool bLinked = Schema->TryCreateConnection(ValuePin->SubPins[0], NodeX->FindPin(TEXT("A")))
		&& Schema->TryCreateConnection(ValuePin->SubPins[1], NodeY->FindPin(TEXT("A")));
	if (!TestTrue(TEXT("the split sub pins feed two distinct consumers"), bLinked)) { Cleanup(Package, Blueprint); return false; }
	const int32 NodesBefore = Graph->Nodes.Num();

	for (const bool bDryRun : { true, false })
	{
		const FCortexCommandResult Result = ReviewSplit(Router, AssetPath, GetterId, bDryRun);
		const TCHAR* Mode = bDryRun ? TEXT("dry run") : TEXT("apply");
		TestFalse(FString::Printf(TEXT("%s: a getter with a split output pin is refused, not reported as zero consumers"), Mode), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: the refusal is INVALID_OPERATION"), Mode), Result.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("%s: the refusal tells to recombine the split pin first (%s)"), Mode, *Result.ErrorMessage),
			Result.ErrorMessage.Contains(TEXT("recombine")));
		TestEqual(FString::Printf(TEXT("%s: nothing was added"), Mode), Graph->Nodes.Num(), NodesBefore);
		TestEqual(FString::Printf(TEXT("%s: the split pins keep their links"), Mode), ValuePin->SubPins[0]->LinkedTo.Num() + ValuePin->SubPins[1]->LinkedTo.Num(), 2);
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// G3: a fresh getter that cannot reproduce the output pin (the variable is gone) adds nothing
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewSplitOrphanedOutputTest,
	"Cortex.Graph.Issue277.Review.SplitGetterOrphanedOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewSplitOrphanedOutputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewSplitOrphan"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bOrphanFlag"), BoolType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bOrphanFlag")));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	if (!TestTrue(TEXT("the getter exists"), Getter && Getter->GetValuePin())) { Cleanup(Package, Blueprint); return false; }
	const FString ValuePinName = Getter->GetValuePin()->PinName.ToString();
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdB, TEXT("bPrintToScreen"));

	// The variable disappears from the declaration and the skeleton, while the getter keeps its linked (orphaned) output pin.
	Blueprint->NewVariables.RemoveAll([](const FBPVariableDescription& Description) { return Description.VarName == FName(TEXT("bOrphanFlag")); });
	FKismetEditorUtilities::GenerateBlueprintSkeleton(Blueprint, true);
	if (!TestTrue(TEXT("the original getter still has its linked output"), Getter->GetValuePin() != nullptr && Getter->GetValuePin()->LinkedTo.Num() == 2))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	const int32 NodesBefore = Graph->Nodes.Num();

	const FCortexCommandResult Result = ReviewSplit(Router, AssetPath, GetterId, false);
	TestFalse(TEXT("a split whose clone cannot reproduce the output is refused"), Result.bSuccess);
	TestEqual(TEXT("the refusal is INVALID_OPERATION"), Result.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestEqual(TEXT("no getter was added"), Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("every link stays on the original getter"), Getter->GetValuePin()->LinkedTo.Num(), 2);

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// G4: a supplied graph_name that is not a non-empty string is refused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewContextNameShapeTest,
	"Cortex.Graph.Issue277.Review.AuthoringContextNameShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewContextNameShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphTreeTest;

	UBlueprint* Blueprint = CreateTreeBlueprint(TEXT("BP_Issue277ReviewContextName"));
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const TArray<TPair<const TCHAR*, TSharedPtr<FJsonValue>>> BadValues = {
		{ TEXT("empty string"), MakeShared<FJsonValueString>(FString()) },
		{ TEXT("null"), MakeShared<FJsonValueNull>() },
		{ TEXT("number"), MakeShared<FJsonValueNumber>(7.0) },
		{ TEXT("boolean"), MakeShared<FJsonValueBoolean>(true) },
	};
	for (const TPair<const TCHAR*, TSharedPtr<FJsonValue>>& Bad : BadValues)
	{
		{
			TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
			Params->SetField(TEXT("graph_name"), Bad.Value);
			const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_authoring_context"), Params);
			TestFalse(FString::Printf(TEXT("top-level graph_name %s is refused"), Bad.Key), Result.bSuccess);
			TestEqual(FString::Printf(TEXT("top-level graph_name %s is INVALID_FIELD"), Bad.Key), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		}
		{
			TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
			TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetField(TEXT("graph_name"), Bad.Value);
			Params->SetObjectField(TEXT("target"), Target);
			const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_authoring_context"), Params);
			TestFalse(FString::Printf(TEXT("target.graph_name %s is refused"), Bad.Key), Result.bSuccess);
			TestEqual(FString::Printf(TEXT("target.graph_name %s is INVALID_FIELD"), Bad.Key), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		}
	}

	// graph_ref keeps precedence: a malformed name next to it is not consulted.
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
		Params->SetObjectField(TEXT("target"), Target);
		Params->SetField(TEXT("graph_name"), MakeShared<FJsonValueString>(FString()));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_authoring_context"), Params);
		TestTrue(FString::Printf(TEXT("graph_ref takes precedence over a malformed graph_name: %s"), *Result.ErrorMessage), Result.bSuccess);
	}

	Blueprint->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// G5: placement arithmetic that leaves the int32 range is refused before anything is added
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewPlacementOverflowTest,
	"Cortex.Graph.Issue277.Review.AddNodePlacementOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewPlacementOverflowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewPlacementOverflow"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString AnchorId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_IfThenElse"), nullptr, FString(), FIntPoint(0, 0));
	const int32 NodesBefore = Graph->Nodes.Num();

	struct FOverflowCase
	{
		const TCHAR* Side;
		double Gap;
	};
	const TArray<FOverflowCase> Cases = {
		{ TEXT("right"), 2147483647.0 },
		{ TEXT("below"), 2147483647.0 },
		{ TEXT("left"), 2147483647.0 },
		{ TEXT("above"), 2147483647.0 },
		{ TEXT("right"), 4294967296.0 },
	};
	for (const FOverflowCase& Case : Cases)
	{
		TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
		Placement->SetStringField(TEXT("relative_to"), AnchorId);
		Placement->SetStringField(TEXT("side"), Case.Side);
		Placement->SetNumberField(TEXT("gap"), Case.Gap);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		Params->SetObjectField(TEXT("placement"), Placement);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		const FString Label = FString::Printf(TEXT("%s gap %.0f"), Case.Side, Case.Gap);
		TestFalse(FString::Printf(TEXT("%s: an out-of-range coordinate is refused"), *Label), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: the refusal is INVALID_FIELD"), *Label), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestEqual(FString::Printf(TEXT("%s: nothing was added"), *Label), Graph->Nodes.Num(), NodesBefore);
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// G6: the override pre-flight resolves a copy of the event reference (a preview never rewrites the node)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewOverrideNoMutationTest,
	"Cortex.Graph.Issue277.Review.OverrideEventsNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewOverrideNoMutationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(ACortexGraphIssue277Actor::StaticClass(), TEXT("BP_Issue277ReviewOverrideRedirect"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* Event = AddOverride(Blueprint, EventGraph, TEXT("NativeHook"), ACortexGraphIssue277Actor::StaticClass());
	if (!TestTrue(TEXT("the override exists"), Event != nullptr && Event->bOverrideFunction)) { Cleanup(Package, Blueprint); return false; }

	// The event names the function by a stale name that a core redirect maps to NativeHook.
	FReviewRedirectScope Redirect(TEXT("OldHook277"), TEXT("NativeHook"));
	Event->EventReference.SetExternalMember(FName(TEXT("OldHook277")), ACortexGraphIssue277Actor::StaticClass());
	FMemberReference Probe = Event->EventReference;
	Probe.ResolveMember<UFunction>(Event->GetBlueprintClassFromNode());
	if (!TestEqual(TEXT("fixture precondition: resolving a copy follows the redirect"), Probe.GetMemberName(), FName(TEXT("NativeHook"))))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	TestEqual(TEXT("fixture precondition: the node's own reference is still the stale name"), Event->EventReference.GetMemberName(), FName(TEXT("OldHook277")));

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FCortexCommandResult Preview = ReviewRemove(Router, Blueprint, EventGraph, Event->GetName());
	if (TestTrue(FString::Printf(TEXT("the preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		const TSharedPtr<FJsonObject> Entry = FindOverrideEntry(Preview, TEXT("NativeHook"));
		if (TestTrue(TEXT("the override is reported under its resolved function"), Entry.IsValid()))
		{
			TestEqual(TEXT("declared_in is the fixture class"), Entry->GetStringField(TEXT("declared_in")), ACortexGraphIssue277Actor::StaticClass()->GetPathName());
		}
	}
	TestEqual(TEXT("a preview does not rewrite the live event reference"), Event->EventReference.GetMemberName(), FName(TEXT("OldHook277")));

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// G7: an interface-declared native event has a native default only when the Blueprint's native ancestor implements it
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewOverrideInterfaceDefaultTest,
	"Cortex.Graph.Issue277.Review.OverrideEventsInterfaceDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewOverrideInterfaceDefaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UClass* const InterfaceClass = UCortexGraphIssue277Interface::StaticClass();
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);

	// A Blueprint whose native parent implements the interface natively: the `_Implementation` is reachable.
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CreateBlueprint(ACortexGraphIssue277InterfaceActor::StaticClass(), TEXT("BP_Issue277ReviewInterfaceNative"), Package);
		if (TestNotNull(TEXT("native-implementer fixture created"), Blueprint))
		{
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			UK2Node_Event* Event = AddOverride(Blueprint, EventGraph, TEXT("InterfaceHook"), InterfaceClass);
			if (TestTrue(TEXT("the interface override exists"), Event != nullptr))
			{
				const FCortexCommandResult Preview = ReviewRemove(Router, Blueprint, EventGraph, Event->GetName());
				const TSharedPtr<FJsonObject> Entry = Preview.bSuccess ? FindOverrideEntry(Preview, TEXT("InterfaceHook")) : nullptr;
				if (TestTrue(FString::Printf(TEXT("the native-implementer preview reports the override: %s"), *Preview.ErrorMessage), Entry.IsValid()))
				{
					TestEqual(TEXT("declared_in is the interface"), Entry->GetStringField(TEXT("declared_in")), InterfaceClass->GetPathName());
					TestTrue(TEXT("native_declaration"), Entry->GetBoolField(TEXT("native_declaration")));
					TestTrue(TEXT("a native ancestor implementing the interface has a native default"), Entry->GetBoolField(TEXT("has_native_default")));
				}
			}
		}
		Cleanup(Package, Blueprint);
	}

	// A Blueprint that implements the interface only in Blueprint: no `_Implementation` is reachable.
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewInterfaceBlueprintOnly"), Package);
		if (TestNotNull(TEXT("blueprint-only implementer fixture created"), Blueprint)
			&& TestTrue(TEXT("the Blueprint implements the interface"), FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(InterfaceClass))))
		{
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			UK2Node_Event* Event = AddOverride(Blueprint, EventGraph, TEXT("InterfaceHook"), InterfaceClass);
			if (TestTrue(TEXT("the interface override exists"), Event != nullptr))
			{
				const FCortexCommandResult Preview = ReviewRemove(Router, Blueprint, EventGraph, Event->GetName());
				const TSharedPtr<FJsonObject> Entry = Preview.bSuccess ? FindOverrideEntry(Preview, TEXT("InterfaceHook")) : nullptr;
				if (TestTrue(FString::Printf(TEXT("the blueprint-only preview reports the override: %s"), *Preview.ErrorMessage), Entry.IsValid()))
				{
					TestTrue(TEXT("native_declaration"), Entry->GetBoolField(TEXT("native_declaration")));
					TestFalse(TEXT("a Blueprint-only implementation has no native default to fall back to"), Entry->GetBoolField(TEXT("has_native_default")));
				}
			}
		}
		Cleanup(Package, Blueprint);
	}
	return true;
}

// ---------------------------------------------------------------------------
// G8: a referencer whose package is resident but whose Blueprint is not loaded is not "loaded"
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewOverrideResidentPackageTest,
	"Cortex.Graph.Issue277.Review.OverrideEventsResidentPackageUnloadedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewOverrideResidentPackageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	// A saved Blueprint deriving from the fixture actor references the declaring /Script package.
	FString OwnedReferencerName;
	UPackage* ReferencerPackage = CreateCortexGraphTestPackage(TEXT("BP_Issue277ResidentReferencer"), OwnedReferencerName);
	UBlueprint* Referencer = FKismetEditorUtilities::CreateBlueprint(
		ACortexGraphIssue277Actor::StaticClass(), ReferencerPackage, FName(TEXT("BP_Issue277ResidentReferencer")), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("referencer Blueprint created"), Referencer)) { return false; }
	FKismetEditorUtilities::CompileBlueprint(Referencer);

	const FString ReferencerPackageName = ReferencerPackage->GetName();
	const FString ReferencerFilename = FPackageName::LongPackageNameToFilename(ReferencerPackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(ReferencerPackage, Referencer, *ReferencerFilename, SaveArgs);
	IAssetRegistry* const AssetRegistry = IAssetRegistry::Get();
	if (bSaved && AssetRegistry != nullptr)
	{
		AssetRegistry->ScanFilesSynchronous(TArray<FString>{ ReferencerFilename }, true);
	}

	// Keep the package resident while its Blueprint object is unloaded.
	ReferencerPackage->AddToRoot();
	const FString ReferencerObjectPath = Referencer->GetPathName();
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphIssue277ResidentPackage")));
	}
	Referencer->ClearFlags(RF_Standalone | RF_Public);
	Referencer->MarkAsGarbage();
	Referencer = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	TArray<FName> ScriptReferencers;
	if (AssetRegistry != nullptr)
	{
		AssetRegistry->GetReferencers(FName(TEXT("/Script/CortexGraph")), ScriptReferencers, UE::AssetRegistry::EDependencyCategory::Package);
	}
	const bool bFixtureReady = bSaved && AssetRegistry != nullptr
		&& FindPackage(nullptr, *ReferencerPackageName) != nullptr
		&& FindObject<UObject>(nullptr, *ReferencerObjectPath) == nullptr
		&& ScriptReferencers.Contains(FName(*ReferencerPackageName));
	if (TestTrue(TEXT("fixture precondition: a saved referencer of /Script/CortexGraph with a resident package and an unloaded Blueprint"), bFixtureReady))
	{
		UPackage* TargetPackage = nullptr;
		UBlueprint* Target = CreateBlueprint(ACortexGraphIssue277Actor::StaticClass(), TEXT("BP_Issue277ReviewResidentTarget"), TargetPackage);
		if (TestNotNull(TEXT("target Blueprint created"), Target))
		{
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Target);
			UK2Node_Event* Event = AddOverride(Target, EventGraph, TEXT("NativeHook"), ACortexGraphIssue277Actor::StaticClass());
			FCortexCommandRouter Router;
			RegisterIssue277GraphDomain(Router);
			const FCortexCommandResult Preview = Event != nullptr ? ReviewRemove(Router, Target, EventGraph, Event->GetName()) : FCortexCommandResult();
			const TSharedPtr<FJsonObject> Entry = Preview.bSuccess ? FindOverrideEntry(Preview, TEXT("NativeHook")) : nullptr;
			if (TestTrue(FString::Printf(TEXT("the preview reports NativeHook: %s"), *Preview.ErrorMessage), Entry.IsValid()))
			{
				TestFalse(TEXT("a resident package with an unloaded Blueprint cannot be claimed inspected"),
					Entry->GetBoolField(TEXT("blueprint_callers_complete")));
				const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
				bool bListed = false;
				if (Entry->TryGetArrayField(TEXT("unloaded_referencers"), Listed) && Listed != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Value : *Listed)
					{
						bListed |= Value.IsValid() && Value->AsString() == ReferencerPackageName;
					}
				}
				TestTrue(TEXT("the resident-but-unloaded referencer is listed"), bListed);
			}
		}
		Cleanup(TargetPackage, Target);
	}

	ReferencerPackage->RemoveFromRoot();
	ReferencerPackage->ClearFlags(RF_Standalone);
	ReferencerPackage->MarkAsGarbage();
	DeleteCortexGraphTestPackageFile(nullptr, OwnedReferencerName);
	return true;
}

// ---------------------------------------------------------------------------
// G9: an unresolved declaration is reported as such, with no name-only callers and no completeness claim
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewOverrideUnresolvedTest,
	"Cortex.Graph.Issue277.Review.OverrideEventsUnresolvedDeclaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewOverrideUnresolvedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* TargetPackage = nullptr;
	UBlueprint* Target = CreateBlueprint(ACortexGraphIssue277Actor::StaticClass(), TEXT("BP_Issue277ReviewUnresolvedTarget"), TargetPackage);
	UPackage* CallerPackage = nullptr;
	UBlueprint* Caller = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewUnresolvedCaller"), CallerPackage);
	if (!TestTrue(TEXT("fixture Blueprints created"), Target != nullptr && Caller != nullptr))
	{
		Cleanup(TargetPackage, Target);
		Cleanup(CallerPackage, Caller);
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Target);
	UEdGraph* CallerGraph = FBlueprintEditorUtils::FindEventGraph(Caller);

	// An override whose signature no longer resolves anywhere.
	UK2Node_Event* Event = NewObject<UK2Node_Event>(EventGraph);
	Event->EventReference.SetExternalMember(FName(TEXT("MissingHook277")), ACortexGraphIssue277Actor::StaticClass());
	Event->bOverrideFunction = true;
	Event->CreateNewGuid();
	EventGraph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();

	// A same-named call on an unrelated Blueprint must not be reported as a caller of the stale override.
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(CallerGraph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("MissingHook277")), AActor::StaticClass());
	Call->CreateNewGuid();
	CallerGraph->AddNode(Call, false, false);

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FCortexCommandResult Preview = ReviewRemove(Router, Target, EventGraph, Event->GetName());
	const TSharedPtr<FJsonObject> Entry = Preview.bSuccess ? FindOverrideEntry(Preview, TEXT("MissingHook277")) : nullptr;
	if (TestTrue(FString::Printf(TEXT("the unresolved override is reported: %s"), *Preview.ErrorMessage), Entry.IsValid()))
	{
		bool bResolved = true;
		TestTrue(TEXT("declaration_resolved is published"), Entry->TryGetBoolField(TEXT("declaration_resolved"), bResolved));
		TestFalse(TEXT("declaration_resolved is false"), bResolved);
		TestTrue(TEXT("declared_in is empty"), Entry->GetStringField(TEXT("declared_in")).IsEmpty());
		TestFalse(TEXT("native_declaration is false"), Entry->GetBoolField(TEXT("native_declaration")));
		TestFalse(TEXT("has_native_default is false"), Entry->GetBoolField(TEXT("has_native_default")));
		const TArray<TSharedPtr<FJsonValue>>* Callers = nullptr;
		if (TestTrue(TEXT("blueprint_callers is published"), Entry->TryGetArrayField(TEXT("blueprint_callers"), Callers) && Callers != nullptr))
		{
			TestEqual(TEXT("no name-only caller is claimed for an unresolved declaration"), Callers->Num(), 0);
		}
		TestFalse(TEXT("no completeness is claimed for an unresolved declaration"), Entry->GetBoolField(TEXT("blueprint_callers_complete")));
	}

	Cleanup(TargetPackage, Target);
	Cleanup(CallerPackage, Caller);
	return true;
}

// ---------------------------------------------------------------------------
// H1: an out-of-range left/above placement is refused before a composite node (and its bound graph) is created
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewPlacementCompositeTest,
	"Cortex.Graph.Issue277.Review.AddNodePlacementCompositeOutOfRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewPlacementCompositeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewPlacementComposite"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString AnchorId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_IfThenElse"), nullptr, FString(), FIntPoint(0, 0));

	const int32 NodesBefore = Graph->Nodes.Num();
	const int32 SubGraphsBefore = Graph->SubGraphs.Num();
	TArray<UEdGraph*> GraphsBefore;
	Blueprint->GetAllGraphs(GraphsBefore);

	for (const TCHAR* Side : { TEXT("left"), TEXT("above") })
	{
		TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
		Placement->SetStringField(TEXT("relative_to"), AnchorId);
		Placement->SetStringField(TEXT("side"), Side);
		Placement->SetNumberField(TEXT("gap"), 2147483647.0);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_Composite"));
		Params->SetObjectField(TEXT("placement"), Placement);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);

		TestFalse(FString::Printf(TEXT("%s: an out-of-range composite placement is refused"), Side), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: the refusal is INVALID_FIELD"), Side), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestEqual(FString::Printf(TEXT("%s: no node was added"), Side), Graph->Nodes.Num(), NodesBefore);
		TestEqual(FString::Printf(TEXT("%s: no orphaned bound graph was left in the graph's SubGraphs"), Side), Graph->SubGraphs.Num(), SubGraphsBefore);
		TArray<UEdGraph*> GraphsAfter;
		Blueprint->GetAllGraphs(GraphsAfter);
		TestEqual(FString::Printf(TEXT("%s: the Blueprint gained no graph"), Side), GraphsAfter.Num(), GraphsBefore.Num());
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// H3: a call that still carries a redirected function name is matched through a resolved copy of its reference
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277ReviewOverrideCallerRedirectTest,
	"Cortex.Graph.Issue277.Review.OverrideEventsCallerRedirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277ReviewOverrideCallerRedirectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277ReviewTest;

	UPackage* TargetPackage = nullptr;
	UBlueprint* Target = CreateBlueprint(ACortexGraphIssue277Actor::StaticClass(), TEXT("BP_Issue277ReviewCallerRedirectTarget"), TargetPackage);
	UPackage* CallerPackage = nullptr;
	UBlueprint* Caller = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277ReviewCallerRedirectCaller"), CallerPackage);
	if (!TestTrue(TEXT("fixture Blueprints created"), Target != nullptr && Caller != nullptr))
	{
		Cleanup(TargetPackage, Target);
		Cleanup(CallerPackage, Caller);
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Target);
	UEdGraph* CallerGraph = FBlueprintEditorUtils::FindEventGraph(Caller);
	UK2Node_Event* Event = AddOverride(Target, EventGraph, TEXT("NativeHook"), ACortexGraphIssue277Actor::StaticClass());
	if (!TestNotNull(TEXT("the override exists"), Event))
	{
		Cleanup(TargetPackage, Target);
		Cleanup(CallerPackage, Caller);
		return false;
	}

	// The call names the function by a stale name that a core redirect maps to NativeHook.
	FReviewRedirectScope Redirect(TEXT("OldCallHook277"), TEXT("NativeHook"));
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(CallerGraph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("OldCallHook277")), ACortexGraphIssue277Actor::StaticClass());
	Call->CreateNewGuid();
	CallerGraph->AddNode(Call, false, false);
	FMemberReference Probe = Call->FunctionReference;
	Probe.ResolveMember<UFunction>(Caller->SkeletonGeneratedClass);
	if (!TestEqual(TEXT("fixture precondition: resolving a copy of the call's reference follows the redirect"),
		Probe.GetMemberName(), FName(TEXT("NativeHook"))))
	{
		Cleanup(TargetPackage, Target);
		Cleanup(CallerPackage, Caller);
		return false;
	}

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FCortexCommandResult Preview = ReviewRemove(Router, Target, EventGraph, Event->GetName());
	const TSharedPtr<FJsonObject> Entry = Preview.bSuccess ? FindOverrideEntry(Preview, TEXT("NativeHook")) : nullptr;
	if (TestTrue(FString::Printf(TEXT("the preview reports NativeHook: %s"), *Preview.ErrorMessage), Entry.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Callers = nullptr;
		if (TestTrue(TEXT("blueprint_callers is published"), Entry->TryGetArrayField(TEXT("blueprint_callers"), Callers) && Callers != nullptr))
		{
			const TSharedPtr<FJsonObject> Found = FindObjectByField(*Callers, TEXT("node_guid"), Call->NodeGuid.ToString());
			if (TestTrue(TEXT("a call carrying the redirected old name is a caller"), Found.IsValid()))
			{
				TestEqual(TEXT("the caller's asset"), Found->GetStringField(TEXT("asset_path")), Caller->GetPathName());
			}
		}
	}
	TestEqual(TEXT("the scan does not rewrite the call's own reference"), Call->FunctionReference.GetMemberName(), FName(TEXT("OldCallHook277")));

	Cleanup(TargetPackage, Target);
	Cleanup(CallerPackage, Caller);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
