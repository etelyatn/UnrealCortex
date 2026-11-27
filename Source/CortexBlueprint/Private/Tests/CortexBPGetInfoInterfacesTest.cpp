// #277 item 6: blueprint.get_info lists the interfaces a Blueprint implements, with their implementation graphs.

#include "Misc/AutomationTest.h"
#include "UObject/Interface.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Unity-safe: block-scope using-declarations beat same-named anonymous-namespace helpers of other test files in one blob.
#define CORTEX_277_GETINFO_USE_HELPERS \
	using CortexIssue277GetInfoInterfacesTest::FScope; \
	using CortexIssue277GetInfoInterfacesTest::MakeInterface; \
	using CortexIssue277GetInfoInterfacesTest::GetInfo; \
	using CortexIssue277GetInfoInterfacesTest::InterfaceRows; \
	using CortexIssue277GetInfoInterfacesTest::StringSet; \
	using CortexIssue277GetInfoInterfacesTest::StringField;

namespace CortexIssue277GetInfoInterfacesTest
{
	struct FScope
	{
		TArray<UBlueprint*> Blueprints;
		FScope() = default;
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;
		~FScope()
		{
			CortexAdoptTest::ResetUndoHistory();
			for (UBlueprint* Blueprint : Blueprints)
			{
				CortexAdoptTest::Discard(Blueprint);
			}
		}
	};

