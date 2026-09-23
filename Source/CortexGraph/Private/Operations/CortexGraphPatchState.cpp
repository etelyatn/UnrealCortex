#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "CortexAssetFingerprint.h"
#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "IO/IoHash.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Composite.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"

namespace
{
	struct FGraphInfo
	{
		UEdGraph* Graph = nullptr;
		ECortexGraphKind Kind = ECortexGraphKind::Function;
		FName OwningInterface = NAME_None;
		FString SubgraphPath;
		FString SortKey;
	};

	void CollectSubgraphsRecursive(
		UEdGraph* Graph,
		ECortexGraphKind Kind,
		FName OwningInterface,
		const FString& CurrentPath,
		TArray<FGraphInfo>& OutGraphs,
		int32 Depth)
	{
		if (!Graph || Depth >= 4)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node)) continue;
			UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
			if (!Composite || !Composite->BoundGraph) continue;

			UEdGraph* Sub = Composite->BoundGraph;
			if (!IsValid(Sub)) continue;

			FString SubPath = CurrentPath.IsEmpty()
				? Sub->GetName()
				: FString::Printf(TEXT("%s.%s"), *CurrentPath, *Sub->GetName());

			FGraphInfo Info;
			Info.Graph = Sub;
			Info.Kind = Kind;
			Info.OwningInterface = OwningInterface;
			Info.SubgraphPath = SubPath;
			Info.SortKey = FString::Printf(TEXT("%s_%s_%s"),
				Sub->GraphGuid.IsValid() ? *Sub->GraphGuid.ToString() : *Sub->GetName(),
				*FCortexGraphNodeOps::GraphKindToString(Kind),
				*SubPath);
			OutGraphs.Add(Info);

			CollectSubgraphsRecursive(Sub, Kind, OwningInterface, SubPath, OutGraphs, Depth + 1);
		}
	}
}

