#pragma once

#include "Misc/EngineVersionComparison.h"
#include "GameplayTagContainer.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeState.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "PropertyBindingPath.h"
#include "StateTreePropertyBindings.h"
#endif
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "IMessageLogListing.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "StateTreeCompiler.h"
#include "UObject/UnrealType.h"
#else
#include "StateTreeEditingSubsystem.h"
#endif

// UE 5.4 compatibility shims for StateTree editor APIs introduced in 5.5+:
// UStateTreeState::GetPath/Tag/RequiredEventToEnter and
// FStateTreeTransition::RequiredEvent (5.4 exposes EventTag instead).
namespace CortexSTCompat
{
inline FString GetStatePath(const UStateTreeState* State)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	// UStateTreeState::GetPath() does not exist in 5.4 — rebuild "Root/Child/Leaf"
	// from the parent chain.
	TArray<FString> Segments;
	for (const UStateTreeState* Current = State; Current != nullptr; Current = Current->Parent)
	{
		Segments.Insert(Current->Name.ToString(), 0);
	}
	return FString::Join(Segments, TEXT("/"));
#else
	return State != nullptr ? State->GetPath() : FString();
#endif
}

inline FGameplayTag GetStateTag(const UStateTreeState* State)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	return FGameplayTag();
#else
	return State != nullptr ? State->Tag : FGameplayTag();
#endif
}

// Returns false when the engine has no per-state gameplay tag (UE 5.4) so the
// caller can surface an explicit unsupported-field error instead of silently
// dropping the value.
inline bool SetStateTag(UStateTreeState& State, const FGameplayTag& Tag)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	return false;
#else
	State.Tag = Tag;
	return true;
#endif
}

inline bool SetStateRequiredEventToEnter(UStateTreeState& State, const FGameplayTag& Tag)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	return false;
#else
	State.RequiredEventToEnter.Tag = Tag;
	State.bHasRequiredEventToEnter = State.RequiredEventToEnter.IsValid();
	return true;
#endif
}

inline FGameplayTag GetTransitionEventTag(const FStateTreeTransition& Transition)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	return Transition.EventTag;
#else
	return Transition.RequiredEvent.Tag;
#endif
}

inline void SetTransitionEventTag(FStateTreeTransition& Transition, const FGameplayTag& EventTag)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	Transition.EventTag = EventTag;
#else
	Transition.RequiredEvent.Tag = EventTag;
#endif
}

#if UE_VERSION_OLDER_THAN(5, 5, 0)
namespace Private
{
// Mirrors the engine's private FStateTreeObjectCRC32 (StateTreeObjectHash.h):
// identical skip rules so LastCompiledEditorDataHash matches what the editor
// itself would compute for the same data.
class FCortexStateTreeObjectCRC32 : public FArchiveObjectCrc32
{
public:
	virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
	{
		if (InProperty == nullptr)
		{
			return false;
		}
		static const FName ExcludeFromHashName(TEXT("ExcludeFromHash"));
		return FArchiveObjectCrc32::ShouldSkipProperty(InProperty)
			|| InProperty->HasAllPropertyFlags(CPF_Transient)
			|| InProperty->HasMetaData(ExcludeFromHashName);
	}
};
}
#endif

// UStateTreeEditingSubsystem arrived in 5.5. On 5.4, compile through the
// public FStateTreeCompiler and maintain LastCompiledEditorDataHash manually,
// the same sequence FStateTreeEditor::Compile uses.
inline bool CompileStateTree(UStateTree* StateTree, FStateTreeCompilerLog& Log)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	if (StateTree == nullptr)
	{
		return false;
	}
	uint32 EditorDataHash = 0;
	if (StateTree->EditorData != nullptr)
	{
		Private::FCortexStateTreeObjectCRC32 Archive;
		EditorDataHash = Archive.Crc32(StateTree->EditorData, 0);
	}
	FStateTreeCompiler Compiler(Log);
	const bool bCompiled = Compiler.Compile(*StateTree);
	StateTree->LastCompiledEditorDataHash = bCompiled ? EditorDataHash : 0;
	return bCompiled;
#else
	return UStateTreeEditingSubsystem::CompileStateTree(StateTree, Log);
#endif
}