	/** A compiled BPI with one value-returning function (implemented as an interface function graph). */
	UBlueprint* MakeInterface(FScope& Scope, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(), Package, FName(Name), BPTYPE_Interface, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Interface == nullptr)
		{
			return nullptr;
		}
		Scope.Blueprints.Add(Interface);
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Interface, TEXT("GetValue"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, Graph, true, nullptr);
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Entry == nullptr)
			{
				Entry = Cast<UK2Node_FunctionEntry>(Node);
			}
		}
		if (Entry == nullptr)
		{
			return nullptr;
		}
		FEdGraphPinType DoubleType;
		DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
		DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
		Result->FunctionReference = Entry->FunctionReference;
		Result->CreateNewGuid();
		Result->AllocateDefaultPins();
		Graph->AddNode(Result, true, false);
		Result->CreateUserDefinedPin(TEXT("Value"), DoubleType, EGPD_Input, false);
		FKismetEditorUtilities::CompileBlueprint(Interface);
		return Interface;
	}

	FCortexCommandResult GetInfo(UBlueprint* Blueprint)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		return Handler.Execute(TEXT("get_info"), Params);
	}

	/** The `interfaces` rows of a get_info response (empty when the field is absent or empty). */
	TArray<TSharedPtr<FJsonObject>> InterfaceRows(const TSharedPtr<FJsonObject>& Data)
	{
		TArray<TSharedPtr<FJsonObject>> Rows;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(TEXT("interfaces"), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr)
				{
					Rows.Add(Row);
				}
			}
		}
		return Rows;
	}

	TSet<FString> StringSet(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TSet<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Values.Add(Value->AsString());
			}
		}
		return Values;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoInterfacesTest,
	"Cortex.Blueprint.GetInfo.Interfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGetInfoInterfacesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_GETINFO_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_GetInfoInterfaces"));
	UBlueprint* Implementer = CortexAdoptTest::MakeBlueprint(TEXT("BP_GetInfoImplementer"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestNotNull(TEXT("Implementer"), Implementer))
	{
		return false;
	}
	Scope.Blueprints.Add(Implementer);
	if (!TestTrue(TEXT("Blueprint interface implemented"),
			FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(Interface->GeneratedClass)))
		|| !TestTrue(TEXT("Native interface implemented"),
			FBlueprintEditorUtils::ImplementNewInterface(Implementer, FTopLevelAssetPath(UCortexBPAdoptInterface::StaticClass())))
		|| !TestEqual(TEXT("Two interfaces implemented"), Implementer->ImplementedInterfaces.Num(), 2))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Implementer);

	UBlueprint* Child = CortexAdoptTest::MakeBlueprint(TEXT("BP_GetInfoChild"), Implementer->GeneratedClass);
	UBlueprint* Plain = CortexAdoptTest::MakeBlueprint(TEXT("BP_GetInfoPlain"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Child of the implementer"), Child) || !TestNotNull(TEXT("Plain Blueprint"), Plain))
	{
		return false;
	}
	Scope.Blueprints.Add(Child);
	Scope.Blueprints.Add(Plain);

	const FCortexCommandResult Info = GetInfo(Implementer);
	if (!TestTrue(FString::Printf(TEXT("get_info succeeds: %s"), *Info.ErrorMessage), Info.bSuccess) || !Info.Data.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Rows = InterfaceRows(Info.Data);
	if (!TestEqual(TEXT("one row per implemented interface"), Rows.Num(), 2))
	{
		return false;
	}

	// Rows follow ImplementedInterfaces, so each is checked against its live description.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FBPInterfaceDescription& Description = Implementer->ImplementedInterfaces[Index];
		const TSharedPtr<FJsonObject>& Row = Rows[Index];
		const bool bBlueprintInterface = Cast<UBlueprintGeneratedClass>(Description.Interface.Get()) != nullptr;

		TestTrue(FString::Printf(TEXT("row %d name identifies the interface"), Index), StringField(Row, TEXT("name")).Contains(
			bBlueprintInterface ? TEXT("BPI_GetInfoInterfaces") : TEXT("CortexBPAdoptInterface")));
		TestFalse(FString::Printf(TEXT("row %d path"), Index), StringField(Row, TEXT("path")).IsEmpty());
		TestTrue(FString::Printf(TEXT("row %d path names the interface"), Index),
			StringField(Row, TEXT("path")).Contains(bBlueprintInterface ? Interface->GetOutermost()->GetName() : TEXT("CortexBPAdoptInterface")));
		bool bIsBlueprint = !bBlueprintInterface;
		TestTrue(FString::Printf(TEXT("row %d is_blueprint is reported"), Index), Row->TryGetBoolField(TEXT("is_blueprint"), bIsBlueprint));
		TestEqual(FString::Printf(TEXT("row %d is_blueprint"), Index), bIsBlueprint, bBlueprintInterface);

		TSet<FString> ExpectedGraphs;
		for (const UEdGraph* Graph : Description.Graphs)
		{
			if (Graph != nullptr)
			{
				ExpectedGraphs.Add(Graph->GetName());
			}
		}
		const TSet<FString> ReportedGraphs = StringSet(Row, TEXT("implementation_graphs"));
		TestTrue(FString::Printf(TEXT("row %d implementation_graphs are the interface function graphs"), Index),
			ReportedGraphs.Difference(ExpectedGraphs).IsEmpty() && ExpectedGraphs.Difference(ReportedGraphs).IsEmpty());
		if (bBlueprintInterface)
		{
			TestTrue(TEXT("the Blueprint interface's GetValue is an implementation graph"), ReportedGraphs.Contains(TEXT("GetValue")));
		}
	}

	// Interfaces the parent implements are inherited, not listed on the child; a Blueprint without any lists none.
	const FCortexCommandResult ChildInfo = GetInfo(Child);
	if (TestTrue(TEXT("get_info on the child succeeds"), ChildInfo.bSuccess))
	{
		TestEqual(TEXT("an inherited interface is not listed"), InterfaceRows(ChildInfo.Data).Num(), 0);
	}
	const FCortexCommandResult PlainInfo = GetInfo(Plain);
	if (TestTrue(TEXT("get_info on a plain Blueprint succeeds"), PlainInfo.bSuccess))
	{
		TestEqual(TEXT("no interfaces"), InterfaceRows(PlainInfo.Data).Num(), 0);
	}
	return true;
}

#undef CORTEX_277_GETINFO_USE_HELPERS
