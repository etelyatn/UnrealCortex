#include "Operations/CortexAnimRebindOps.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexGraphMemberReferences.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimBindingReflection.h"
#include "Operations/CortexAnimBindingReport.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace CortexAnimRebindPrivate
{
#if WITH_AUTOMATION_TESTS
	TFunction<bool(FCortexAnimRebindOps::EPhase, UAnimBlueprint*)>& FaultHook()
	{
		static TFunction<bool(FCortexAnimRebindOps::EPhase, UAnimBlueprint*)> Hook;
		return Hook;
	}
#endif

	bool Fault(const FCortexAnimRebindOps::EPhase Phase, UAnimBlueprint* AnimBlueprint)
	{
#if WITH_AUTOMATION_TESTS
		return FaultHook() && FaultHook()(Phase, AnimBlueprint);
#else
		return false;
#endif
	}

	const TCHAR* const SlotProperties[] = {
		TEXT("InitialUpdateFunction"),
		TEXT("BecomeRelevantFunction"),
		TEXT("UpdateFunction"),
	};

	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	TSharedPtr<FJsonObject> Details(const FString& Field, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("field"), Field);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		return Out;
	}

	bool ReadRebindBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const bool bDefault, bool& bOutValue, FCortexCommandResult& OutError)
	{
		bOutValue = bDefault;
		if (!Params.IsValid() || !Params->HasField(Field))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::Boolean>(Field))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be a boolean when provided"), Field),
				Details(Field, FString()));
			return false;
		}
		Params->TryGetBoolField(Field, bOutValue);
		return true;
	}

	bool SameReference(const FMemberReference& Left, const FMemberReference& Right)
	{
		return FMemberReference::StaticStruct()->CompareScriptStruct(&Left, &Right, PPF_None);
	}

	void CollectAnimNodes(const UBlueprint* Blueprint, TArray<UAnimGraphNode_Base*>& OutNodes)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
				{
					OutNodes.Add(AnimNode);
				}
			}
		}
	}

	// ── The exact occurrences ─────────────────────────────────────────────

	/** One binding-map entry whose path holds the function at the listed segments. */
	struct FBindingOccurrence
	{
		FName PropertyName = NAME_None;
		int32 ArrayIndex = INDEX_NONE;
		TArray<int32> Segments;
	};

	/** The occurrences of the source function on one anim node, split by ownership. */
	struct FNodePlan
	{
		UAnimGraphNode_Base* Node = nullptr;
		TArray<FBindingOccurrence> OwnedBindings;
		TArray<FBindingOccurrence> ForeignBindings;
		TArray<FName> OwnedSlots;
		TArray<FName> ForeignSlots;

		bool HasOwned() const { return OwnedBindings.Num() > 0 || OwnedSlots.Num() > 0; }
		bool HasAny() const { return HasOwned() || ForeignBindings.Num() > 0 || ForeignSlots.Num() > 0; }
	};

	/**
	 * Every anim node's occurrences of the function named From. An occurrence is owned when it resolves to the function this Blueprint
	 * declares (FromFunction): a binding segment by the engine's own rename predicate, a slot by resolving its member reference on the
	 * skeleton class. A same-named function elsewhere is foreign and never rewritten.
	 */
	void BuildPlan(const UAnimBlueprint* AnimBlueprint, const FName From, TArray<FNodePlan>& OutPlans, TArray<UAnimGraphNode_Base*>& OutUnsupportedNodes)
	{
		UClass* Skeleton = AnimBlueprint->SkeletonGeneratedClass;
		const UFunction* FromFunction = Skeleton != nullptr ? Skeleton->FindFunctionByName(From) : nullptr;

		TArray<UAnimGraphNode_Base*> Nodes;
		CollectAnimNodes(AnimBlueprint, Nodes);
		for (UAnimGraphNode_Base* Node : Nodes)
		{
			FNodePlan Plan;
			Plan.Node = Node;

			TArray<FCortexAnimBindingEntry> Entries;
			const bool bSupported = FCortexAnimBindingReflection::ReadNodeBindings(Node, Entries) == ECortexAnimBindingStorage::Supported;
			for (const FCortexAnimBindingEntry& Entry : Entries)
			{
				FBindingOccurrence Owned;
				FBindingOccurrence Foreign;
				FCortexAnimBindingReport::ResolveFunctionSegments(AnimBlueprint, Entry.Path, From, Owned.Segments, Foreign.Segments);
				Owned.PropertyName = Foreign.PropertyName = Entry.PropertyName;
				Owned.ArrayIndex = Foreign.ArrayIndex = Entry.ArrayIndex;
				if (Owned.Segments.Num() > 0)
				{
					Plan.OwnedBindings.Add(MoveTemp(Owned));
				}
				if (Foreign.Segments.Num() > 0)
				{
					Plan.ForeignBindings.Add(MoveTemp(Foreign));
				}
			}

			for (const TCHAR* SlotName : SlotProperties)
			{
				const FMemberReference* Slot = FCortexAnimBindingReflection::FindMutableSlot(Node, SlotName);
				if (Slot == nullptr || Slot->GetMemberName() != From)
				{
					continue;
				}
				const bool bOwned = FromFunction != nullptr && Slot->ResolveMember<UFunction>(Skeleton) == FromFunction;
				(bOwned ? Plan.OwnedSlots : Plan.ForeignSlots).Add(SlotName);
			}

			if (Plan.HasAny())
			{
				if (!bSupported)
				{
					OutUnsupportedNodes.Add(Node);
				}
				OutPlans.Add(MoveTemp(Plan));
			}
		}
	}

	// ── The journal ───────────────────────────────────────────────────────

	struct FNodeJournal
	{
		TWeakObjectPtr<UAnimGraphNode_Base> Node;
		bool bHasBindings = false;
		FCortexBindingMapSnapshot Bindings;
		TArray<FCortexAnimBindingEntry> EntriesBefore;
		TArray<TPair<FName, FMemberReference>> Slots;
	};

	struct FCallJournal
	{
		TWeakObjectPtr<UK2Node_CallFunction> Node;
		FMemberReference Reference;
	};

	struct FJournal
	{
		TArray<FNodeJournal> Nodes;
		TArray<FCallJournal> Calls;
	};

	/** Captures every anim node's whole binding map and function slots and every K2 call's function reference. */
	void CaptureJournal(const UBlueprint* AnimBlueprint, FJournal& OutJournal)
	{
		TArray<UAnimGraphNode_Base*> Nodes;
		CollectAnimNodes(AnimBlueprint, Nodes);
		for (UAnimGraphNode_Base* Node : Nodes)
		{
			FNodeJournal Journal;
			Journal.Node = Node;
			Journal.bHasBindings = FCortexAnimBindingReflection::CaptureBindingMap(Node, Journal.Bindings);
			FCortexAnimBindingReflection::ReadNodeBindings(Node, Journal.EntriesBefore);
			for (const TCHAR* SlotName : SlotProperties)
			{
				Journal.Slots.Add({ SlotName, *FCortexAnimBindingReflection::FindMutableSlot(Node, SlotName) });
			}
			OutJournal.Nodes.Add(MoveTemp(Journal));
		}

		TArray<UEdGraph*> Graphs;
		AnimBlueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
				{
					OutJournal.Calls.Add({ Call, Call->FunctionReference });
				}
			}
		}
	}

	/** Puts every captured value back; false (naming the steps) when any step could not be applied. */
	bool RestoreJournal(const FJournal& Journal, TArray<FString>& OutFailedSteps)
	{
		for (const FNodeJournal& Entry : Journal.Nodes)
		{
			UAnimGraphNode_Base* Node = Entry.Node.Get();
			if (Node == nullptr)
			{
				OutFailedSteps.Add(TEXT("an anim node no longer exists"));
				continue;
			}
			if (Entry.bHasBindings && !FCortexAnimBindingReflection::RestoreBindingMap(Node, Entry.Bindings))
			{
				OutFailedSteps.Add(FString::Printf(TEXT("binding map of %s"), *GuidText(Node->NodeGuid)));
			}
			for (const TPair<FName, FMemberReference>& Slot : Entry.Slots)
			{
				if (FMemberReference* Target = FCortexAnimBindingReflection::FindMutableSlot(Node, Slot.Key))
				{
					Node->Modify();
					*Target = Slot.Value;
				}
				else
				{
					OutFailedSteps.Add(FString::Printf(TEXT("slot %s of %s"), *Slot.Key.ToString(), *GuidText(Node->NodeGuid)));
				}
			}
		}
		for (const FCallJournal& Call : Journal.Calls)
		{
			if (UK2Node_CallFunction* Node = Call.Node.Get())
			{
				Node->FunctionReference = Call.Reference;
			}
			else
			{
				OutFailedSteps.Add(TEXT("a K2 call node no longer exists"));
			}
		}
		return OutFailedSteps.Num() == 0;
	}

	/** Compares the live state with the journal in every captured field; true when nothing differs. */
	bool JournalMatches(const FJournal& Journal, TArray<FString>& OutDifferences)
	{
		for (const FNodeJournal& Entry : Journal.Nodes)
		{
			const UAnimGraphNode_Base* Node = Entry.Node.Get();
			if (Node == nullptr)
			{
				OutDifferences.Add(TEXT("an anim node no longer exists"));
				continue;
			}
			if (Entry.bHasBindings && !FCortexAnimBindingReflection::BindingMapMatches(Node, Entry.Bindings))
			{
				OutDifferences.Add(FString::Printf(TEXT("binding map of %s differs"), *GuidText(Node->NodeGuid)));
			}
			for (const TPair<FName, FMemberReference>& Slot : Entry.Slots)
			{
				const FMemberReference* Current = FCortexAnimBindingReflection::FindMutableSlot(const_cast<UAnimGraphNode_Base*>(Node), Slot.Key);
				if (Current == nullptr || !SameReference(*Current, Slot.Value))
				{
					OutDifferences.Add(FString::Printf(TEXT("slot %s of %s differs"), *Slot.Key.ToString(), *GuidText(Node->NodeGuid)));
				}
			}
		}
		for (const FCallJournal& Call : Journal.Calls)
		{
			const UK2Node_CallFunction* Node = Call.Node.Get();
			if (Node == nullptr || !SameReference(Node->FunctionReference, Call.Reference))
			{
				OutDifferences.Add(Node != nullptr ? FString::Printf(TEXT("K2 call %s differs"), *GuidText(Node->NodeGuid)) : FString(TEXT("a K2 call node no longer exists")));
			}
		}
		return OutDifferences.Num() == 0;
	}

	// ── The applied-state readback ────────────────────────────────────────

	bool SameKey(const FCortexAnimBindingEntry& Left, const FCortexAnimBindingEntry& Right)
	{
		return Left.PropertyName == Right.PropertyName && Left.ArrayIndex == Right.ArrayIndex;
	}

	/**
	 * Checks the live state against the plan: owned binding segments now name To (their other fields unchanged), every other binding
	 * entry and slot is identical to the journal, owned slots resolve to the destination, K2 call references are unchanged, the engine's
	 * own ReferencesFunction agrees, and no owned occurrence of From remains.
	 */
	void VerifyApplied(const UAnimBlueprint* AnimBlueprint, const FJournal& Journal, const TArray<FNodePlan>& Plans, const FName From, const FName To,
		TArray<FString>& OutMismatches)
	{
		UClass* Skeleton = AnimBlueprint->SkeletonGeneratedClass;
		const UFunction* ToFunction = Skeleton != nullptr ? Skeleton->FindFunctionByName(To) : nullptr;
		if (ToFunction == nullptr)
		{
			OutMismatches.Add(FString::Printf(TEXT("'%s' no longer resolves on the skeleton class"), *To.ToString()));
			return;
		}

		for (const FNodeJournal& Entry : Journal.Nodes)
		{
			UAnimGraphNode_Base* Node = Entry.Node.Get();
			if (Node == nullptr)
			{
				OutMismatches.Add(TEXT("an anim node no longer exists"));
				continue;
			}
			const FNodePlan* Plan = Plans.FindByPredicate([Node](const FNodePlan& Candidate) { return Candidate.Node == Node; });

			if (Plan == nullptr || Plan->OwnedBindings.Num() == 0)
			{
				if (Entry.bHasBindings && !FCortexAnimBindingReflection::BindingMapMatches(Node, Entry.Bindings))
				{
					OutMismatches.Add(FString::Printf(TEXT("binding map of %s changed without being planned"), *GuidText(Node->NodeGuid)));
				}
			}
			else
			{
				TArray<FCortexAnimBindingEntry> After;
				FCortexAnimBindingReflection::ReadNodeBindings(Node, After);
				if (After.Num() != Entry.EntriesBefore.Num())
				{
					OutMismatches.Add(FString::Printf(TEXT("binding count of %s changed from %d to %d"), *GuidText(Node->NodeGuid), Entry.EntriesBefore.Num(), After.Num()));
				}
				for (const FCortexAnimBindingEntry& Before : Entry.EntriesBefore)
				{
					const FBindingOccurrence* Owned = Plan->OwnedBindings.FindByPredicate([&Before](const FBindingOccurrence& Candidate)
					{
						return Candidate.PropertyName == Before.PropertyName && Candidate.ArrayIndex == Before.ArrayIndex;
					});
					const FCortexAnimBindingEntry* Current = After.FindByPredicate([&Before](const FCortexAnimBindingEntry& Candidate) { return SameKey(Candidate, Before); });
					if (Current == nullptr)
					{
						OutMismatches.Add(FString::Printf(TEXT("binding %s of %s disappeared"), *Before.PropertyName.ToString(), *GuidText(Node->NodeGuid)));
						continue;
					}

					TArray<FString> ExpectedPath = Before.Path;
					if (Owned != nullptr)
					{
						for (const int32 Segment : Owned->Segments)
						{
							ExpectedPath[Segment] = To.ToString();
						}
					}
					const bool bSame = Current->Path == ExpectedPath && Current->bFunction == Before.bFunction && Current->bIsBound == Before.bIsBound
						&& Current->bIsPromotion == Before.bIsPromotion && Current->bOnlyUpdateWhenActive == Before.bOnlyUpdateWhenActive
						&& Current->PathText == (Owned != nullptr ? FCortexAnimBindingReport::MakePathText(AnimBlueprint, ExpectedPath).ToString() : Before.PathText);
					if (!bSame)
					{
						OutMismatches.Add(FString::Printf(TEXT("binding %s of %s is not the planned result"), *Before.PropertyName.ToString(), *GuidText(Node->NodeGuid)));
					}
				}
			}

			for (const TPair<FName, FMemberReference>& Slot : Entry.Slots)
			{
				const FMemberReference* Current = FCortexAnimBindingReflection::FindMutableSlot(Node, Slot.Key);
				const bool bOwned = Plan != nullptr && Plan->OwnedSlots.Contains(Slot.Key);
				if (Current == nullptr)
				{
					OutMismatches.Add(FString::Printf(TEXT("slot %s of %s is unreadable"), *Slot.Key.ToString(), *GuidText(Node->NodeGuid)));
				}
				else if (bOwned)
				{
					if (Current->GetMemberName() != To || Current->ResolveMember<UFunction>(Skeleton) != ToFunction)
					{
						OutMismatches.Add(FString::Printf(TEXT("slot %s of %s does not name the destination"), *Slot.Key.ToString(), *GuidText(Node->NodeGuid)));
					}
				}
				else if (!SameReference(*Current, Slot.Value))
				{
					OutMismatches.Add(FString::Printf(TEXT("slot %s of %s changed without being planned"), *Slot.Key.ToString(), *GuidText(Node->NodeGuid)));
				}
			}

			if (Plan != nullptr && Plan->HasOwned() && !Node->ReferencesFunction(To, nullptr))
			{
				OutMismatches.Add(FString::Printf(TEXT("the engine does not report %s as referencing the destination"), *GuidText(Node->NodeGuid)));
			}
		}

		for (const FCallJournal& Call : Journal.Calls)
		{
			const UK2Node_CallFunction* Node = Call.Node.Get();
			if (Node == nullptr || !SameReference(Node->FunctionReference, Call.Reference))
			{
				OutMismatches.Add(TEXT("a K2 call node changed"));
			}
		}

		TArray<FNodePlan> Remaining;
		TArray<UAnimGraphNode_Base*> Ignored;
		BuildPlan(AnimBlueprint, From, Remaining, Ignored);
		for (const FNodePlan& Left : Remaining)
		{
			if (Left.HasOwned())
			{
				OutMismatches.Add(FString::Printf(TEXT("%s still holds an owned occurrence of the source"), *GuidText(Left.Node->NodeGuid)));
			}
		}
	}

	// ── JSON ──────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> NodeLocator(const UAnimGraphNode_Base* Node)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_guid"), GuidText(Node->NodeGuid));
		Entry->SetStringField(TEXT("graph_guid"), Node->GetGraph() != nullptr ? GuidText(Node->GetGraph()->GraphGuid) : FString());
		Entry->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		return Entry;
	}

	TArray<TSharedPtr<FJsonValue>> OccurrencesToJson(const TArray<FNodePlan>& Plans, const bool bOwned, const TCHAR* Reason)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FNodePlan& Plan : Plans)
		{
			for (const FBindingOccurrence& Binding : bOwned ? Plan.OwnedBindings : Plan.ForeignBindings)
			{
				TSharedRef<FJsonObject> Entry = NodeLocator(Plan.Node);
				Entry->SetStringField(TEXT("kind"), TEXT("anim_node_binding"));
				Entry->SetStringField(TEXT("slot"), Binding.PropertyName.ToString());
				Entry->SetNumberField(TEXT("array_index"), Binding.ArrayIndex);
				TArray<TSharedPtr<FJsonValue>> Segments;
				for (const int32 Segment : Binding.Segments)
				{
					Segments.Add(MakeShared<FJsonValueNumber>(Segment));
				}
				Entry->SetArrayField(TEXT("segments"), Segments);
				if (!bOwned)
				{
					Entry->SetStringField(TEXT("reason"), Reason);
				}
				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}
			for (const FName& Slot : bOwned ? Plan.OwnedSlots : Plan.ForeignSlots)
			{
				TSharedRef<FJsonObject> Entry = NodeLocator(Plan.Node);
				Entry->SetStringField(TEXT("kind"), TEXT("anim_node_function"));
				Entry->SetStringField(TEXT("slot"), Slot.ToString());
				if (!bOwned)
				{
					Entry->SetStringField(TEXT("reason"), Reason);
				}
				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		return Values;
	}

	/** Everything the command leaves alone: the K2 references to the source, plus same-named functions of another owner. */
	TArray<TSharedPtr<FJsonValue>> UnmodifiedToJson(const UBlueprint* AnimBlueprint, const FName From, const TArray<FNodePlan>& Plans)
	{
		TArray<FCortexMemberReference> References;
		FCortexGraphMemberReferences::FindFunctionReferences(AnimBlueprint, From, References);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCortexMemberReference& Reference : References)
		{
			if (Reference.Kind != TEXT("anim_node_binding") && Reference.Kind != TEXT("anim_node_function"))
			{
				Values.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Reference)));
			}
		}
		Values.Append(OccurrencesToJson(Plans, /*bOwned=*/false, TEXT("different_owner")));
		return Values;
	}

	FString CompileStatus(const UAnimBlueprint* AnimBlueprint)
	{
		return AnimBlueprint->Status == BS_UpToDate ? TEXT("success") : AnimBlueprint->Status == BS_Error ? TEXT("failed") : TEXT("warning");
	}
}