TSharedPtr<FJsonObject> FCortexGraphPatchState::ComputeFingerprint(UBlueprint* Blueprint)
{
	FCortexAssetFingerprint BaseFingerprint = MakeObjectAssetFingerprint(Blueprint);
	TSharedPtr<FJsonObject> Json = BaseFingerprint.ToJson();
	Json->SetNumberField(TEXT("graph_authoring_version"), 1);

	if (Blueprint == nullptr)
	{
		Json->SetStringField(TEXT("graph_authoring_hash"), TEXT(""));
		return Json;
	}

	FString Buffer;
	Buffer.Reserve(16384);

	// 1. Asset Identity
	Buffer += FString::Printf(TEXT("AssetPath: %s\n"), *Blueprint->GetPathName());
	Buffer += FString::Printf(TEXT("AssetClass: %s\n"), *Blueprint->GetClass()->GetName());
	Buffer += FString::Printf(TEXT("ParentClass: %s\n"),
		Blueprint->ParentClass ? *Blueprint->ParentClass->GetPathName() : TEXT("None"));
	Buffer += FString::Printf(TEXT("GeneratedClass: %s\n"),
		Blueprint->GeneratedClass ? *Blueprint->GeneratedClass->GetPathName() : TEXT("None"));

	// 2. Variables (sorted by VarName)
	TArray<FBPVariableDescription> SortedVars = Blueprint->NewVariables;
	SortedVars.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B) {
		return A.VarName.LexicalLess(B.VarName);
	});

	Buffer += FString::Printf(TEXT("Variables: %d\n"), SortedVars.Num());
	for (const FBPVariableDescription& Var : SortedVars)
	{
		Buffer += FString::Printf(TEXT("  Var: %s Guid: %s Cat: %s SubCat: %s SubObj: %s Container: %d Flags: %llu Def: %s\n"),
			*Var.VarName.ToString(),
			*Var.VarGuid.ToString(),
			*Var.VarType.PinCategory.ToString(),
			*Var.VarType.PinSubCategory.ToString(),
			Var.VarType.PinSubCategoryObject.IsValid() ? *Var.VarType.PinSubCategoryObject->GetPathName() : TEXT("None"),
			static_cast<int32>(Var.VarType.ContainerType),
			static_cast<uint64>(Var.PropertyFlags),
			*Var.DefaultValue);
	}

	// 3. Implemented Interfaces (sorted by interface class path)
	TArray<FBPInterfaceDescription> SortedInterfaces = Blueprint->ImplementedInterfaces;
	SortedInterfaces.Sort([](const FBPInterfaceDescription& A, const FBPInterfaceDescription& B) {
		FString NameA = A.Interface ? A.Interface->GetPathName() : TEXT("");
		FString NameB = B.Interface ? B.Interface->GetPathName() : TEXT("");
		return NameA < NameB;
	});

	Buffer += FString::Printf(TEXT("Interfaces: %d\n"), SortedInterfaces.Num());
	for (const FBPInterfaceDescription& Iface : SortedInterfaces)
	{
		Buffer += FString::Printf(TEXT("  Interface: %s Graphs: %d\n"),
			Iface.Interface ? *Iface.Interface->GetPathName() : TEXT("None"),
			Iface.Graphs.Num());
	}

	// 4. Graphs and their Nodes/Pins/Layout
	TArray<FCortexGraphEntry> Entries;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);

	TArray<FGraphInfo> AllGraphs;
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (!Entry.Graph) continue;
		FGraphInfo Info;
		Info.Graph = Entry.Graph;
		Info.Kind = Entry.Kind;
		Info.OwningInterface = Entry.OwningInterface;
		Info.SubgraphPath = TEXT("");
		Info.SortKey = FString::Printf(TEXT("%s_%s_"),
			Entry.Graph->GraphGuid.IsValid() ? *Entry.Graph->GraphGuid.ToString() : *Entry.Graph->GetName(),
			*FCortexGraphNodeOps::GraphKindToString(Entry.Kind));
		AllGraphs.Add(Info);

		CollectSubgraphsRecursive(Entry.Graph, Entry.Kind, Entry.OwningInterface, TEXT(""), AllGraphs, 0);
	}

	AllGraphs.Sort([](const FGraphInfo& A, const FGraphInfo& B) {
		return A.SortKey < B.SortKey;
	});

	Buffer += FString::Printf(TEXT("Graphs: %d\n"), AllGraphs.Num());
	for (const FGraphInfo& GInfo : AllGraphs)
	{
		UEdGraph* Graph = GInfo.Graph;
		Buffer += FString::Printf(TEXT("Graph: Guid=%s Name=%s Kind=%s Iface=%s SubPath=%s\n"),
			*Graph->GraphGuid.ToString(),
			*Graph->GetName(),
			*FCortexGraphNodeOps::GraphKindToString(GInfo.Kind),
			*GInfo.OwningInterface.ToString(),
			*GInfo.SubgraphPath);

		// Sort nodes deterministically
		TArray<UEdGraphNode*> SortedNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node))
			{
				SortedNodes.Add(Node);
			}
		}

		SortedNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) {
			FString KeyA = A.NodeGuid.IsValid() ? A.NodeGuid.ToString() : A.GetName();
			FString KeyB = B.NodeGuid.IsValid() ? B.NodeGuid.ToString() : B.GetName();
			return KeyA < KeyB;
		});

		Buffer += FString::Printf(TEXT(" Nodes: %d\n"), SortedNodes.Num());
		for (UEdGraphNode* Node : SortedNodes)
		{
			Buffer += FString::Printf(TEXT("  Node: Guid=%s Name=%s Class=%s Pos=(%d,%d) Size=(%d,%d) Comment=\"%s\" PinBubble=%d VisBubble=%d\n"),
				*Node->NodeGuid.ToString(),
				*Node->GetName(),
				*Node->GetClass()->GetPathName(),
				Node->NodePosX,
				Node->NodePosY,
				Node->NodeWidth,
				Node->NodeHeight,
				*Node->NodeComment,
				Node->bCommentBubblePinned ? 1 : 0,
				Node->bCommentBubbleVisible ? 1 : 0);

			// Authored node settings
			if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				const FString MemberName = CallNode->FunctionReference.GetMemberName().ToString();
				const FString MemberParent = CallNode->FunctionReference.GetMemberParentClass() ? CallNode->FunctionReference.GetMemberParentClass()->GetPathName() : TEXT("None");
				Buffer += FString::Printf(TEXT("   CallFunc: Member=%s Parent=%s Pure=%d\n"),
					*MemberName,
					*MemberParent,
					CallNode->IsNodePure() ? 1 : 0);
			}
			else if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				const FString CustomName = CustomEvent->CustomFunctionName.ToString();
				Buffer += FString::Printf(TEXT("   CustomEvent: Name=%s\n"), *CustomName);
			}
			else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				const FString EventName = EventNode->EventReference.GetMemberName().ToString();
				const FString EventParent = EventNode->EventReference.GetMemberParentClass() ? EventNode->EventReference.GetMemberParentClass()->GetPathName() : TEXT("None");
				Buffer += FString::Printf(TEXT("   Event: Name=%s Parent=%s Override=%d\n"),
					*EventName,
					*EventParent,
					EventNode->bOverrideFunction ? 1 : 0);
			}
			else if (const UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
			{
				const FString VarName = VarGet->VariableReference.GetMemberName().ToString();
				const FString VarParent = VarGet->VariableReference.GetMemberParentClass() ? VarGet->VariableReference.GetMemberParentClass()->GetPathName() : TEXT("None");
				Buffer += FString::Printf(TEXT("   VarGet: Member=%s Parent=%s Self=%d\n"),
					*VarName,
					*VarParent,
					VarGet->VariableReference.IsSelfContext() ? 1 : 0);
			}
			else if (const UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
			{
				const FString VarName = VarSet->VariableReference.GetMemberName().ToString();
				const FString VarParent = VarSet->VariableReference.GetMemberParentClass() ? VarSet->VariableReference.GetMemberParentClass()->GetPathName() : TEXT("None");
				Buffer += FString::Printf(TEXT("   VarSet: Member=%s Parent=%s Self=%d\n"),
					*VarName,
					*VarParent,
					VarSet->VariableReference.IsSelfContext() ? 1 : 0);
			}
			else if (const UK2Node_DynamicCast* DynCast = Cast<UK2Node_DynamicCast>(Node))
			{
				const FString TargetType = DynCast->TargetType ? DynCast->TargetType->GetPathName() : TEXT("None");
				Buffer += FString::Printf(TEXT("   DynCast: Target=%s Pure=%d\n"),
					*TargetType,
					DynCast->IsNodePure() ? 1 : 0);
			}
			else if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				Buffer += FString::Printf(TEXT("   FuncEntry: CustomName=%s\n"),
					*EntryNode->CustomGeneratedFunctionName.ToString());
			}

			// Sort pins by Direction then PinName
			TArray<UEdGraphPin*> SortedPins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					SortedPins.Add(Pin);
				}
			}
			SortedPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) {
				if (A.Direction != B.Direction)
				{
					return static_cast<int32>(A.Direction) < static_cast<int32>(B.Direction);
				}
				return A.PinName.LexicalLess(B.PinName);
			});

			Buffer += FString::Printf(TEXT("   Pins: %d\n"), SortedPins.Num());
			for (UEdGraphPin* Pin : SortedPins)
			{
				Buffer += FString::Printf(TEXT("    Pin: Name=%s Dir=%d Cat=%s SubCat=%s SubObj=%s Container=%d Ref=%d Const=%d Def=\"%s\" DefTextId=\"%s\" DefObj=%s Links=%d\n"),
					*Pin->PinName.ToString(),
					static_cast<int32>(Pin->Direction),
					*Pin->PinType.PinCategory.ToString(),
					*Pin->PinType.PinSubCategory.ToString(),
					Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("None"),
					static_cast<int32>(Pin->PinType.ContainerType),
					Pin->PinType.bIsReference ? 1 : 0,
					Pin->PinType.bIsConst ? 1 : 0,
					*Pin->DefaultValue,
					// Canonical FText identity, shared with the pin-default comparison: two defaults
					// that display identically (a table entry and a literal, or two tables) are
					// different intents and must move the hash.
					*FCortexGraphPinDefaults::CanonicalTextIdentity(Pin->DefaultTextValue),
					Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT("None"),
					Pin->LinkedTo.Num());

				// Sort links deterministically
				TArray<UEdGraphPin*> SortedLinks;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin)
					{
						SortedLinks.Add(LinkedPin);
					}
				}
				SortedLinks.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) {
					FString NodeA = A.GetOwningNode() ? (A.GetOwningNode()->NodeGuid.IsValid() ? A.GetOwningNode()->NodeGuid.ToString() : A.GetOwningNode()->GetName()) : TEXT("");
					FString NodeB = B.GetOwningNode() ? (B.GetOwningNode()->NodeGuid.IsValid() ? B.GetOwningNode()->NodeGuid.ToString() : B.GetOwningNode()->GetName()) : TEXT("");
					if (NodeA != NodeB)
					{
						return NodeA < NodeB;
					}
					return A.PinName.LexicalLess(B.PinName);
				});

				for (UEdGraphPin* Linked : SortedLinks)
				{
					UEdGraphNode* LinkedNode = Linked->GetOwningNode();
					FString LinkedNodeId = LinkedNode ? (LinkedNode->NodeGuid.IsValid() ? LinkedNode->NodeGuid.ToString() : LinkedNode->GetName()) : TEXT("None");
					Buffer += FString::Printf(TEXT("     Link -> Node=%s Pin=%s Dir=%d\n"),
						*LinkedNodeId,
						*Linked->PinName.ToString(),
						static_cast<int32>(Linked->Direction));
				}
			}
		}
	}

	// 5. Designer variable state (Widget Blueprint)
	if (const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
	{
		if (WidgetBP->WidgetTree)
		{
			TArray<UWidget*> AllWidgets;
			WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);
			AllWidgets.Sort([](const UWidget& A, const UWidget& B) {
				return A.GetFName().LexicalLess(B.GetFName());
			});

			Buffer += FString::Printf(TEXT("DesignerWidgets: %d\n"), AllWidgets.Num());
			for (const UWidget* Widget : AllWidgets)
			{
				if (!Widget) continue;
				Buffer += FString::Printf(TEXT("  Widget: Name=%s Class=%s IsVar=%d\n"),
					*Widget->GetFName().ToString(),
					*Widget->GetClass()->GetPathName(),
					Widget->bIsVariable ? 1 : 0);
			}
		}
	}

	// Compute IoHash of Buffer
	FTCHARToUTF8 Utf8Buffer(*Buffer);
	FIoHash IoHash = FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8Buffer.Get()), Utf8Buffer.Length());
	FString HashHex = LexToString(IoHash);

	Json->SetStringField(TEXT("graph_authoring_hash"), HashHex);
	return Json;
}

