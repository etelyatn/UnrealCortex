#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexGraphNodeOps.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue #234: graph.disconnect accepts graph.connect's source_node/source_pin/target_node/target_pin shape and then
 * breaks only that one link, leaving the pins' other links intact. The node_id/pin_name shape still breaks every link.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDisconnectLinkTest,
	"Cortex.Graph.DisconnectLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

namespace
{
	FString AddPrintString(FCortexCommandRouter& Router, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		FString NodeId;
		if (Result.bSuccess && Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}
		return NodeId;
	}

	TSharedPtr<FJsonObject> LinkParams(const FString& AssetPath, const FString& SourceNode, const FString& TargetNode)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), SourceNode);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), TargetNode);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));
		return Params;
	}

	UEdGraphPin* FindPin(UEdGraph* Graph, const FString& NodeId, const TCHAR* PinName)
	{
		FCortexCommandResult Error;
		UEdGraphNode* Node = FCortexGraphNodeOps::FindNode(Graph, NodeId, Error);
		return Node != nullptr ? Node->FindPin(FName(PinName)) : nullptr;
	}
}

bool FCortexGraphDisconnectLinkTest::RunTest(const FString& Parameters)
{
	// No PKG_PlayInEditor: objects in PIE packages are excluded from the transaction buffer, and undo is asserted.
	UPackage* TestPackage = CreatePackage(TEXT("/Game/Temp/CortexGraphDisconnectLinkTest"));
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		FName(TEXT("BP_CortexGraphTest_DisconnectLink")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Test Blueprint should be created"), TestBP))
	{
		TestPackage->MarkAsGarbage();
		return false;
	}
	ON_SCOPE_EXIT
	{
		TestBP->MarkAsGarbage();
		TestPackage->MarkAsGarbage();
	};

	const FString AssetPath = TestBP->GetPathName();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

	// A.then -> B.execute and C.then -> B.execute: B.execute carries two links.
	const FString NodeA = AddPrintString(Router, AssetPath);
	const FString NodeB = AddPrintString(Router, AssetPath);
	const FString NodeC = AddPrintString(Router, AssetPath);
	if (!TestTrue(TEXT("three nodes were added"), !NodeA.IsEmpty() && !NodeB.IsEmpty() && !NodeC.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("connect A -> B"), Router.Execute(TEXT("graph.connect"), LinkParams(AssetPath, NodeA, NodeB)).bSuccess);
	TestTrue(TEXT("connect C -> B"), Router.Execute(TEXT("graph.connect"), LinkParams(AssetPath, NodeC, NodeB)).bSuccess);

	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(TestBP);
	UEdGraphPin* AThen = FindPin(Graph, NodeA, TEXT("then"));
	UEdGraphPin* BExecute = FindPin(Graph, NodeB, TEXT("execute"));
	UEdGraphPin* CThen = FindPin(Graph, NodeC, TEXT("then"));
	if (!TestTrue(TEXT("fixture pins resolve"), AThen != nullptr && BExecute != nullptr && CThen != nullptr)
		|| !TestEqual(TEXT("B.execute has two links"), BExecute->LinkedTo.Num(), 2))
	{
		return false;
	}

	// The connect shape breaks exactly the named link.
	{
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.disconnect"), LinkParams(AssetPath, NodeA, NodeB));
		TestTrue(TEXT("link-shaped disconnect succeeds"), Result.bSuccess);
		if (TestTrue(TEXT("link-shaped disconnect returns a payload"), Result.Data.IsValid()))
		{
			TestTrue(TEXT("disconnected is true"), Result.Data->GetBoolField(TEXT("disconnected")));
			TestEqual(TEXT("one link broken"), static_cast<int32>(Result.Data->GetNumberField(TEXT("broken_connections"))), 1);
			TestEqual(TEXT("source echoed"), Result.Data->GetStringField(TEXT("source")), NodeA + TEXT(".then"));
			TestEqual(TEXT("target echoed"), Result.Data->GetStringField(TEXT("target")), NodeB + TEXT(".execute"));
		}
		TestFalse(TEXT("A.then is no longer linked to B.execute"), AThen->LinkedTo.Contains(BExecute));
		TestTrue(TEXT("C.then -> B.execute is kept"), CThen->LinkedTo.Contains(BExecute));
		TestEqual(TEXT("B.execute keeps one link"), BExecute->LinkedTo.Num(), 1);
	}

	// One editor undo step restores the link.
	const FString UndoTitle = GEditor != nullptr && GEditor->Trans != nullptr
		? GEditor->Trans->GetUndoContext(true).Title.ToString() : FString();
	if (TestEqual(TEXT("the disconnect is the latest undo step"), UndoTitle, FString(TEXT("Cortex:Disconnect Link"))))
	{
		GEditor->UndoTransaction();
		TestTrue(TEXT("undo restores A.then -> B.execute"), AThen->LinkedTo.Contains(BExecute));
		TestTrue(TEXT("undo keeps C.then -> B.execute"), CThen->LinkedTo.Contains(BExecute));
		GEditor->RedoTransaction();
		TestFalse(TEXT("redo breaks A.then -> B.execute again"), AThen->LinkedTo.Contains(BExecute));
	}

	// A link that does not exist is refused and changes nothing.
	{
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.disconnect"), LinkParams(AssetPath, NodeA, NodeB));
		TestFalse(TEXT("disconnecting an absent link fails"), Result.bSuccess);
		TestEqual(TEXT("absent link is INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("C.then -> B.execute survives the refusal"), CThen->LinkedTo.Contains(BExecute));
	}

	// Mixing the two shapes, or a partial link shape, is refused before any change.
	{
		TSharedPtr<FJsonObject> Mixed = LinkParams(AssetPath, NodeC, NodeB);
		Mixed->SetStringField(TEXT("node_id"), NodeC);
		Mixed->SetStringField(TEXT("pin_name"), TEXT("then"));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.disconnect"), Mixed);
		TestFalse(TEXT("mixed shapes fail"), Result.bSuccess);
		TestEqual(TEXT("mixed shapes are INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Partial = LinkParams(AssetPath, NodeC, NodeB);
		Partial->RemoveField(TEXT("target_pin"));
		const FCortexCommandResult PartialResult = Router.Execute(TEXT("graph.disconnect"), Partial);
		TestFalse(TEXT("partial link shape fails"), PartialResult.bSuccess);
		TestEqual(TEXT("partial link shape is INVALID_FIELD"), PartialResult.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("C.then -> B.execute survives refused shapes"), CThen->LinkedTo.Contains(BExecute));
	}

	// The pin shape still breaks every link on the pin.
	{
		TestTrue(TEXT("reconnect A -> B"), Router.Execute(TEXT("graph.connect"), LinkParams(AssetPath, NodeA, NodeB)).bSuccess);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeB);
		Params->SetStringField(TEXT("pin_name"), TEXT("execute"));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.disconnect"), Params);
		TestTrue(TEXT("pin-shaped disconnect succeeds"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			TestEqual(TEXT("both links broken"), static_cast<int32>(Result.Data->GetNumberField(TEXT("broken_connections"))), 2);
		}
		TestEqual(TEXT("B.execute has no links"), BExecute->LinkedTo.Num(), 0);
	}

	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