FCortexCommandResult FCortexAnimRebindOps::RebindFunction(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexAnimRebindPrivate;

	FString AssetPath;
	FString From;
	FString To;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path (string)"), Details(TEXT("asset_path"), AssetPath));
	}
	if (!Params->TryGetStringField(TEXT("from"), From) || From.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: from (string)"), Details(TEXT("from"), AssetPath));
	}
	if (!Params->TryGetStringField(TEXT("to"), To) || To.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: to (string)"), Details(TEXT("to"), AssetPath));
	}

	FCortexCommandResult Error;
	bool bDryRun = true;
	bool bCompile = true;
	bool bSave = false;
	if (!ReadRebindBool(Params, TEXT("dry_run"), true, bDryRun, Error)
		|| !ReadRebindBool(Params, TEXT("compile"), true, bCompile, Error)
		|| !ReadRebindBool(Params, TEXT("save"), false, bSave, Error))
	{
		return Error;
	}

	// Mutation guard first: a blocked asset is refused before anything is read, planned or mutated.
	FString BlockedReason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, BlockedReason))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset path is blocked by a previous failed operation: %s (%s)"), *AssetPath, *BlockedReason),
			Details(TEXT("asset_path"), AssetPath));
	}

	FCortexAnimResolvedAsset Resolved;
	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimBlueprint>(Params, TEXT("AnimBlueprint"), Resolved, Error))
	{
		return Error;
	}
	UAnimBlueprint* AnimBlueprint = CastChecked<UAnimBlueprint>(Resolved.Asset);
	UPackage* Package = AnimBlueprint->GetPackage();

	if (!FCortexAnimAssetUtils::CheckExpectedFingerprint(AnimBlueprint, Params, Error))
	{
		return Error;
	}
	if (AnimBlueprint->SkeletonGeneratedClass == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("The Anim Blueprint has no skeleton class; compile it first"),
			Details(TEXT("asset_path"), AssetPath));
	}

	const FName FromName(*From);
	const FName ToName(*To);
	UFunction* FromFunction = AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(FromName);
	if (FromFunction == nullptr || UBlueprint::GetBlueprintFromClass(FromFunction->GetOuterUClass()) != AnimBlueprint)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is not a function declared by this Blueprint"), *From),
			Details(TEXT("from"), AssetPath));
	}
	UFunction* ToFunction = AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(ToName);
	if (ToFunction == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' does not resolve on the Blueprint or its parent classes"), *To),
			Details(TEXT("to"), AssetPath));
	}
	if (!FromFunction->IsSignatureCompatibleWith(ToFunction))
	{
		TSharedPtr<FJsonObject> Mismatch = Details(TEXT("to"), AssetPath);
		Mismatch->SetStringField(TEXT("from"), From);
		Mismatch->SetStringField(TEXT("to"), To);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::TypeMismatch,
			FString::Printf(TEXT("'%s' and '%s' do not have compatible signatures"), *From, *To),
			Mismatch);
	}

	FString MissingField;
	if (!FCortexAnimBindingReflection::CheckBindingStorage(MissingField))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("This engine build does not expose the reflected field '%s' needed to journal anim-node bindings"), *MissingField),
			Details(TEXT("to"), AssetPath));
	}

	// The exact occurrences: owned ones are the plan, foreign ones are reported and preserved.
	TArray<FNodePlan> Plans;
	TArray<UAnimGraphNode_Base*> UnsupportedNodes;
	BuildPlan(AnimBlueprint, FromName, Plans, UnsupportedNodes);
	int32 OwnedCount = 0;
	for (const FNodePlan& Plan : Plans)
	{
		OwnedCount += Plan.OwnedBindings.Num() + Plan.OwnedSlots.Num();
	}
	for (const UAnimGraphNode_Base* Unsupported : UnsupportedNodes)
	{
		const FNodePlan* Plan = Plans.FindByPredicate([Unsupported](const FNodePlan& Candidate) { return Candidate.Node == Unsupported; });
		if (Plan != nullptr && Plan->HasOwned())
		{
			TSharedPtr<FJsonObject> Unsupp = Details(TEXT("asset_path"), AssetPath);
			Unsupp->SetStringField(TEXT("node_guid"), GuidText(Unsupported->NodeGuid));
			Unsupp->SetStringField(TEXT("binding_storage"), TEXT("unsupported"));
			return FCortexCommandRouter::Error(
				CortexErrorCodes::UnsupportedOperation,
				TEXT("A node to retarget keeps its bindings in storage this build cannot journal; nothing was changed"),
				Unsupp);
		}
	}

	const bool bWasDirty = Package != nullptr && Package->IsDirty();
	if (bDryRun || OwnedCount == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
		Data->SetStringField(TEXT("from"), From);
		Data->SetStringField(TEXT("to"), To);
		Data->SetBoolField(TEXT("dry_run"), bDryRun);
		Data->SetArrayField(TEXT("would_retarget"), OccurrencesToJson(Plans, /*bOwned=*/true, TEXT("")));
		Data->SetArrayField(TEXT("unmodified_references"), UnmodifiedToJson(AnimBlueprint, FromName, Plans));
		Data->SetStringField(TEXT("compile_status"), TEXT("not_run"));
		Data->SetStringField(TEXT("readback_status"), bDryRun ? TEXT("dry_run") : TEXT("nothing_to_retarget"));
		Data->SetBoolField(TEXT("dirty"), bWasDirty);
		Data->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(AnimBlueprint));
		return FCortexCommandRouter::Success(Data);
	}

	// ── Apply: one transaction from the first edit to the verdict, so a failure can cancel its undo record ──
	FJournal Journal;
	CaptureJournal(AnimBlueprint, Journal);
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(FText::FromString(TEXT("Cortex: rebind animation function")));
	AnimBlueprint->Modify();

	for (const FNodePlan& Plan : Plans)
	{
		if (!Plan.HasOwned())
		{
			continue;
		}
		Plan.Node->Modify();
		if (Plan.OwnedBindings.Num() > 0)
		{
			FCortexAnimBindingReflection::MarkBindingModified(Plan.Node);
			Plan.Node->HandleFunctionRenamed(AnimBlueprint, AnimBlueprint->SkeletonGeneratedClass, Plan.Node->GetGraph(), FromName, ToName);
		}
		// The engine hook skips the three public function slots (AnimGraphNode_Base.cpp:1097-1103).
		for (const FName& SlotName : Plan.OwnedSlots)
		{
			if (FMemberReference* Slot = FCortexAnimBindingReflection::FindMutableSlot(Plan.Node, SlotName))
			{
				Slot->SetFromField<UFunction>(ToFunction, /*bIsConsideredSelfContext=*/true);
			}
		}
	}

	// The edits must equal the plan exactly: every planned node's binding map is compared, entry by entry and field by field (paths,
	// display text, flags, pin types, contexts), with a copy of the journal in which only the planned rewrites were applied.
	auto VerifyEdits = [&](TArray<FString>& OutMismatches)
	{
		for (const FNodePlan& Plan : Plans)
		{
			if (Plan.OwnedBindings.Num() == 0)
			{
				continue;
			}
			const FNodeJournal* Entry = Journal.Nodes.FindByPredicate([&Plan](const FNodeJournal& Candidate) { return Candidate.Node.Get() == Plan.Node; });
			FCortexBindingMapSnapshot Expected;
			if (Entry == nullptr || !FCortexAnimBindingReflection::CloneBindingMap(Entry->Bindings, Expected))
			{
				OutMismatches.Add(FString::Printf(TEXT("no journal copy of %s"), *GuidText(Plan.Node->NodeGuid)));
				continue;
			}
			for (const FBindingOccurrence& Owned : Plan.OwnedBindings)
			{
				const FCortexAnimBindingEntry* Before = Entry->EntriesBefore.FindByPredicate([&Owned](const FCortexAnimBindingEntry& Candidate) { return SameKey(Candidate, FCortexAnimBindingEntry{ Owned.PropertyName, Owned.ArrayIndex }); });
				if (Before == nullptr)
				{
					OutMismatches.Add(FString::Printf(TEXT("planned binding %s of %s is not in the journal"), *Owned.PropertyName.ToString(), *GuidText(Plan.Node->NodeGuid)));
					continue;
				}
				TArray<FString> NewPath = Before->Path;
				for (const int32 Segment : Owned.Segments)
				{
					NewPath[Segment] = ToName.ToString();
				}
				FCortexAnimBindingReflection::RewriteSnapshotEntry(Expected, Owned.PropertyName, Owned.ArrayIndex, NewPath, FCortexAnimBindingReport::MakePathText(AnimBlueprint, NewPath));
			}
			if (!FCortexAnimBindingReflection::BindingMapMatchesContent(Plan.Node, Expected))
			{
				OutMismatches.Add(FString::Printf(TEXT("the binding map of %s is not exactly the planned result"), *GuidText(Plan.Node->NodeGuid)));
			}
		}
	};

	// The single failure route.
	auto Recover = [&](const TCHAR* Phase, const FString& Reason, const FString& ErrorCode) -> FCortexCommandResult
	{
		TArray<FString> RestoreFailures;
		bool bRestoreOk = RestoreJournal(Journal, RestoreFailures);
		if (Fault(EPhase::AfterRestore, AnimBlueprint))
		{
			bRestoreOk = false;
			RestoreFailures.Add(TEXT("injected restore failure"));
		}
		Transaction->Cancel();
		Transaction.Reset();
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

		TArray<FString> Differences;
		const bool bVerified = JournalMatches(Journal, Differences);
		const bool bRestored = bRestoreOk && bVerified;
		if (bRestored)
		{
			if (Package != nullptr)
			{
				Package->SetDirtyFlag(bWasDirty);
			}
		}
		else
		{
			FCortexAssetMutationGuard::Block(AnimBlueprint, FString::Printf(TEXT("anim.rebind_function: the pre-call state could not be restored and verified after a %s failure"), Phase));
		}

		TSharedPtr<FJsonObject> Failure = Details(TEXT("to"), AssetPath);
		Failure->SetStringField(TEXT("failed_phase"), Phase);
		Failure->SetStringField(TEXT("failure"), Reason);
		Failure->SetStringField(TEXT("rollback_status"), bRestored ? TEXT("restored") : TEXT("verification_failed"));
		Failure->SetStringField(TEXT("recovery_compile"), TEXT("run"));
		Failure->SetStringField(TEXT("compile_status"), CompileStatus(AnimBlueprint));
		Failure->SetBoolField(TEXT("content_restored"), bRestored);
		TArray<TSharedPtr<FJsonValue>> Problems;
		for (const FString& Problem : RestoreFailures)
		{
			Problems.Add(MakeShared<FJsonValueString>(Problem));
		}
		for (const FString& Problem : Differences)
		{
			Problems.Add(MakeShared<FJsonValueString>(Problem));
		}
		Failure->SetArrayField(TEXT("restore_problems"), Problems);
		Failure->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(AnimBlueprint));
		return FCortexCommandRouter::Error(
			bRestored ? ErrorCode : CortexErrorCodes::VerificationFailed,
			FString::Printf(TEXT("Retargeting '%s' to '%s' failed in the %s phase (%s); %s"), *From, *To, Phase, *Reason,
				bRestored ? TEXT("the pre-call state was restored and verified") : TEXT("the pre-call state could not be verified, the asset path is blocked")),
			Failure);
	};

	if (Fault(EPhase::AfterMutation, AnimBlueprint))
	{
		return Recover(TEXT("mutation"), TEXT("injected failure after the edits"), CortexErrorCodes::VerificationFailed);
	}

	{
		TArray<FString> EditMismatches;
		VerifyEdits(EditMismatches);
		if (EditMismatches.Num() > 0)
		{
			return Recover(TEXT("readback"), FString::Join(EditMismatches, TEXT("; ")), CortexErrorCodes::VerificationFailed);
		}
	}

	if (bCompile)
	{
		if (Fault(EPhase::Compile, AnimBlueprint))
		{
			return Recover(TEXT("compile"), TEXT("injected compile failure"), CortexErrorCodes::CompileFailed);
		}
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
		if (AnimBlueprint->Status == BS_Error)
		{
			return Recover(TEXT("compile"), TEXT("compile error after the retarget"), CortexErrorCodes::CompileFailed);
		}
	}

	// ── Readback against the exact plan ──
	TArray<FString> Mismatches;
	VerifyApplied(AnimBlueprint, Journal, Plans, FromName, ToName, Mismatches);
	if (Fault(EPhase::Readback, AnimBlueprint))
	{
		Mismatches.Add(TEXT("injected readback mismatch"));
	}
	if (Mismatches.Num() > 0)
	{
		return Recover(TEXT("readback"), FString::Join(Mismatches, TEXT("; ")), CortexErrorCodes::VerificationFailed);
	}

	TArray<FString> SavedPackages;
	if (bSave)
	{
		FString Saved;
		FCortexCommandResult SaveError;
		const bool bSaved = !Fault(EPhase::Save, AnimBlueprint) && FCortexAnimAssetUtils::SaveAsset(AnimBlueprint, Saved, SaveError);
		if (!bSaved)
		{
			return Recover(TEXT("save"), SaveError.ErrorMessage.IsEmpty() ? FString(TEXT("injected save failure")) : SaveError.ErrorMessage, CortexErrorCodes::SaveFailed);
		}
		SavedPackages.Add(Saved);
	}
	Transaction.Reset();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetStringField(TEXT("from"), From);
	Data->SetStringField(TEXT("to"), To);
	Data->SetBoolField(TEXT("dry_run"), false);
	Data->SetArrayField(TEXT("retargeted"), OccurrencesToJson(Plans, /*bOwned=*/true, TEXT("")));
	Data->SetArrayField(TEXT("unmodified_references"), UnmodifiedToJson(AnimBlueprint, FromName, Plans));
	Data->SetStringField(TEXT("compile_status"), bCompile ? CompileStatus(AnimBlueprint) : FString(TEXT("not_run")));
	Data->SetStringField(TEXT("readback_status"), TEXT("matched"));
	const UClass* ToOwner = AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(ToName) != nullptr
		? AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(ToName)->GetOuterUClass() : nullptr;
	Data->SetBoolField(TEXT("declared_in_native"), ToOwner != nullptr && !ToOwner->HasAnyClassFlags(CLASS_CompiledFromBlueprint));
	Data->SetBoolField(TEXT("saved"), bSave && SavedPackages.Num() > 0);
	if (SavedPackages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SavedValues;
		for (const FString& SavedPackage : SavedPackages)
		{
			SavedValues.Add(MakeShared<FJsonValueString>(SavedPackage));
		}
		Data->SetArrayField(TEXT("saved_packages"), SavedValues);
	}
	Data->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(AnimBlueprint));
	return FCortexCommandRouter::Success(Data);
}

#if WITH_AUTOMATION_TESTS
void FCortexAnimRebindOps::SetFaultHookForTest(TFunction<bool(EPhase, UAnimBlueprint*)> Hook)
{
	CortexAnimRebindPrivate::FaultHook() = MoveTemp(Hook);
}

void FCortexAnimRebindOps::ClearFaultHookForTest()
{
	CortexAnimRebindPrivate::FaultHook() = nullptr;
}
#endif
