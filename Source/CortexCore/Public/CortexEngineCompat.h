#pragma once

#include "CortexTypes.h"
#include "Delegates/Delegate.h"
#include "Dom/JsonObject.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/EngineVersionComparison.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/WeakObjectPtr.h"

class UGameInstance;
class ULevelEditorPlaySettings;
class UWorld;

/** Mirrors the engine's scoped phase so consumers never include the engine header. */
enum class ECortexScopedPIEPhase : uint8
{
	Pending,
	Worldless,
	Live,
	Quiescent,
	Ending,
	Ended,
	Cancelled
};

/** Engine-issued request/context identity plus weak live-object views; never roots retired objects. */
struct FCortexScopedPIESnapshot
{
	uint64 RequestSerial = 0;
	uint64 ContextIncarnation = 0;
	uint64 TopologyRevision = 0;
	FName ContextHandle = NAME_None;
	ECortexScopedPIEPhase Phase = ECortexScopedPIEPhase::Pending;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGameInstance> GameInstance;
	TWeakObjectPtr<ULevelEditorPlaySettings> Settings;
	bool bHasForeignPlayWork = false;
	bool bScopedEndQueued = false;
};

namespace CortexEngineCompat
{
	/**
	 * Subscribe to ownership-scoped PIE lifecycle transitions. Only an engine that provides the
	 * scoped PIE capability emits them; a stock engine returns an invalid handle.
	 */
	CORTEXCORE_API FDelegateHandle ObserveScopedPIELifecycle(
		TFunction<void(const FCortexScopedPIESnapshot&)>&& Callback);

	CORTEXCORE_API void RemoveScopedPIELifecycleObserver(FDelegateHandle Handle);

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	inline FString JsonKeyToString(const FString& Key)
	{
		return Key;
	}
#else
	inline FString JsonKeyToString(const FJsonObject::FStringType& Key)
	{
		return FString(Key.ToView());
	}
#endif

	inline void SetStringTableSourceString(
		FStringTable& StringTable,
		const FTextKey& Key,
		const FString& SourceString)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		StringTable.SetSourceString(Key, SourceString);
#else
		StringTable.SetSourceString(Key, SourceString, FString());
		if (UStringTable* OwnerAsset = StringTable.GetOwnerAsset())
		{
			const FName TableId = OwnerAsset->GetStringTableId();
			if (!FStringTableRegistry::Get().FindStringTable(TableId).IsValid())
			{
				FStringTableRegistry::Get().RegisterStringTable(TableId, StringTable.AsShared());
			}
		}
#endif
	}

	inline void ForEachObjectWithPackage(
		const UPackage* Outer,
		TFunctionRef<bool(UObject*)> Operation,
		bool bIncludeNestedObjects = true)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		::ForEachObjectWithPackage(Outer, Operation, bIncludeNestedObjects);
#else
		::ForEachObjectWithPackage(Outer, Operation, bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#endif
	}
}
