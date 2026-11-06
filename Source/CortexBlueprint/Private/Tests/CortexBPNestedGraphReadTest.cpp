#include "Misc/AutomationTest.h"

#include "CortexBPCommandHandler.h"
#include "CortexBPNestedGraphTestTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CortexBPNestedGraphReadTestSupport
{
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* HostFn = nullptr;
		UCortexBPNestedTestGraph* Nested = nullptr;
		UK2Node_CallFunction* RootCall = nullptr;
		UK2Node_CallFunction* NestedCall = nullptr;
		UK2Node_CustomEvent* NestedEvent = nullptr;
		UK2Node_CallFunction* OrphanCall = nullptr;
	};

	UBlueprint* NewBlueprint(const TCHAR* BaseName)
	{
		const FString UniqueName = FString::Printf(TEXT("%s_%s"), BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), FName(*UniqueName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	UEdGraph* AddFunctionGraph(UBlueprint* Blueprint)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("HostFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Blueprint->FunctionGraphs.Add(Graph);
		return Graph;
	}

	UK2Node_CallFunction* AddPrintString(UEdGraph* Graph, const TCHAR* InString)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->CreateNewGuid();
		Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
		Graph->AddNode(Call, false, false);
		Call->AllocateDefaultPins();
		if (InString != nullptr)
		{
			Call->FindPinChecked(TEXT("InString"))->DefaultValue = InString;
		}
		return Call;
	}

	/** HostFn (function graph) holds RootCall and a host node whose GetSubGraphs() returns the nested graph "NestedBody". */
	bool BuildFixture(FFixture& OutFixture)
	{
		OutFixture.Blueprint = NewBlueprint(TEXT("BP_NestedGraphRead"));
		if (OutFixture.Blueprint == nullptr)
		{
			return false;
		}
		OutFixture.HostFn = AddFunctionGraph(OutFixture.Blueprint);
		OutFixture.RootCall = AddPrintString(OutFixture.HostFn, TEXT("RootPinNeedle"));

		UCortexBPNestedTestHostNode* Host = NewObject<UCortexBPNestedTestHostNode>(OutFixture.HostFn);
		Host->CreateNewGuid();
		OutFixture.HostFn->AddNode(Host, false, false);
		Host->AllocateDefaultPins();

		OutFixture.Nested = NewObject<UCortexBPNestedTestGraph>(Host, TEXT("NestedBody"), RF_Transactional);
		OutFixture.Nested->Schema = UEdGraphSchema_K2::StaticClass();
		OutFixture.Nested->GraphGuid = FGuid::NewGuid();
		Host->HostedGraphs.Add(OutFixture.Nested);

		OutFixture.NestedEvent = NewObject<UK2Node_CustomEvent>(OutFixture.Nested);
		OutFixture.NestedEvent->CreateNewGuid();
		OutFixture.NestedEvent->CustomFunctionName = TEXT("NestedEvt");
		OutFixture.Nested->AddNode(OutFixture.NestedEvent, false, false);
		OutFixture.NestedEvent->AllocateDefaultPins();
		OutFixture.NestedCall = AddPrintString(OutFixture.Nested, TEXT("NestedPinNeedle"));
		OutFixture.NestedEvent->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(OutFixture.NestedCall->GetExecPin());
		OutFixture.OrphanCall = AddPrintString(OutFixture.Nested, nullptr);
		return true;
	}

	FString Serialize(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	TArray<TSharedPtr<FJsonObject>> ObjectsOf(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Result;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Field, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				if (Value.IsValid() && Value->AsObject().IsValid())
				{
					Result.Add(Value->AsObject());
				}
			}
		}
		return Result;
	}

	TSharedPtr<FJsonObject> Find(const TArray<TSharedPtr<FJsonObject>>& Objects, const TCHAR* Field, const FString& Value)
	{
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			FString Actual;
			if (Object->TryGetStringField(Field, Actual) && Actual == Value)
			{
				return Object;
			}
		}
		return nullptr;
	}

	FCortexCommandResult RunSearch(UBlueprint* Blueprint, const TCHAR* Query)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("query"), Query);
		TArray<TSharedPtr<FJsonValue>> SearchIn;
		SearchIn.Add(MakeShared<FJsonValueString>(TEXT("pins")));
		Params->SetArrayField(TEXT("search_in"), SearchIn);
		return Handler.Execute(TEXT("search"), Params);
	}

	FCortexCommandResult RunMetrics(UBlueprint* Blueprint)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		return Handler.Execute(TEXT("graph_metrics"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPNestedGraphReadTest,
	"Cortex.Blueprint.NestedGraphRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPNestedGraphReadTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPNestedGraphReadTestSupport;

	FFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildFixture(Fixture)))
	{
		return false;
	}
	const FString NestedGuid = Fixture.Nested->GraphGuid.ToString();

	// ── blueprint.search finds a literal pin default inside the nested graph and reports its graph_guid ──
	{
		const FCortexCommandResult Result = RunSearch(Fixture.Blueprint, TEXT("PinNeedle"));
		if (TestTrue(FString::Printf(TEXT("search succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TArray<TSharedPtr<FJsonObject>> Matches = ObjectsOf(Result.Data, TEXT("matches"));
			TestEqual(TEXT("the root hit and the nested hit are both found"), Matches.Num(), 2);

			const TSharedPtr<FJsonObject> Nested = Find(Matches, TEXT("value"), TEXT("NestedPinNeedle"));
			if (TestTrue(TEXT("the nested literal is found"), Nested.IsValid()))
			{
				TestEqual(TEXT("nested match reports graph_guid"), Nested->GetStringField(TEXT("graph_guid")), NestedGuid);
				TestEqual(TEXT("nested match reports its graph_name"), Nested->GetStringField(TEXT("graph_name")), FString(TEXT("NestedBody")));
				TestEqual(TEXT("nested match reports its node_id"), Nested->GetStringField(TEXT("node_id")), Fixture.NestedCall->GetName());
			}

			const TSharedPtr<FJsonObject> Root = Find(Matches, TEXT("value"), TEXT("RootPinNeedle"));
			if (TestTrue(TEXT("the root literal is found"), Root.IsValid()))
			{
				TestFalse(TEXT("a root-graph match keeps its existing shape (no graph_guid)"), Root->HasField(TEXT("graph_guid")));
				TestEqual(TEXT("root match graph_name"), Root->GetStringField(TEXT("graph_name")), FString(TEXT("HostFn")));
			}
		}
	}

	// ── blueprint.graph_metrics counts the nested graph's nodes ──
	{
		const FCortexCommandResult Result = RunMetrics(Fixture.Blueprint);
		if (TestTrue(FString::Printf(TEXT("graph_metrics succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Totals = Result.Data->GetObjectField(TEXT("totals"));
			TestEqual(TEXT("the nested call counts as functional logic"), Totals->GetNumberField(TEXT("functional_nodes")), 1.0);

			const TArray<TSharedPtr<FJsonObject>> Entries = ObjectsOf(Result.Data, TEXT("entries"));
			const TSharedPtr<FJsonObject> Event = Find(Entries, TEXT("name"), TEXT("NestedEvt"));
			if (TestTrue(TEXT("the nested event is an entry"), Event.IsValid()))
			{
				TestEqual(TEXT("entry graph"), Event->GetStringField(TEXT("graph")), FString(TEXT("NestedBody")));
				TestEqual(TEXT("entry graph_guid"), Event->GetStringField(TEXT("graph_guid")), NestedGuid);
				TestEqual(TEXT("entry functional_nodes"), Event->GetNumberField(TEXT("functional_nodes")), 1.0);
				TestTrue(TEXT("entry is live"), Event->GetBoolField(TEXT("live")));
			}

			const TArray<TSharedPtr<FJsonObject>> Unreachable = ObjectsOf(Result.Data, TEXT("unreachable"));
			const TSharedPtr<FJsonObject> Row = Find(Unreachable, TEXT("graph"), TEXT("NestedBody"));
			if (TestTrue(TEXT("the orphan in the nested graph is reported unreachable"), Row.IsValid()))
			{
				TestEqual(TEXT("one orphan"), Row->GetNumberField(TEXT("count")), 1.0);
				TestEqual(TEXT("unreachable row graph_guid"), Row->GetStringField(TEXT("graph_guid")), NestedGuid);
				const TArray<TSharedPtr<FJsonValue>>* NodeIds = nullptr;
				if (TestTrue(TEXT("node_ids listed"), Row->TryGetArrayField(TEXT("node_ids"), NodeIds) && NodeIds && NodeIds->Num() == 1))
				{
					TestEqual(TEXT("the orphan node id is listed"), (*NodeIds)[0]->AsString(), Fixture.OrphanCall->GetName());
				}
			}

			int32 RowTotal = 0;
			for (const TSharedPtr<FJsonObject>& Item : Unreachable)
			{
				RowTotal += static_cast<int32>(Item->GetNumberField(TEXT("count")));
			}
			TestEqual(TEXT("totals.unreachable_nodes is the sum of the rows"), Totals->GetNumberField(TEXT("unreachable_nodes")), static_cast<double>(RowTotal));
		}
	}
	Fixture.Blueprint->MarkAsGarbage();

	// ── A Blueprint without nested graphs gains no graph_guid anywhere ──
	{
		UBlueprint* Plain = NewBlueprint(TEXT("BP_NestedGraphReadPlain"));
		if (!TestNotNull(TEXT("plain Blueprint"), Plain))
		{
			return false;
		}
		UEdGraph* PlainFn = AddFunctionGraph(Plain);
		AddPrintString(PlainFn, TEXT("PlainPinNeedle"));

		const FCortexCommandResult Search = RunSearch(Plain, TEXT("PlainPinNeedle"));
		if (TestTrue(TEXT("plain search succeeds"), Search.bSuccess))
		{
			TestEqual(TEXT("one match"), ObjectsOf(Search.Data, TEXT("matches")).Num(), 1);
			TestFalse(TEXT("search output carries no graph_guid"), Serialize(Search.Data).Contains(TEXT("graph_guid")));
		}
		const FCortexCommandResult Metrics = RunMetrics(Plain);
		if (TestTrue(TEXT("plain graph_metrics succeeds"), Metrics.bSuccess))
		{
			TestFalse(TEXT("graph_metrics output carries no graph_guid"), Serialize(Metrics.Data).Contains(TEXT("graph_guid")));
		}
		Plain->MarkAsGarbage();
	}

	return true;
}
