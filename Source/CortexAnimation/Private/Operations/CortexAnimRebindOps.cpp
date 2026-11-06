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
#include "HAL/Platform.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimBindingReflection.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace CortexAnimRebindPrivate
{
#if WITH_AUTOMATION_TESTS
	TFunction<bool(UAnimBlueprint*)>& CompileFailureHook()
	{
		static TFunction<bool(UAnimBlueprint*)> Hook;
		return Hook;
	}
#endif

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

	/** A retargetable reference: an anim node whose function binding or function slot names the retargeted function. */
	struct FRetarget
	{
		UAnimGraphNode_Base* Node = nullptr;
		FString Kind;
		FString Slot;
		FGuid GraphGuid;
	};

	bool IsRetargetable(const FCortexMemberReference& Reference)
	{
		return Reference.Kind == TEXT("anim_node_binding") || Reference.Kind == TEXT("anim_node_function");
	}

	/** The retargetable anim nodes referencing FunctionName, resolved through the reference query (the same query the readback uses). */
	void CollectRetargets(const UBlueprint* AnimBlueprint, const FName FunctionName, TArray<FRetarget>& OutRetargets)
	{
		TArray<FCortexMemberReference> References;
		FCortexGraphMemberReferences::FindFunctionReferences(AnimBlueprint, FunctionName, References);

		TArray<UEdGraph*> Graphs;
		AnimBlueprint->GetAllGraphs(Graphs);
		TMap<FGuid, UAnimGraphNode_Base*> NodesByGuid;
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
				{
					NodesByGuid.Add(AnimNode->NodeGuid, AnimNode);
				}
			}
		}

		for (const FCortexMemberReference& Reference : References)
		{
			if (!IsRetargetable(Reference))
			{
				continue;
			}
			UAnimGraphNode_Base* const* Node = NodesByGuid.Find(Reference.NodeGuid);
			if (Node == nullptr)
			{
				continue;
			}
			OutRetargets.Add({ *Node, Reference.Kind, Reference.Slot, Reference.GraphGuid });
		}
	}

	/** Content signature of the retargetable references to a function: node guid, kind and slot, sorted. */
	TArray<FString> ContentSignature(const UBlueprint* AnimBlueprint, const FName FunctionName)
	{
		TArray<FRetarget> Retargets;
		CollectRetargets(AnimBlueprint, FunctionName, Retargets);
		TArray<FString> Signature;
		Signature.Reserve(Retargets.Num());
		for (const FRetarget& Retarget : Retargets)
		{
			Signature.Add(FString::Printf(TEXT("%s|%s|%s"), *GuidText(Retarget.Node->NodeGuid), *Retarget.Kind, *Retarget.Slot));
		}
		Signature.Sort();
		return Signature;
	}

	/** The touched state of one node: its binding map and the three slot references, for the rollback journal. */
	struct FNodeJournal
	{
		UAnimGraphNode_Base* Node = nullptr;
		bool bHasBindingMap = false;
		TMap<FName, FAnimGraphNodePropertyBinding> Bindings;
		TArray<TPair<FName, FMemberReference>> Slots;
	};

	void CaptureJournal(UAnimGraphNode_Base* Node, FNodeJournal& OutJournal)
	{
		OutJournal.Node = Node;
		OutJournal.bHasBindingMap = FCortexAnimBindingReflection::CopyBindingMap(Node, OutJournal.Bindings);
		OutJournal.Slots.Reset();
		TArray<FCortexAnimFunctionSlot> Slots;
		FCortexAnimBindingReflection::ReadFunctionSlots(Node, Slots);
		for (const FCortexAnimFunctionSlot& Slot : Slots)
		{
			OutJournal.Slots.Add({ Slot.PropertyName, *Slot.Reference });
		}
	}

	bool RestoreJournal(const FNodeJournal& Journal)
	{
		if (Journal.Node == nullptr)
		{
			return false;
		}
		bool bOk = true;
		if (Journal.bHasBindingMap)
		{
			bOk &= FCortexAnimBindingReflection::RestoreBindingMap(Journal.Node, Journal.Bindings);
		}
		for (const TPair<FName, FMemberReference>& Slot : Journal.Slots)
		{
			if (FMemberReference* Target = FCortexAnimBindingReflection::FindMutableSlot(Journal.Node, Slot.Key))
			{
				*Target = Slot.Value;
			}
			else
			{
				bOk = false;
			}
		}
		return bOk;
	}

	/** The binding map lives on the node's Binding sub-object, so the undo transaction needs that object modified too. */
	void MarkBindingModified(UAnimGraphNode_Base* Node)
	{
		const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Binding"));
		if (UObject* Binding = BindingProperty != nullptr ? BindingProperty->GetObjectPropertyValue_InContainer(Node) : nullptr)
		{
			Binding->Modify();
		}
	}

	void SetSlot(UAnimGraphNode_Base* Node, const FName PropertyName, const UFunction* Function)
	{
		if (FMemberReference* Slot = FCortexAnimBindingReflection::FindMutableSlot(Node, PropertyName))
		{
			Slot->SetFromField<UFunction>(Function, /*bIsConsideredSelfContext=*/true);
		}
	}

	TArray<TSharedPtr<FJsonValue>> RetargetsToJson(const TArray<FRetarget>& Retargets)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FRetarget& Retarget : Retargets)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_guid"), GuidText(Retarget.Node->NodeGuid));
			Entry->SetStringField(TEXT("graph_guid"), GuidText(Retarget.GraphGuid));
			Entry->SetStringField(TEXT("node_class"), Retarget.Node->GetClass()->GetName());
			Entry->SetStringField(TEXT("kind"), Retarget.Kind);
			Entry->SetStringField(TEXT("slot"), Retarget.Slot);
			Values.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> UnmodifiedToJson(const TArray<FCortexMemberReference>& References)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCortexMemberReference& Reference : References)
		{
			if (!IsRetargetable(Reference))
			{
				Values.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Reference)));
			}
		}
		return Values;
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

	if (!FCortexAnimAssetUtils::CheckExpectedFingerprint(AnimBlueprint, Params, Error))
	{
		return Error;
	}

	// References and the retargetable subset (anim-node function bindings and function slots).
	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindFunctionReferences(AnimBlueprint, FromName, References);
	TArray<FRetarget> Retargets;
	CollectRetargets(AnimBlueprint, FromName, Retargets);

	// Every retargeted node's binding storage must be journallable; a node whose storage is not the default class is refused, never
	// retargeted half-way.
	FString MissingField;
	if (Retargets.Num() > 0 && !FCortexAnimBindingReflection::CheckBindingStorage(MissingField))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("This engine build does not expose the reflected field '%s' needed to journal anim-node bindings"), *MissingField),
			Details(TEXT("to"), AssetPath));
	}

	const TArray<FString> SignatureBefore = ContentSignature(AnimBlueprint, FromName);
	const TArray<FString> SignatureToBefore = ContentSignature(AnimBlueprint, ToName);
	const bool bWasDirty = Package != nullptr && Package->IsDirty();

	if (bDryRun || Retargets.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
		Data->SetStringField(TEXT("from"), From);
		Data->SetStringField(TEXT("to"), To);
		Data->SetBoolField(TEXT("dry_run"), bDryRun);
		Data->SetArrayField(TEXT("would_retarget"), RetargetsToJson(Retargets));
		Data->SetArrayField(TEXT("unmodified_references"), UnmodifiedToJson(References));
		Data->SetStringField(TEXT("compile_status"), TEXT("not_run"));
		Data->SetStringField(TEXT("readback_status"), bDryRun ? TEXT("dry_run") : TEXT("nothing_to_retarget"));
		Data->SetBoolField(TEXT("dirty"), bWasDirty);
		Data->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(AnimBlueprint));
		return FCortexCommandRouter::Success(Data);
	}

	// ── Apply ──
	TArray<FNodeJournal> Journals;
	Journals.Reserve(Retargets.Num());
	bool bMutationFailed = false;
	FString MutationFailure;
	{
		// The transaction covers the graph edits only: a Blueprint compile inside it would replace the recorded objects and leave
		// the undo record unable to restore the binding map. The compile runs once the edits are recorded.
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: rebind animation function")));
		AnimBlueprint->Modify();

		for (const FRetarget& Retarget : Retargets)
		{
			FNodeJournal Journal;
			CaptureJournal(Retarget.Node, Journal);
			Journals.Add(MoveTemp(Journal));

			Retarget.Node->Modify();
			MarkBindingModified(Retarget.Node);
			const UEdGraph* Graph = Retarget.Node->GetGraph();
			Retarget.Node->HandleFunctionRenamed(AnimBlueprint, AnimBlueprint->SkeletonGeneratedClass, const_cast<UEdGraph*>(Graph), FromName, ToName);
			// The engine hook skips the three public function slots (AnimGraphNode_Base.cpp:1097-1103); write each one that names `from`.
			for (const TCHAR* SlotName : { TEXT("InitialUpdateFunction"), TEXT("BecomeRelevantFunction"), TEXT("UpdateFunction") })
			{
				if (FMemberReference* Slot = FCortexAnimBindingReflection::FindMutableSlot(Retarget.Node, SlotName))
				{
					if (Slot->GetMemberName() == FromName)
					{
						SetSlot(Retarget.Node, SlotName, ToFunction);
					}
				}
			}
		}

	}

	{
		bool bFaultInjected = false;
#if WITH_AUTOMATION_TESTS
		bFaultInjected = CompileFailureHook() && CompileFailureHook()(AnimBlueprint);
#endif
		if (bCompile && !bFaultInjected)
		{
			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
		}
		if (bFaultInjected || (bCompile && AnimBlueprint->Status == BS_Error))
		{
			bMutationFailed = true;
			MutationFailure = bFaultInjected ? TEXT("injected compile failure") : TEXT("compile error after the retarget");
		}

		if (bMutationFailed)
		{
			// Journal restore (never FScopedTransaction::Cancel, which does not revert state) → exactly one recovery compile →
			// dirty flag → content readback.
			for (const FNodeJournal& Journal : Journals)
			{
				RestoreJournal(Journal);
			}
			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
			}
			if (Package != nullptr)
			{
				Package->SetDirtyFlag(bWasDirty);
			}

			const bool bContentRestored = ContentSignature(AnimBlueprint, FromName) == SignatureBefore
				&& ContentSignature(AnimBlueprint, ToName) == SignatureToBefore;
			FString RollbackStatus = bContentRestored ? TEXT("restored") : TEXT("verification_failed");
			if (!bContentRestored)
			{
				FCortexAssetMutationGuard::Block(AnimBlueprint, TEXT("anim.rebind_function: content readback did not match the pre-call state after the rollback"));
			}

			TSharedPtr<FJsonObject> Rollback = Details(TEXT("to"), AssetPath);
			Rollback->SetStringField(TEXT("rollback_status"), RollbackStatus);
			Rollback->SetStringField(TEXT("compile_status"), TEXT("failed"));
			Rollback->SetStringField(TEXT("failure"), MutationFailure);
			Rollback->SetBoolField(TEXT("content_restored"), bContentRestored);
			Rollback->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(AnimBlueprint));
			return FCortexCommandRouter::Error(
				bContentRestored ? CortexErrorCodes::CompileFailed : CortexErrorCodes::VerificationFailed,
				FString::Printf(TEXT("Retargeting '%s' to '%s' failed (%s); the pre-call state was %s"), *From, *To, *MutationFailure,
					bContentRestored ? TEXT("restored") : TEXT("not restored, the asset path is blocked")),
				Rollback);
		}
	}

	// ── Readback: content, then the engine virtuals ──
	const TArray<FString> SignatureAfter = ContentSignature(AnimBlueprint, FromName);
	const TArray<FString> SignatureToAfter = ContentSignature(AnimBlueprint, ToName);
	TArray<TSharedPtr<FJsonValue>> DisagreeingNodes;
	for (const FRetarget& Retarget : Retargets)
	{
		if (!Retarget.Node->ReferencesFunction(ToName, nullptr))
		{
			DisagreeingNodes.Add(MakeShared<FJsonValueString>(GuidText(Retarget.Node->NodeGuid)));
		}
	}

	const bool bReadbackOk = SignatureAfter.Num() == 0
		&& SignatureToAfter.Num() == SignatureToBefore.Num() + SignatureBefore.Num()
		&& DisagreeingNodes.Num() == 0;
	if (!bReadbackOk)
	{
		const FString Reason = FString::Printf(
			TEXT("readback mismatch after the retarget (remaining references to '%s': %d, references to '%s': %d, nodes disagreeing: %d)"),
			*From, SignatureAfter.Num(), *To, SignatureToAfter.Num(), DisagreeingNodes.Num());
		FCortexAssetMutationGuard::Block(AnimBlueprint, TEXT("anim.rebind_function: ") + Reason);
		TSharedPtr<FJsonObject> Mismatch = Details(TEXT("to"), AssetPath);
		Mismatch->SetStringField(TEXT("rollback_status"), TEXT("verification_failed"));
		Mismatch->SetArrayField(TEXT("disagreeing_nodes"), DisagreeingNodes);
		return FCortexCommandRouter::Error(CortexErrorCodes::VerificationFailed, Reason, Mismatch);
	}

	TArray<FString> SavedPackages;
	if (bSave)
	{
		FString Saved;
		if (!FCortexAnimAssetUtils::SaveAsset(AnimBlueprint, Saved, Error))
		{
			return Error;
		}
		SavedPackages.Add(Saved);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetStringField(TEXT("from"), From);
	Data->SetStringField(TEXT("to"), To);
	Data->SetBoolField(TEXT("dry_run"), false);
	Data->SetArrayField(TEXT("retargeted"), RetargetsToJson(Retargets));
	Data->SetArrayField(TEXT("unmodified_references"), UnmodifiedToJson(References));
	Data->SetStringField(TEXT("compile_status"), bCompile ? (AnimBlueprint->Status == BS_UpToDate ? TEXT("success") : TEXT("warning")) : TEXT("not_run"));
	Data->SetStringField(TEXT("readback_status"), TEXT("matched"));
	Data->SetBoolField(TEXT("declared_in_native"), ToFunction->GetOuterUClass() != nullptr
		&& !ToFunction->GetOuterUClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint));
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
void FCortexAnimRebindOps::SetCompileFailureHookForTest(TFunction<bool(UAnimBlueprint*)> Hook)
{
	CortexAnimRebindPrivate::CompileFailureHook() = MoveTemp(Hook);
}

void FCortexAnimRebindOps::ClearCompileFailureHookForTest()
{
	CortexAnimRebindPrivate::CompileFailureHook() = nullptr;
}
#endif
