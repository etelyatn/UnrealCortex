#include "Operations/CortexBPGuardedRun.h"

#include "CortexAssetMutationGuard.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexGraphFingerprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_Event.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*)> GPostRestoreHookForTest;
	bool GSkipUndoForTest = false;
	bool GForceSaveFailureForTest = false;
}
#endif

namespace
{
	struct FRunState
	{
		bool bDryRun = false;
		FString ApplyStatus = TEXT("not_run");
		bool bChanged = false;
		FString CompileStatus = TEXT("skipped");
		FCortexBPCompileOutcome Compile;
		FString ReadbackStatus = TEXT("skipped");
		FString RollbackStatus = TEXT("not_needed");
		FString SaveStatus = TEXT("skipped");
		bool bSaved = false;
		bool bBlocked = false;
		bool bDirtyBefore = false;
		bool bDirtyAfter = false;
		TSharedPtr<FJsonObject> FingerprintBefore;
		TSharedPtr<FJsonObject> FingerprintAfter;
	};

	TSharedPtr<FJsonObject> ToOutcomeJson(const FRunState& State)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("dry_run"), State.bDryRun);
		Json->SetStringField(TEXT("apply_status"), State.ApplyStatus);
		Json->SetBoolField(TEXT("changed"), State.bChanged);
		Json->SetStringField(TEXT("compile_status"), State.CompileStatus);
		Json->SetNumberField(TEXT("error_count"), State.Compile.ErrorCount);
		Json->SetNumberField(TEXT("warning_count"), State.Compile.WarningCount);
		Json->SetArrayField(TEXT("diagnostics"), State.Compile.Diagnostics);
		Json->SetStringField(TEXT("readback_status"), State.ReadbackStatus);
		Json->SetStringField(TEXT("rollback_status"), State.RollbackStatus);
		Json->SetStringField(TEXT("save_status"), State.SaveStatus);
		Json->SetBoolField(TEXT("saved"), State.bSaved);
		Json->SetBoolField(TEXT("blocked"), State.bBlocked);
		Json->SetBoolField(TEXT("dirty_before"), State.bDirtyBefore);
		Json->SetBoolField(TEXT("dirty_after"), State.bDirtyAfter);
		Json->SetObjectField(TEXT("fingerprint_before"), State.FingerprintBefore);
		Json->SetObjectField(TEXT("fingerprint_after"), State.FingerprintAfter);
		return Json;
	}

	bool FingerprintsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A.IsValid() && B.IsValid() && FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(A), *MakeShared<FJsonValueObject>(B));
	}

	FString GraphAuthoringHash(UBlueprint* Blueprint)
	{
		const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphFingerprint::Compute(Blueprint);
		return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
	}

	/**
	 * Generic recovery journal: parent, NewVariables, interfaces, a DuplicateObject copy of every top-level graph (composite
	 * and nested graphs are subobjects, so they are copied with it) and the SCS / inheritable-component template properties.
	 */
	struct FJournal
	{
		UClass* Parent = nullptr;
		TArray<FBPVariableDescription> NewVariables;
		TArray<FBPInterfaceDescription> Interfaces;
		TStrongObjectPtr<UBlueprint> Holder;

		struct FGraphCopy
		{
			/** 0 ubergraph, 1 function, 2 macro, 3 delegate signature, 4 interface implementation graph. */
			int32 Category = 0;
			/** Category 4: the implemented interface that owns the graph. */
			UClass* Interface = nullptr;
			TStrongObjectPtr<UEdGraph> Copy;
		};
		TArray<FGraphCopy> Graphs;

		/**
		 * Exact EventReference of every event node, by node guid. DuplicateObject runs UK2Node_Event::PostDuplicate ->
		 * FixupEventReference, which rewrites an interface event to the Blueprint's own class (the skeleton class lists the
		 * interface's skeleton, so ImplementsInterface fails), so the graph copies cannot carry the reference themselves.
		 */
		TMap<FGuid, FMemberReference> EventReferences;

		struct FScsState
		{
			FName Variable;
			FName ParentVariable;
			FName AttachTo;
			FName OwnerClassName;
			bool bParentNative = false;
		};
		TArray<FScsState> ScsStates;

		struct FTemplateCopy
		{
			FName Name;
			bool bInheritable = false;
			TStrongObjectPtr<UActorComponent> Copy;
		};
		TArray<FTemplateCopy> Templates;

		/**
		 * A copy of one property value in memory owned by the journal. The property is a native or parent-class property, so it outlives
		 * the recompile that regenerates the Blueprint's own class (a property declared by the Blueprint's own class is not journaled).
		 */
		struct FOwnedValue
		{
			const FProperty* Property = nullptr;
			void* Memory = nullptr;

			FOwnedValue() = default;
			FOwnedValue(const FOwnedValue&) = delete;
			FOwnedValue& operator=(const FOwnedValue&) = delete;
			~FOwnedValue()
			{
				if (Memory != nullptr)
				{
					Property->DestroyValue(Memory);
					FMemory::Free(Memory);
				}
			}
		};
		/** CDO property name -> value copy (only with FSpec::bJournalCdo). */
		TMap<FName, TSharedPtr<FOwnedValue>> CdoValues;

		FString GraphHash;
		FString GeneratedDigest;
		FString ExtraDigest;
	};

	TArray<TObjectPtr<UEdGraph>>* GraphArray(UBlueprint* Blueprint, int32 Category)
	{
		switch (Category)
		{
		case 0: return &Blueprint->UbergraphPages;
		case 1: return &Blueprint->FunctionGraphs;
		case 2: return &Blueprint->MacroGraphs;
		default: return &Blueprint->DelegateSignatureGraphs;
		}
	}

	UInheritableComponentHandler* InheritableHandler(UBlueprint* Blueprint)
	{
		UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		return Class ? Class->GetInheritableComponentHandler(false) : nullptr;
	}

	bool CaptureJournal(UBlueprint* Blueprint, const FCortexBPGuardedRun::FSpec& Spec, FJournal& Journal)
	{
		Journal.Parent = Blueprint->ParentClass;
		Journal.NewVariables = Blueprint->NewVariables;
		Journal.Interfaces = Blueprint->ImplementedInterfaces;
		Journal.Holder = TStrongObjectPtr<UBlueprint>(NewObject<UBlueprint>(GetTransientPackage()));
		if (!Journal.Holder.IsValid())
		{
			return false;
		}
		// Copied call nodes fix their self references up against their owning Blueprint (UK2Node_CallFunction::PostDuplicate), which
		// resolves them against its skeleton class. A bare holder has none, so the engine would log a warning for every copied self call.
		// With the Blueprint's generated class set as well, FBlueprintEditorUtils::GatherDependencies treats the holder like a PIE copy
		// (generated by someone else) and skips it, so no dependency scan or rename ever reaches the copies.
		if (Blueprint->GeneratedClass != nullptr && Blueprint->SkeletonGeneratedClass != nullptr)
		{
			Journal.Holder->GeneratedClass = Blueprint->GeneratedClass;
			Journal.Holder->SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass;
		}

		const auto CopyGraphs = [&Journal](const TArray<TObjectPtr<UEdGraph>>& Graphs, int32 Category, UClass* Interface)
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph == nullptr)
				{
					continue;
				}
				UEdGraph* Copy = DuplicateObject<UEdGraph>(Graph, Journal.Holder.Get(), Graph->GetFName());
				if (Copy == nullptr)
				{
					return false;
				}
				FJournal::FGraphCopy& Saved = Journal.Graphs.AddDefaulted_GetRef();
				Saved.Category = Category;
				Saved.Interface = Interface;
				Saved.Copy = TStrongObjectPtr<UEdGraph>(Copy);
			}
			return true;
		};
		for (int32 Category = 0; Category < 4; ++Category)
		{
			if (!CopyGraphs(*GraphArray(Blueprint, Category), Category, nullptr))
			{
				return false;
			}
		}
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			if (!CopyGraphs(Description.Graphs, 4, Description.Interface))
			{
				return false;
			}
		}

		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node == nullptr)
				{
					continue;
				}
				FJournal::FScsState& State = Journal.ScsStates.AddDefaulted_GetRef();
				State.Variable = Node->GetVariableName();
				State.ParentVariable = Node->ParentComponentOrVariableName;
				State.AttachTo = Node->AttachToName;
				State.OwnerClassName = Node->ParentComponentOwnerClassName;
				State.bParentNative = Node->bIsParentComponentNative;
				if (Node->ComponentTemplate != nullptr)
				{
					UActorComponent* Copy = DuplicateObject<UActorComponent>(Node->ComponentTemplate, GetTransientPackage());
					if (Copy == nullptr)
					{
						return false;
					}
					FJournal::FTemplateCopy& Saved = Journal.Templates.AddDefaulted_GetRef();
					Saved.Name = Node->GetVariableName();
					Saved.Copy = TStrongObjectPtr<UActorComponent>(Copy);
				}
			}
		}
		if (UInheritableComponentHandler* Handler = InheritableHandler(Blueprint))
		{
			TArray<UActorComponent*> Overrides;
			Handler->GetAllTemplates(Overrides);
			for (UActorComponent* Template : Overrides)
			{
				if (Template == nullptr)
				{
					continue;
				}
				UActorComponent* Copy = DuplicateObject<UActorComponent>(Template, GetTransientPackage());
				if (Copy == nullptr)
				{
					return false;
				}
				FJournal::FTemplateCopy& Saved = Journal.Templates.AddDefaulted_GetRef();
				Saved.Name = Template->GetFName();
				Saved.bInheritable = true;
				Saved.Copy = TStrongObjectPtr<UActorComponent>(Copy);
			}
		}

		{
			TArray<UEdGraph*> LiveGraphs;
			Blueprint->GetAllGraphs(LiveGraphs);
			for (const UEdGraph* Graph : LiveGraphs)
			{
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
					{
						Journal.EventReferences.Add(Event->NodeGuid, Event->EventReference);
					}
				}
			}
		}
		Journal.GraphHash = GraphAuthoringHash(Blueprint);
		Journal.GeneratedDigest = FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint);
		Journal.ExtraDigest = Spec.DigestExtra ? Spec.DigestExtra(Blueprint) : FString();
		return true;
	}

	/** Writes the journaled EventReference back onto every event node of a graph that was duplicated or recompiled. */
	void RestoreEventReferences(const FJournal& Journal, UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			const FMemberReference* Saved = Event != nullptr ? Journal.EventReferences.Find(Event->NodeGuid) : nullptr;
			if (Saved != nullptr)
			{
				Event->EventReference = *Saved;
			}
		}
	}

	/** Reconciles the Blueprint with the journal; idempotent after an undo that already restored everything. */
	bool RestoreFromJournal(UBlueprint* Blueprint, FJournal& Journal)
	{
		bool bOk = true;
		Blueprint->ParentClass = Journal.Parent;
		Blueprint->NewVariables = Journal.NewVariables;
		Blueprint->ImplementedInterfaces = Journal.Interfaces;

		if (GraphAuthoringHash(Blueprint) != Journal.GraphHash)
		{
			const auto Rebuild = [&](TArray<TObjectPtr<UEdGraph>>& Array, int32 Category, UClass* Interface)
			{
				TArray<UEdGraph*> Replaced;
				for (UEdGraph* Graph : Array)
				{
					if (Graph != nullptr)
					{
						Replaced.Add(Graph);
						Graph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
					}
				}
				Array.Reset();
				for (const FJournal::FGraphCopy& Saved : Journal.Graphs)
				{
					if (Saved.Category != Category || Saved.Interface != Interface)
					{
						continue;
					}
					UEdGraph* Fresh = DuplicateObject<UEdGraph>(Saved.Copy.Get(), Blueprint, Saved.Copy->GetFName());
					if (Fresh == nullptr)
					{
						bOk = false;
						continue;
					}
					RestoreEventReferences(Journal, Fresh);
					Array.Add(Fresh);
				}
				for (UEdGraph* Old : Replaced)
				{
					Old->MarkAsGarbage();
				}
			};
			for (int32 Category = 0; Category < 4; ++Category)
			{
				Rebuild(*GraphArray(Blueprint, Category), Category, nullptr);
			}
			for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
			{
				Rebuild(Description.Graphs, 4, Description.Interface);
			}
		}

		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (const FJournal::FScsState& State : Journal.ScsStates)
			{
				USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(State.Variable);
				if (Node == nullptr)
				{
					bOk = false; // a structural SCS change is only recoverable through the undo
					continue;
				}
				Node->ParentComponentOrVariableName = State.ParentVariable;
				Node->AttachToName = State.AttachTo;
				Node->ParentComponentOwnerClassName = State.OwnerClassName;
				Node->bIsParentComponentNative = State.bParentNative;
			}
		}
		for (const FJournal::FTemplateCopy& Saved : Journal.Templates)
		{
			UActorComponent* Current = nullptr;
			if (!Saved.bInheritable)
			{
				USCS_Node* Node = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(Saved.Name) : nullptr;
				Current = Node ? Node->ComponentTemplate : nullptr;
			}
			else if (UInheritableComponentHandler* Handler = InheritableHandler(Blueprint))
			{
				TArray<UActorComponent*> Overrides;
				Handler->GetAllTemplates(Overrides);
				for (UActorComponent* Candidate : Overrides)
				{
					if (Candidate != nullptr && Candidate->GetFName() == Saved.Name)
					{
						Current = Candidate;
						break;
					}
				}
			}
			if (Current == nullptr)
			{
				bOk = false;
				continue;
			}
			// Full copy, not a delta against the class default: a property the mutation moved away from its default must come back.
			UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
			CopyParams.bDoDelta = false;
			CopyParams.bNotifyObjectReplacement = false;
			UEngine::CopyPropertiesForUnrelatedObjects(Saved.Copy.Get(), Current, CopyParams);
		}
		return bOk;
	}

	/**
	 * The recovery compile reconstructs event nodes against the skeleton class it starts from, and FixupEventReference rewrites
	 * an interface event to the Blueprint's own class when that skeleton does not implement the interface yet. The compile
	 * must not change what the journal restored, so the exact pre-state references are written back afterwards.
	 */
	void ReapplyEventReferences(UBlueprint* Blueprint, const FJournal& Journal)
	{
		TArray<UEdGraph*> Live;
		Blueprint->GetAllGraphs(Live);
		for (UEdGraph* Graph : Live)
		{
			if (Graph != nullptr)
			{
				RestoreEventReferences(Journal, Graph);
			}
		}
	}

	/**
	 * A CDO property the journal carries: one the #124 snapshot tracks, declared outside the Blueprint's own class (that class's properties
	 * are regenerated by the compile, which invalidates them; their defaults come back with the journaled variable defaults and the
	 * recovery compile), holding no instanced subobject and no strong object reference (the compile can replace or free the referent, and a
	 * pointer copy would dangle).
	 */
	bool IsJournaledCdoProperty(const FProperty* Property, const UBlueprint* Blueprint)
	{
		const UClass* Owner = Property->GetOwnerClass();
		if (Owner == nullptr || Owner == Blueprint->GeneratedClass.Get() || Owner == Blueprint->SkeletonGeneratedClass.Get()
			|| !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
			|| Property->HasAnyPropertyFlags(CPF_Deprecated)
			|| Property->ArrayDim != 1
			|| Property->ContainsInstancedObjectProperty()
			|| CastField<FDelegateProperty>(Property) != nullptr
			|| CastField<FMulticastDelegateProperty>(Property) != nullptr)
		{
			return false;
		}
		TArray<const FStructProperty*> EncounteredStructProps;
		return !Property->ContainsObjectReference(EncounteredStructProps);
	}

	void CaptureCdo(UBlueprint* Blueprint, FJournal& Journal)
	{
		const UClass* Class = Blueprint->GeneratedClass;
		const UObject* Cdo = Class != nullptr ? Class->GetDefaultObject(/*bCreateIfNeeded=*/false) : nullptr;
		if (Cdo == nullptr)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsJournaledCdoProperty(Property, Blueprint))
			{
				continue;
			}
			TSharedRef<FJournal::FOwnedValue> Value = MakeShared<FJournal::FOwnedValue>();
			Value->Property = Property;
			Value->Memory = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			Property->InitializeValue(Value->Memory);
			Property->CopyCompleteValue(Value->Memory, Property->ContainerPtrToValuePtr<void>(Cdo));
			Journal.CdoValues.Add(Property->GetFName(), Value);
		}
	}

	/** Writes every journaled CDO value that differs back (compared and copied with the property itself, never through text). */
	bool RestoreCdo(UBlueprint* Blueprint, const FJournal& Journal)
	{
		if (Journal.CdoValues.IsEmpty())
		{
			return true;
		}
		UClass* Class = Blueprint->GeneratedClass;
		UObject* Cdo = Class != nullptr ? Class->GetDefaultObject(/*bCreateIfNeeded=*/false) : nullptr;
		if (Cdo == nullptr)
		{
			return false;
		}
		bool bOk = true;
		for (const TPair<FName, TSharedPtr<FJournal::FOwnedValue>>& Pair : Journal.CdoValues)
		{
			const FJournal::FOwnedValue& Saved = *Pair.Value;
			const FProperty* Current = Class->FindPropertyByName(Pair.Key);
			if (Current != Saved.Property)
			{
				bOk = false; // the property is gone or was replaced: it cannot be restored
				continue;
			}
			void* Live = Current->ContainerPtrToValuePtr<void>(Cdo);
			if (!Current->Identical(Live, Saved.Memory, PPF_None))
			{
				Current->CopyCompleteValue(Live, Saved.Memory);
				bOk = bOk && Current->Identical(Live, Saved.Memory, PPF_None);
			}
		}
		return bOk;
	}

	/** Records every package marked dirty while it is alive (also a package that was dirty already), with its state before the first mark. */
	class FDirtiedPackageTracker
	{
	public:
		FDirtiedPackageTracker()
		{
			MarkedHandle = UPackage::PackageMarkedDirtyEvent.AddRaw(this, &FDirtiedPackageTracker::OnMarkedDirty);
			StateHandle = UPackage::PackageDirtyStateChangedEvent.AddRaw(this, &FDirtiedPackageTracker::OnDirtyStateChanged);
		}

		~FDirtiedPackageTracker()
		{
			UPackage::PackageMarkedDirtyEvent.Remove(MarkedHandle);
			UPackage::PackageDirtyStateChangedEvent.Remove(StateHandle);
		}

		FDirtiedPackageTracker(const FDirtiedPackageTracker&) = delete;
		FDirtiedPackageTracker& operator=(const FDirtiedPackageTracker&) = delete;

		/** reload_packages entries, sorted by package name. The target's own package, transient, script and PIE packages are skipped. */
		TArray<TSharedPtr<FJsonValue>> BuildReloadPackages(const UPackage* Own) const
		{
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Entries;
			for (const TPair<TWeakObjectPtr<UPackage>, bool>& Pair : Packages)
			{
				const UPackage* Package = Pair.Key.Get();
				if (Package == nullptr || Package == Own || Package == GetTransientPackage()
					|| Package->HasAnyPackageFlags(PKG_PlayInEditor))
				{
					continue;
				}
				const FString Name = Package->GetName();
				if (FPackageName::IsScriptPackage(Name))
				{
					continue;
				}
				FString File;
				if (!FPackageName::TryConvertLongPackageNameToFilename(Name, File, FPackageName::GetAssetPackageExtension())
					|| !IFileManager::Get().FileExists(*File))
				{
					File.Reset();
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("package"), Name);
				Entry->SetStringField(TEXT("file"), File);
				Entry->SetBoolField(TEXT("was_dirty_before"), Pair.Value);
				const TSharedPtr<FJsonValue> EntryValue = MakeShared<FJsonValueObject>(Entry);
				Entries.Emplace(Name, EntryValue);
			}
			Entries.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Entries)
			{
				Out.Add(Entry.Value);
			}
			return Out;
		}

	private:
		void OnMarkedDirty(UPackage* Package, bool bWasDirty)
		{
			Packages.FindOrAdd(TWeakObjectPtr<UPackage>(Package), bWasDirty);
		}

		void OnDirtyStateChanged(UPackage* Package)
		{
			// Only the clean -> dirty transition is a modification.
			if (Package != nullptr && Package->IsDirty())
			{
				Packages.FindOrAdd(TWeakObjectPtr<UPackage>(Package), false);
			}
		}

		TMap<TWeakObjectPtr<UPackage>, bool> Packages;
		FDelegateHandle MarkedHandle;
		FDelegateHandle StateHandle;
	};
}

