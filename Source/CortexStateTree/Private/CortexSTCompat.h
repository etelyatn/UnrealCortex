#pragma once

#include "Misc/EngineVersionComparison.h"
#include "GameplayTagContainer.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeState.h"
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
}