// FStateTreeCompilerLog::ToTokenizedMessages() arrived in 5.5. On 5.4 the
// message list is protected; route it through a transient MessageLog listing.
inline TArray<TSharedRef<FTokenizedMessage>> GetCompilerLogTokenizedMessages(const FStateTreeCompilerLog& Log)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	static const FName ListingName(TEXT("CortexStateTreeCompilerLog"));
	const TSharedRef<IMessageLogListing> Listing = MessageLogModule.IsRegisteredLogListing(ListingName)
		? MessageLogModule.GetLogListing(ListingName)
		: MessageLogModule.CreateLogListing(ListingName);
	Listing->ClearMessages();
	Log.AppendToLog(&Listing.Get());
	return Listing->GetFilteredMessages();
#else
	return Log.ToTokenizedMessages();
#endif
}

inline void ValidateStateTree(UStateTree* StateTree)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	// No public 5.4 equivalent of UStateTreeEditingSubsystem::ValidateStateTree;
	// the validation payload is built independently by the op, so this is a
	// no-op on 5.4.
	(void)StateTree;
#else
	UStateTreeEditingSubsystem::ValidateStateTree(StateTree);
#endif
}

// 5.4 only has AddTransition(Trigger, EventTag, Type, State); the 3-arg
// overload without the event tag arrived in 5.5.
inline FStateTreeTransition& AddTransition(
	UStateTreeState& State,
	const EStateTreeTransitionTrigger Trigger,
	const EStateTreeTransitionType Type,
	const UStateTreeState* TargetState,
	const FGameplayTag& EventTag = FGameplayTag())
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	return State.AddTransition(Trigger, EventTag, Type, TargetState);
#else
	FStateTreeTransition& Transition = State.AddTransition(Trigger, Type, TargetState);
	Transition.RequiredEvent.Tag = EventTag;
	return Transition;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Node-binding shims. Binding-aware StateTree code needs the support types that moved into PropertyBindingUtils in UE 5.7
// (FPropertyBindingPath, FStateTreePropertyPathBinding::IsOutputBinding, FStateTreeEditorNode::GetNodeID/ReallocInstanceData).
// Older engines compile to the same API with bNodeBindingsAvailable == false; every caller refuses with NOT_AVAILABLE first.
// ---------------------------------------------------------------------------------------------------------------------
inline constexpr bool bNodeBindingsAvailable = !UE_VERSION_OLDER_THAN(5, 7, 0);

struct FBindingSegmentInfo
{
	FName Name;
	FName DisplayName;
	int32 ArrayIndex = INDEX_NONE;
	/** Null for native types; refilled from the resolved owner type, never read from the transient stored GUID. */
	FGuid PropertyGuid;
	FString InstanceStructPath;
};

struct FBindingSideInfo
{
	FGuid StructID;
	TArray<FBindingSegmentInfo> Segments;
	FString Path;
	/** False when the struct the path is relative to is not part of the tree (such a target binding is removed by validation). */
	bool bStructFound = false;
	bool bResolved = false;
	FString ResolveError;
};

struct FBindingInfo
{
	int32 Index = INDEX_NONE;
	FBindingSideInfo Source;
	FBindingSideInfo Target;
	bool bOutput = false;
	bool bFunction = false;
	bool bCompletion = false;
	FString CompletionCondition;
};

enum class EBindingSide : uint8
{
	Source,
	Target,
};

/** What a bindable struct ID names in the editor data (GetBindableStructByID). */
struct FBindableStructInfo
{
	bool bFound = false;
	FName Name;
	FString StatePath;
	/** Engine enum name of the descriptor's data source: Task, Condition, Evaluator, GlobalTask, Consideration, Parameter, StateParameter, Context, PropertyFunction, Transition, TransitionEvent, StateEvent. */
	FString DataSource;
};

enum class EPropertyUsage : uint8
{
	Invalid,
	Input,
	Output,
	Context,
	Parameter,
};

/** Plain copy of everything the editor bindings store (restoring it replaces the collection, order included). */
struct FEditorBindingsJournal
{
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	TOptional<FStateTreeEditorPropertyBindings> Bindings;
#endif
	int32 Count = 0;
};

inline int32 GetEditorBindingCount(const UStateTreeEditorData& EditorData)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	return 0;
#else
	return EditorData.EditorBindings.GetBindings().Num();