FCortexCommandResult FCortexBPGuardedRun::Execute(const FSpec& Spec)
{
	UBlueprint* Blueprint = Spec.Blueprint;
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: no Blueprint to run against"), *Spec.OpName));
	}
	if (Spec.bDryRun && Spec.bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (Spec.bSave && !Spec.bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
	}
	if (!Spec.bDryRun && (!Spec.Apply || !Spec.Verify))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: apply and verify steps are required"), *Spec.OpName));
	}

	UPackage* Package = Blueprint->GetOutermost();
	FRunState State;
	State.bDryRun = Spec.bDryRun;
	State.bDirtyBefore = Package->IsDirty();
	State.bDirtyAfter = State.bDirtyBefore;

	if (!Spec.bDryRun)
	{
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		}
	}

	if (Spec.Expected.IsValid())
	{
		FCortexCommandResult ExpectedError;
		if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Spec.Expected, ExpectedError))
		{
			return ExpectedError;
		}
	}

	State.FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	State.FingerprintAfter = State.FingerprintBefore;

	if (Spec.bDryRun)
	{
		State.ApplyStatus = TEXT("dry_run");
		return FCortexCommandRouter::Success(ToOutcomeJson(State));
	}

	FString SnapshotError;
	const TSharedPtr<FJsonObject> SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, Spec.SnapshotSections, SnapshotError);
	if (!SnapshotBefore.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: could not record the pre-call snapshot: %s"), *Spec.OpName, *SnapshotError));
	}

	FJournal Journal;
	TUniquePtr<FDirtiedPackageTracker> DirtiedPackages;
	if (Spec.bReportDirtiedPackages)
	{
		DirtiedPackages = MakeUnique<FDirtiedPackageTracker>();
	}

	if (!CaptureJournal(Blueprint, Spec, Journal))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: could not record the recovery journal"), *Spec.OpName));
	}
	if (Spec.bJournalCdo)
	{
		CaptureCdo(Blueprint, Journal);
	}

	const EBlueprintStatus StatusBefore = Blueprint->Status;
	// Inside an outer transaction (router batch) our transaction is nested: it has no entry of its own and the editor refuses to
	// undo while a transaction is active, so only the journal can restore.
	const bool bOuterTransaction = GEditor != nullptr && GEditor->IsTransactionActive();
	// A run that opts out of the editor undo does not begin a transaction at all unless it asks for the undo step (bRecordUndoStep):
	// it neither records an entry (a state the undo cannot rebuild) nor needs one, so a user undo after a successful adoption
	// cannot produce that state either.
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
		FText::FromString(FString::Printf(TEXT("Cortex: %s"), *Spec.OpName)),
		/*bShouldActuallyTransact=*/Spec.bUndoOnFailure || Spec.bRecordUndoStep);
	Blueprint->Modify();
	// The guarded transaction is identified by id, not by undo-list growth: redo history or eviction keeps the length flat.
	const FGuid TransactionId = (!bOuterTransaction && GEditor && GEditor->Trans) ? GEditor->Trans->GetUndoContext(false).TransactionId : FGuid();

	FString FailurePhase;
	FString FailureMessage;
	FString FailureCode;
	if (Spec.Capture && !Spec.Capture(Blueprint, FailureMessage))
	{
		FailurePhase = TEXT("capture");
		FailureCode = CortexErrorCodes::InvalidOperation;
		State.ApplyStatus = TEXT("failed");
	}
	else if (!Spec.Apply(Blueprint, FailureMessage))
	{
		FailurePhase = TEXT("apply");
		FailureCode = CortexErrorCodes::InvalidOperation;
		State.ApplyStatus = TEXT("failed");
	}
	else if (Spec.VerifyBeforeCompile && !Spec.VerifyBeforeCompile(Blueprint, FailureMessage))
	{
		State.ApplyStatus = TEXT("applied");
		State.bChanged = true;
		State.ReadbackStatus = TEXT("mismatched");
		FailurePhase = TEXT("readback");
		FailureCode = CortexErrorCodes::ReadbackMismatch;
	}
	else
	{
		State.ApplyStatus = TEXT("applied");
		State.bChanged = true;
		if (Spec.bCompile)
		{
			FCortexBPCompileSupport::Compile(Blueprint, Spec.CompileOptions, State.Compile);
			State.CompileStatus = State.Compile.Status;
			if (State.Compile.HasErrors() || Blueprint->Status == BS_Error)
			{
				FailurePhase = TEXT("compile");
				FailureCode = CortexErrorCodes::CompileFailed;
				FailureMessage = FString::Printf(TEXT("Blueprint compilation failed with %d errors"), State.Compile.ErrorCount);
			}
		}
		if (FailurePhase.IsEmpty())
		{
			if (Spec.Verify(Blueprint, Spec.bCompile, FailureMessage))
			{
				State.ReadbackStatus = TEXT("matched");
			}
			else
			{
				State.ReadbackStatus = TEXT("mismatched");
				FailurePhase = TEXT("readback");
				FailureCode = CortexErrorCodes::ReadbackMismatch;
			}
		}
	}

	if (!FailurePhase.IsEmpty())
	{
		const bool bParentChanged = Blueprint->ParentClass != Journal.Parent;

		// End the guarded transaction (recorded, not cancelled: Cancel restores nothing), then restore. A run that opted out of the
		// undo holds no transaction; cancelling a nested one would cancel the whole outer transaction (a router batch), so a
		// nested run never cancels.
		if (!Spec.bUndoOnFailure && Transaction.IsValid() && !bOuterTransaction)
		{
			Transaction->Cancel();
		}
		Transaction.Reset();
		bool bUndone = true;
		bool bSkipUndo = false;
#if WITH_DEV_AUTOMATION_TESTS
		bSkipUndo = GSkipUndoForTest;
#endif
		if (Spec.bUndoOnFailure && !bSkipUndo && TransactionId.IsValid() && GEditor != nullptr && GEditor->Trans != nullptr
			&& GEditor->Trans->GetUndoContext(false).TransactionId == TransactionId)
		{
			bUndone = GEditor->UndoTransaction(/*bCanRedo=*/false);
		}
		const bool bJournalRestored = RestoreFromJournal(Blueprint, Journal);
		FString RestoreError;
		const bool bOwnRestored = !Spec.Restore || Spec.Restore(Blueprint, RestoreError);
		const bool bRestored = bJournalRestored && bOwnRestored && (bUndone || bSkipUndo);

		FCortexBPCompileOutcome Recovery;
		if (Spec.bCompile)
		{
			// Exactly one recovery compile; it reuses the failed compile's options when the parent changed in the run so
			// restored inherited CDO values are not overwritten by the new parent's constructor defaults.
			const EBlueprintCompileOptions RecoveryOptions = bParentChanged
				? State.Compile.EffectiveOptions
				: EBlueprintCompileOptions::None;
			FCortexBPCompileSupport::Compile(Blueprint, RecoveryOptions, Recovery);
		}

		const bool bCdoRestored = !Spec.bJournalCdo || RestoreCdo(Blueprint, Journal);
		const bool bPostRecoveryOk = !Spec.PostRecovery || Spec.PostRecovery(Blueprint, RestoreError);
		const bool bPostRecovered = bCdoRestored && bPostRecoveryOk;
		if (GraphAuthoringHash(Blueprint) != Journal.GraphHash)
		{
			ReapplyEventReferences(Blueprint, Journal);
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (GPostRestoreHookForTest)
		{
			GPostRestoreHookForTest(Blueprint);
		}
#endif

		FString AfterError;
		const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, Spec.SnapshotSections, AfterError);
		bool bSnapshotsEqual = false;
		if (SnapshotAfter.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> Differences;
			TSharedPtr<FJsonObject> Counts;
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(SnapshotBefore, SnapshotAfter, Spec.SnapshotSections, Differences, Counts);
			bSnapshotsEqual = Diff.bSuccess && Differences.Num() == 0;
		}
		const TSharedPtr<FJsonObject> FingerprintRestored = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		const bool bFingerprintEqual = FingerprintsEqual(FingerprintRestored, State.FingerprintBefore);
		const bool bGraphEqual = GraphAuthoringHash(Blueprint) == Journal.GraphHash;
		const bool bGeneratedEqual = !Spec.bCompile
			|| FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint) == Journal.GeneratedDigest;
		const bool bExtraEqual = !Spec.DigestExtra || Spec.DigestExtra(Blueprint) == Journal.ExtraDigest;
		const bool bRecoveryCompileOk = !Spec.bCompile || !Recovery.HasErrors() || StatusBefore == BS_Error;
		const bool bVerified = bRestored && bPostRecovered && bRecoveryCompileOk && bSnapshotsEqual && bFingerprintEqual && bGraphEqual && bGeneratedEqual && bExtraEqual;

		if (bVerified)
		{
			Package->SetDirtyFlag(State.bDirtyBefore);
			State.RollbackStatus = TEXT("restored");
			State.bChanged = false;
		}
		else
		{
			Package->SetDirtyFlag(true);
			FCortexAssetMutationGuard::Block(Blueprint,
				FString::Printf(TEXT("%s rollback verification failed"), *Spec.OpName));
			State.RollbackStatus = TEXT("unverified");
			State.bBlocked = true;
		}
		if (Spec.bCompile)
		{
			State.CompileStatus = FailurePhase == TEXT("compile") ? TEXT("failed") : State.CompileStatus;
		}
		State.FingerprintAfter = FingerprintRestored;
		State.bDirtyAfter = Package->IsDirty();

		TSharedPtr<FJsonObject> Details = ToOutcomeJson(State);
		Details->SetStringField(TEXT("failure_phase"), FailurePhase);
		Details->SetStringField(TEXT("failure_message"), FailureMessage);
		Details->SetBoolField(TEXT("rollback_content_restored"), bRestored);
		Details->SetBoolField(TEXT("rollback_snapshot_matches"), bSnapshotsEqual);
		Details->SetBoolField(TEXT("rollback_fingerprint_matches"), bFingerprintEqual);
		Details->SetBoolField(TEXT("rollback_graphs_match"), bGraphEqual);
		Details->SetBoolField(TEXT("rollback_generated_state_matches"), bGeneratedEqual);
		Details->SetBoolField(TEXT("rollback_extra_digest_matches"), bExtraEqual);
		if (Spec.bJournalCdo)
		{
			Details->SetBoolField(TEXT("rollback_cdo_restored"), bCdoRestored);
		}
		if (DirtiedPackages.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>> ReloadPackages = DirtiedPackages->BuildReloadPackages(Package);
			Details->SetArrayField(TEXT("reload_packages"), ReloadPackages);
			if (!ReloadPackages.IsEmpty())
			{
				Details->SetStringField(TEXT("reload_instruction"),
					TEXT("The failed run modified these other loaded packages in memory and the rollback restores only the target Blueprint. "
						"Reload each from disk before saving anything (Content Browser: right-click the asset, Asset Actions, Reload; or close and reopen the asset). "
						"A package with was_dirty_before=true carried unsaved edits that the reload also discards."));
			}
		}
		if (Spec.bCompile)
		{
			Details->SetStringField(TEXT("recovery_compile_status"), Recovery.Status);
			Details->SetArrayField(TEXT("recovery_diagnostics"), Recovery.Diagnostics);
		}
		if (!RestoreError.IsEmpty())
		{
			Details->SetStringField(TEXT("rollback_error"), RestoreError);
		}
		return FCortexCommandRouter::Error(FailureCode,
			FString::Printf(TEXT("%s %s failed: %s"), *Spec.OpName, *FailurePhase, *FailureMessage), Details);
	}

	Transaction.Reset();
	Package->MarkPackageDirty();

	if (Spec.bSave)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		SaveParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		FCortexCommandResult SaveResult;
		bool bForcedSaveFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
		bForcedSaveFailure = GForceSaveFailureForTest;