FString FCortexGraphPatchState::ComputeGeneratedStateDigest(UBlueprint* Blueprint)
{
	if (Blueprint == nullptr)
	{
		return FString();
	}

	UClass* GeneratedClass = Blueprint->GeneratedClass;
	FString Buffer;
	Buffer.Reserve(2048);
	Buffer += FString::Printf(TEXT("GeneratedClass: %s\n"),
		GeneratedClass ? *GeneratedClass->GetPathName() : TEXT("None"));
	Buffer += FString::Printf(TEXT("SuperClass: %s\n"),
		GeneratedClass && GeneratedClass->GetSuperClass() ? *GeneratedClass->GetSuperClass()->GetPathName() : TEXT("None"));
	Buffer += FString::Printf(TEXT("ParentClass: %s\n"),
		Blueprint->ParentClass ? *Blueprint->ParentClass->GetPathName() : TEXT("None"));

	TArray<UFunction*> Functions;
	if (GeneratedClass)
	{
		for (TFieldIterator<UFunction> It(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (UFunction* Function = *It)
			{
				Functions.Add(Function);
			}
		}
	}
	Functions.Sort([](const UFunction& A, const UFunction& B) { return A.GetName() < B.GetName(); });

	Buffer += FString::Printf(TEXT("Functions: %d\n"), Functions.Num());
	for (const UFunction* Function : Functions)
	{
		Buffer += FString::Printf(TEXT("  Fn: %s Flags=%llu\n"),
			*Function->GetName(),
			static_cast<uint64>(Function->FunctionFlags));

		TArray<const FProperty*> Parameters;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property && Property->HasAnyPropertyFlags(CPF_Parm))
			{
				Parameters.Add(Property);
			}
		}
		Parameters.Sort([](const FProperty& A, const FProperty& B) { return A.GetName() < B.GetName(); });
		for (const FProperty* Parameter : Parameters)
		{
			Buffer += FString::Printf(TEXT("    Param: %s Type=%s Flags=%llu\n"),
				*Parameter->GetName(),
				*Parameter->GetCPPType(),
				static_cast<uint64>(Parameter->PropertyFlags));
		}
	}

	return Buffer;
}

