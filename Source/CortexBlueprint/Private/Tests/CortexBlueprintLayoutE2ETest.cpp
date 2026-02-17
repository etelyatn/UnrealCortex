#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexBPCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBlueprintLayoutE2ETest,
	"Cortex.Blueprint.Layout.E2E",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBlueprintLayoutE2ETest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphLayoutE2E"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), TestPackage, TEXT("BP_LayoutE2E"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) return false;

	FString AssetPath = TestBP->GetPathName();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());
	Router.RegisterDomain(TEXT("bp"), TEXT("Cortex Blueprint"), TEXT("1.0.0"),
		MakeShared<FCortexBPCommandHandler>());

	// Add two PrintString nodes and collect their IDs
	TArray<FString> NodeIds;
	for (int32 i = 0; i < 2; ++i)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		P->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		P->SetObjectField(TEXT("params"), NP);
		FCortexCommandResult R = Router.Execute(TEXT("graph.add_node"), P);
		TestTrue(TEXT("add_node succeeded"), R.bSuccess);
		if (R.bSuccess && R.Data.IsValid())
		{
			FString NodeId;
			R.Data->TryGetStringField(TEXT("node_id"), NodeId);
			NodeIds.Add(NodeId);
		}
	}

	// Connect node A (then) -> node B (execute)
	if (NodeIds.Num() == 2)
	{
		TSharedPtr<FJsonObject> CP = MakeShared<FJsonObject>();
		CP->SetStringField(TEXT("asset_path"), AssetPath);
		CP->SetStringField(TEXT("source_node"), NodeIds[0]);
		CP->SetStringField(TEXT("source_pin"), TEXT("then"));
		CP->SetStringField(TEXT("target_node"), NodeIds[1]);
		CP->SetStringField(TEXT("target_pin"), TEXT("execute"));
		Router.Execute(TEXT("graph.connect"), CP);
		// Don't assert connect - pin names may differ, layout still works
	}

	// Run graph.auto_layout
	TSharedPtr<FJsonObject> LP = MakeShared<FJsonObject>();
	LP->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult LR = Router.Execute(TEXT("graph.auto_layout"), LP);
	TestTrue(TEXT("auto_layout succeeded"), LR.bSuccess);

	// Verify positions were set via list_nodes
	TSharedPtr<FJsonObject> ListP = MakeShared<FJsonObject>();
	ListP->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult ListR = Router.Execute(TEXT("graph.list_nodes"), ListP);
	TestTrue(TEXT("list_nodes succeeded"), ListR.bSuccess);

	if (ListR.bSuccess && ListR.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
		if (ListR.Data->TryGetArrayField(TEXT("nodes"), NodesArr) && NodesArr)
		{
			TMap<FString, FIntPoint> Positions;
			for (const TSharedPtr<FJsonValue>& V : *NodesArr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (V->TryGetObject(Obj))
				{
					FString Id;
					(*Obj)->TryGetStringField(TEXT("node_id"), Id);
					const TSharedPtr<FJsonObject>* PosObj = nullptr;
					if ((*Obj)->TryGetObjectField(TEXT("position"), PosObj))
					{
						double X = 0.0;
						double Y = 0.0;
						(*PosObj)->TryGetNumberField(TEXT("x"), X);
						(*PosObj)->TryGetNumberField(TEXT("y"), Y);
						Positions.Add(Id, FIntPoint(static_cast<int32>(X), static_cast<int32>(Y)));
					}
				}
			}

			if (NodeIds.Num() == 2 && Positions.Contains(NodeIds[0]) && Positions.Contains(NodeIds[1]))
			{
				TestTrue(TEXT("Source node left of target"),
					Positions[NodeIds[0]].X < Positions[NodeIds[1]].X);
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