#endif
		if (bForcedSaveFailure)
		{
			SaveResult = FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, TEXT("Forced save failure"));
		}
		else
		{
			SaveResult = FCortexBPAssetOps::Save(SaveParams);
		}
		State.SaveStatus = SaveResult.bSuccess ? TEXT("saved") : TEXT("failed");
		State.bSaved = SaveResult.bSuccess;
		State.FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		State.bDirtyAfter = Package->IsDirty();
		if (!SaveResult.bSuccess)
		{
			return FCortexCommandRouter::Error(
				SaveResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : SaveResult.ErrorCode,
				SaveResult.ErrorMessage, ToOutcomeJson(State));
		}
	}

	State.FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	State.bDirtyAfter = Package->IsDirty();
	return FCortexCommandRouter::Success(ToOutcomeJson(State));
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPGuardedRun::SetPostRestoreHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	GPostRestoreHookForTest = MoveTemp(Hook);
}

void FCortexBPGuardedRun::ClearPostRestoreHookForTest()
{
	GPostRestoreHookForTest = nullptr;
}

void FCortexBPGuardedRun::SetSkipUndoForTest(bool bSkip)
{
	GSkipUndoForTest = bSkip;
}

void FCortexBPGuardedRun::SetForceSaveFailureForTest(bool bForce)
{
	GForceSaveFailureForTest = bForce;
}
#endif