bool FCortexGraphPatchState::ValidatePrecondition(
	const TSharedPtr<FJsonObject>& Expected,
	const TSharedPtr<FJsonObject>& Current,
	FCortexCommandResult& OutError)
{
	if (!Expected.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Expected fingerprint is missing or null")
		);
		return false;
	}

	if (!Current.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Current fingerprint is missing or null")
		);
		return false;
	}

	// Check for unknown fields in Expected
	for (const auto& Pair : Expected->Values)
	{
		const FString FieldName = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (FieldName != TEXT("package_saved_hash") &&
			FieldName != TEXT("is_dirty") &&
			FieldName != TEXT("dirty_epoch") &&
			FieldName != TEXT("not_ready") &&
			FieldName != TEXT("compiled_signature_crc") &&
			FieldName != TEXT("graph_authoring_version") &&
			FieldName != TEXT("graph_authoring_hash"))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown field '%s' in expected_fingerprint"), *FieldName)
			);
			return false;
		}
	}

	// Validate exact version
	int32 ExpectedVersion = 0;
	if (!Expected->TryGetNumberField(TEXT("graph_authoring_version"), ExpectedVersion) || ExpectedVersion != 1)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("graph_authoring_version is missing or invalid (expected 1)")
		);
		return false;
	}

	// Validate required authoring hash
	FString ExpectedHash;
	FString CurrentHash;
	const bool bHasHash = Expected->TryGetStringField(TEXT("graph_authoring_hash"), ExpectedHash)
		&& Current->TryGetStringField(TEXT("graph_authoring_hash"), CurrentHash);
	if (!bHasHash || ExpectedHash.IsEmpty() || ExpectedHash != CurrentHash)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Graph authoring precondition is missing or stale")
		);
		return false;
	}

	// Validate required Core fields
	FString ExpectedPkgHash, CurrentPkgHash;
	if (!Expected->TryGetStringField(TEXT("package_saved_hash"), ExpectedPkgHash) ||
		!Current->TryGetStringField(TEXT("package_saved_hash"), CurrentPkgHash) ||
		ExpectedPkgHash != CurrentPkgHash)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("package_saved_hash is missing or does not match current state")
		);
		return false;
	}

	bool bExpectedDirty = false, bCurrentDirty = false;
	if (!Expected->TryGetBoolField(TEXT("is_dirty"), bExpectedDirty) ||
		!Current->TryGetBoolField(TEXT("is_dirty"), bCurrentDirty) ||
		bExpectedDirty != bCurrentDirty)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("is_dirty is missing or does not match current state")
		);
		return false;
	}

	FString ExpectedEpoch, CurrentEpoch;
	if (!Expected->TryGetStringField(TEXT("dirty_epoch"), ExpectedEpoch) ||
		!Current->TryGetStringField(TEXT("dirty_epoch"), CurrentEpoch) ||
		ExpectedEpoch != CurrentEpoch)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("dirty_epoch is missing or does not match current state")
		);
		return false;
	}

	bool bExpectedNotReady = false, bCurrentNotReady = false;
	if (!Expected->TryGetBoolField(TEXT("not_ready"), bExpectedNotReady) ||
		!Current->TryGetBoolField(TEXT("not_ready"), bCurrentNotReady) ||
		bExpectedNotReady != bCurrentNotReady)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("not_ready is missing or does not match current state")
		);
		return false;
	}

	return true;
}