#endif
}

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace Private
{
inline void FillBindingSide(const UStateTreeEditorData& EditorData, const FPropertyBindingPath& Path, FBindingSideInfo& Out)
{
	Out = FBindingSideInfo();
	Out.StructID = Path.GetStructID();
	// Property names, not Path.ToString(): ToString prefers the display names ("Result Out"), which are not what a caller passes back as a property name.
	for (const FPropertyBindingPathSegment& Segment : Path.GetSegments())
	{
		if (!Out.Path.IsEmpty())
		{
			Out.Path += TEXT('.');
		}
		Out.Path += Segment.GetName().ToString();
		if (Segment.GetArrayIndex() != INDEX_NONE)
		{
			Out.Path += FString::Printf(TEXT("[%d]"), Segment.GetArrayIndex());
		}
	}
	for (const FPropertyBindingPathSegment& Segment : Path.GetSegments())
	{
		FBindingSegmentInfo& Info = Out.Segments.AddDefaulted_GetRef();
		Info.Name = Segment.GetName();
		Info.DisplayName = Segment.GetDisplayName();
		Info.ArrayIndex = Segment.GetArrayIndex();
		Info.InstanceStructPath = Segment.GetInstanceStruct() != nullptr ? Segment.GetInstanceStruct()->GetPathName() : FString();
	}

	FPropertyBindingDataView View;
	if (!EditorData.GetBindingDataViewByID(Path.GetStructID(), View))
	{
		Out.ResolveError = TEXT("The struct the path is relative to is not part of this StateTree");
		return;
	}
	Out.bStructFound = true;
	if (Path.IsPathEmpty())
	{
		Out.bResolved = true;
		return;
	}

	// Non-mutating: ResolveIndirectionsWithValue is const and fills the GUID only into the indirections, which is the only place a
	// Blueprint-class segment learns its property GUID (PropertyBindingPath.cpp: "Update PropertyGuid").
	TArray<FPropertyBindingPathIndirection> Indirections;
	constexpr bool bHandleRedirects = true;
	constexpr bool bResetIndirectionsIfFailed = true;
	Out.bResolved = Path.ResolveIndirectionsWithValue(View, Indirections, &Out.ResolveError, bHandleRedirects, bResetIndirectionsIfFailed);
	if (Out.bResolved)
	{
		for (const FPropertyBindingPathIndirection& Indirection : Indirections)
		{
			const int32 SegmentIndex = Indirection.GetPathSegmentIndex();
			if (Out.Segments.IsValidIndex(SegmentIndex) && !Out.Segments[SegmentIndex].PropertyGuid.IsValid())
			{
				Out.Segments[SegmentIndex].PropertyGuid = Indirection.GetPropertyGuid();
			}
		}
	}
}

inline FString LexCompletionCondition(const UE::StateTree::ETaskCompletionCondition Condition)
{
	switch (Condition)
	{
	case UE::StateTree::ETaskCompletionCondition::Succeeds: return TEXT("Succeeds");
	case UE::StateTree::ETaskCompletionCondition::Fails: return TEXT("Fails");
	case UE::StateTree::ETaskCompletionCondition::Completes: return TEXT("Completes");
	default: return TEXT("Unknown");
	}
}

inline FString LexBindableStructSource(const EStateTreeBindableStructSource Source)
{
	switch (Source)
	{
	case EStateTreeBindableStructSource::Context: return TEXT("Context");
	case EStateTreeBindableStructSource::Parameter: return TEXT("Parameter");
	case EStateTreeBindableStructSource::Evaluator: return TEXT("Evaluator");
	case EStateTreeBindableStructSource::GlobalTask: return TEXT("GlobalTask");
	case EStateTreeBindableStructSource::StateParameter: return TEXT("StateParameter");
	case EStateTreeBindableStructSource::Task: return TEXT("Task");
	case EStateTreeBindableStructSource::Condition: return TEXT("Condition");
	case EStateTreeBindableStructSource::Consideration: return TEXT("Consideration");
	case EStateTreeBindableStructSource::TransitionEvent: return TEXT("TransitionEvent");
	case EStateTreeBindableStructSource::StateEvent: return TEXT("StateEvent");
	case EStateTreeBindableStructSource::PropertyFunction: return TEXT("PropertyFunction");
	case EStateTreeBindableStructSource::Transition: return TEXT("Transition");
	default: return TEXT("Unknown");
	}
}
}
#endif

/** One stored editor binding with both sides resolved (redirects handled) and its segment GUIDs. False when BindingIndex is out of range or the engine is older than 5.7. */
inline bool ReadEditorBinding(const UStateTreeEditorData& EditorData, const int32 BindingIndex, FBindingInfo& Out)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)BindingIndex;
	(void)Out;
	return false;
#else
	const TConstArrayView<FStateTreePropertyPathBinding> Bindings = EditorData.EditorBindings.GetBindings();
	if (!Bindings.IsValidIndex(BindingIndex))
	{
		return false;
	}
	const FStateTreePropertyPathBinding& Binding = Bindings[BindingIndex];
	Out = FBindingInfo();
	Out.Index = BindingIndex;
	Out.bOutput = Binding.IsOutputBinding();
	Out.bFunction = Binding.GetPropertyFunctionNode().IsValid();
	if (const TOptional<UE::StateTree::ETaskCompletionCondition> Completion = Binding.GetCompletionBinding(); Completion.IsSet())
	{
		Out.bCompletion = true;
		Out.CompletionCondition = Private::LexCompletionCondition(Completion.GetValue());
	}
	Private::FillBindingSide(EditorData, Binding.GetSourcePath(), Out.Source);
	Private::FillBindingSide(EditorData, Binding.GetTargetPath(), Out.Target);
	return true;
#endif
}

inline FBindableStructInfo DescribeBindableStruct(const UStateTreeEditorData& EditorData, const FGuid& StructID)
{
	FBindableStructInfo Info;
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)StructID;
#else
	TInstancedStruct<FPropertyBindingBindableStructDescriptor> Descriptor;
	if (StructID.IsValid() && EditorData.GetBindableStructByID(StructID, Descriptor) && Descriptor.IsValid())
	{
		if (const FStateTreeBindableStructDesc* Desc = Descriptor.GetPtr<FStateTreeBindableStructDesc>())
		{
			Info.bFound = true;
			Info.Name = Desc->Name;
			Info.StatePath = Desc->StatePath;
			Info.DataSource = Private::LexBindableStructSource(Desc->DataSource);
		}
	}
#endif
	return Info;
}

/** The schema context struct an unbound Context property would be bound to by the compiler (StateTreeCompiler.cpp:3162-3173). */
inline FBindableStructInfo FindContextStructFor(const UStateTreeEditorData& EditorData, const FProperty* Property, FGuid& OutStructID)
{
	FBindableStructInfo Info;
	OutStructID.Invalidate();
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)Property;
#else
	const UStruct* ContextObjectType = nullptr;
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		ContextObjectType = StructProperty->Struct;
	}
	else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		ContextObjectType = ObjectProperty->PropertyClass;
	}
	if (ContextObjectType != nullptr)
	{
		const FStateTreeBindableStructDesc Desc = EditorData.FindContextData(ContextObjectType, Property->GetName());
		if (Desc.IsValid())
		{
			Info.bFound = true;
			Info.Name = Desc.Name;
			Info.StatePath = Desc.StatePath;
			Info.DataSource = Private::LexBindableStructSource(Desc.DataSource);
			OutStructID = Desc.ID;
		}
	}
#endif
	return Info;
}

inline EPropertyUsage GetPropertyUsage(const FProperty* Property)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)Property;
	return EPropertyUsage::Invalid;
#else
	switch (UE::StateTree::GetUsageFromMetaData(Property))
	{
	case EStateTreePropertyUsage::Input: return EPropertyUsage::Input;
	case EStateTreePropertyUsage::Output: return EPropertyUsage::Output;
	case EStateTreePropertyUsage::Context: return EPropertyUsage::Context;
	case EStateTreePropertyUsage::Parameter: return EPropertyUsage::Parameter;
	default: return EPropertyUsage::Invalid;
	}
#endif
}

/** Does the stored path (one side of one binding) still resolve when its first segment is renamed and the owner is a different type? Nested segments are never rewritten. */
inline bool ResolveRenamedBindingPath(const UStateTreeEditorData& EditorData, const int32 BindingIndex, const EBindingSide Side, const FName NewFirstSegment, const UStruct* NewBase, FString& OutError)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)BindingIndex;
	(void)Side;
	(void)NewFirstSegment;
	(void)NewBase;
	OutError = TEXT("Binding paths cannot be inspected before UE 5.7");
	return false;
#else
	const TConstArrayView<FStateTreePropertyPathBinding> Bindings = EditorData.EditorBindings.GetBindings();
	if (!Bindings.IsValidIndex(BindingIndex) || NewBase == nullptr)
	{
		OutError = TEXT("Binding or owner type is not available");
		return false;
	}
	FPropertyBindingPath Copy = Side == EBindingSide::Target ? Bindings[BindingIndex].GetTargetPath() : Bindings[BindingIndex].GetSourcePath();
	TArrayView<FPropertyBindingPathSegment> Segments = Copy.GetMutableSegments();
	if (Segments.IsEmpty())
	{
		return true;
	}
	Segments[0].SetName(NewFirstSegment);
	Segments[0].SetDisplayName(NewFirstSegment);
	Segments[0].SetPropertyGuid(FGuid());
	TArray<FPropertyBindingPathIndirection> Indirections;
	return Copy.ResolveIndirections(NewBase, Indirections, &OutError, /*bHandleRedirects=*/false, /*bResetIndirectionsIfFailed=*/true);
#endif
}

/** Rewrites the first segment of one side of one stored binding: name, display name (path resolution falls back to it, PropertyBindingPath.cpp:386-392) and the stale property GUID. */
inline bool RemapBindingSegments(UStateTreeEditorData& EditorData, const int32 BindingIndex, const EBindingSide Side, const FName NewFirstSegment)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)BindingIndex;
	(void)Side;
	(void)NewFirstSegment;
	return false;
#else
	const TArrayView<FStateTreePropertyPathBinding> Bindings = EditorData.EditorBindings.GetMutableBindings();
	if (!Bindings.IsValidIndex(BindingIndex))
	{
		return false;
	}
	FPropertyBindingPath& Path = Side == EBindingSide::Target ? Bindings[BindingIndex].GetMutableTargetPath() : Bindings[BindingIndex].GetMutableSourcePath();
	const TArrayView<FPropertyBindingPathSegment> Segments = Path.GetMutableSegments();
	if (Segments.IsEmpty())
	{
		return false;
	}
	Segments[0].SetName(NewFirstSegment);
	Segments[0].SetDisplayName(NewFirstSegment);
	Segments[0].SetPropertyGuid(FGuid());
	return true;
#endif
}

inline void CaptureEditorBindings(const UStateTreeEditorData& EditorData, FEditorBindingsJournal& OutJournal)
{
	OutJournal.Count = GetEditorBindingCount(EditorData);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	OutJournal.Bindings = EditorData.EditorBindings;
#endif
}

inline void RestoreEditorBindings(UStateTreeEditorData& EditorData, const FEditorBindingsJournal& Journal)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)EditorData;
	(void)Journal;
#else
	if (Journal.Bindings.IsSet())
	{
		EditorData.EditorBindings = Journal.Bindings.GetValue();
	}
#endif
}

inline FGuid GetNodeDefinitionId(const FStateTreeEditorNode& Node)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)Node;
	return FGuid();
#else
	return Node.GetNodeID();
#endif
}

/** Node.Node.InitializeAs on the FInstancedStruct member: unlike FStateTreeEditorNode::InitializeAs it keeps the editor node ID, which the bindings are keyed by. */
inline void InitializeNodeAs(FStateTreeEditorNode& Node, const UScriptStruct* NewNodeStruct)
{
	Node.Node.InitializeAs(NewNodeStruct);
}

/** Resets and reallocates the instance and execution runtime data for the current node type, preserving ID, indent and operand (StateTreeEditorNode.cpp:33-41). */
inline void ReallocInstanceDataKeepingId(FStateTreeEditorNode& Node, UObject* Outer)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	(void)Node;
	(void)Outer;
#else
	if (Outer != nullptr)
	{
		Node.ReallocInstanceData(Outer);
	}
#endif
}

/** UStateTreeState::Considerations does not exist in 5.4. */
inline TArray<FStateTreeEditorNode>* GetConsiderations(UStateTreeState& State)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	(void)State;
	return nullptr;
#else
	return &State.Considerations;
#endif
}
}
