#include "Operations/CortexLevelInstanceExportOps.h"

#include "CortexCommandRouter.h"
#include "CortexLevelUtils.h"
#include "CortexSafeFileContract.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace CortexLevelInstanceExportPrivate
{
	/** Editor bookkeeping on AActor that differs per instance without being a placed override. */
	bool IsEditorVolatileProperty(const FProperty* Property)
	{
		static const TSet<FName> Volatile = {
			TEXT("ActorLabel"), TEXT("FolderPath"), TEXT("ActorGuid"), TEXT("ActorInstanceGuid"), TEXT("ContentBundleGuid")};
		return Volatile.Contains(Property->GetFName());
	}

	/** Editable on an instance, persisted, not editor bookkeeping and not read-only (VisibleAnywhere component refs, GUIDs). */
	bool IsReportableProperty(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(
				CPF_EditConst | CPF_DisableEditOnInstance | CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
			&& !IsEditorVolatileProperty(Property);
	}

	TSharedPtr<FJsonValue> ValueOrNull(const TSharedPtr<FJsonValue>& Value)
	{
		return Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
	}

	/** One reported property of one instance; returns null when changed_only finds it equal to the archetype. */
	TSharedPtr<FJsonObject> BuildPropertyEntry(
		const FProperty* Property, const FString& Key, const void* ValuePtr, const void* BaselinePtr, const bool bChangedOnly)
	{
		FCortexSerializationPolicy Policy;
		Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;

		TArray<FCortexSerializationIssue> Issues;
		TSharedPtr<FJsonValue> Value;
		bool bComplete = true;

		if (bChangedOnly && BaselinePtr && Property->Identical(ValuePtr, BaselinePtr, PPF_DeepCompareInstances))
		{
			return nullptr; // equal to the archetype, including instanced subobjects compared by content
		}
		if (bChangedOnly && BaselinePtr)
		{
			const FCortexPropertySerializationResult Difference =
				FCortexSerializer::PropertyDifferenceToJson(Property, ValuePtr, BaselinePtr, Policy, Key);
			Issues.Append(Difference.Issues);
			bComplete = Difference.Issues.Num() == 0 && !Difference.bPartial;
			if (bComplete && !Difference.JsonValue.IsValid())
			{
				return nullptr; // proven equal to the archetype
			}
			Value = Difference.JsonValue;
		}
		else
		{
			const FCortexPropertySerializationResult Full = FCortexSerializer::PropertyToJsonDeep(Property, ValuePtr, Policy, Key);
			Issues.Append(Full.Issues);
			bComplete = Full.Issues.Num() == 0 && !Full.bPartial;
			Value = Full.JsonValue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("property"), Key);
		Entry->SetField(TEXT("value"), ValueOrNull(Value));
		if (BaselinePtr)
		{
			const FCortexPropertySerializationResult Baseline = FCortexSerializer::PropertyToJsonDeep(Property, BaselinePtr, Policy, Key);
			Issues.Append(Baseline.Issues);
			bComplete = bComplete && Baseline.Issues.Num() == 0 && !Baseline.bPartial;
			Entry->SetField(TEXT("archetype_value"), ValueOrNull(Baseline.JsonValue));
		}
		else
		{
			// No comparable archetype: the value is the effective value and the entry says so.
			Entry->SetField(TEXT("archetype_value"), MakeShared<FJsonValueNull>());
			Entry->SetBoolField(TEXT("no_archetype"), true);
		}
		if (!bComplete)
		{
			// An incomplete comparison is never presented as equality or as a complete value.
			Entry->SetBoolField(TEXT("incomplete"), true);
			Entry->SetArrayField(TEXT("issues"), FCortexSerializer::SerializationIssuesToJson(Issues));
		}
		return Entry;
	}

	/**
	 * Reports the reportable properties of Object against its archetype. An object property whose value is a subobject
	 * (component) owned by the actor is never compared as a reference: its own editable properties are diffed against
	 * that component's archetype (the SCS template or inherited override) one level down, as `Name.Property`.
	 */
	void CollectProperties(const AActor* Owner, const UObject* Object, const UObject* Archetype, const FString& Prefix,
		const bool bChangedOnly, const bool bRecurseOwned, TSet<const UObject*>& Visited,
		TArray<TSharedPtr<FJsonValue>>& OutProperties, int32& OutPropertyCount)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;

			// A component the actor owns is diffed through its own properties, whatever flags its variable carries
			// (Blueprint component variables are not necessarily editable); each component is walked once, under the
			// first variable that names it.
			if (bRecurseOwned)
			{
				const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
				if (ObjectProperty && !Property->HasAnyPropertyFlags(CPF_Deprecated))
				{
					bool bOwnedReference = false;
					for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
					{
						const UObject* Owned = ObjectProperty->GetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Object, Index));
						if (Owned && Owned->GetOuter() == Owner)
						{
							bOwnedReference = true;
							bool bAlreadyWalked = false;
							Visited.Add(Owned, &bAlreadyWalked);
							if (!bAlreadyWalked)
							{
								const FString Name = Property->ArrayDim > 1
									? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index)
									: Property->GetName();
								CollectProperties(Owner, Owned, Owned->GetArchetype(), Prefix + Name + TEXT("."), bChangedOnly,
									/*bRecurseOwned=*/false, Visited, OutProperties, OutPropertyCount);
							}
						}
					}
					if (bOwnedReference)
					{
						continue;
					}
				}
			}

			if (!IsReportableProperty(Property))
			{
				continue;
			}

			// A baseline is only read from an object that owns the property compatibly.
			const UClass* OwnerClass = Cast<UClass>(Property->GetOwnerStruct());
			const bool bHasBaseline = Archetype && OwnerClass && Archetype->GetClass()->IsChildOf(OwnerClass);

			for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
			{
				const FString Name = Property->ArrayDim > 1
					? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index)
					: Property->GetName();
				const FString Key = Prefix + Name;
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object, Index);
				const void* BaselinePtr = bHasBaseline ? Property->ContainerPtrToValuePtr<void>(Archetype, Index) : nullptr;
				if (const TSharedPtr<FJsonObject> Entry = BuildPropertyEntry(Property, Key, ValuePtr, BaselinePtr, bChangedOnly))
				{
					OutProperties.Add(MakeShared<FJsonValueObject>(Entry));
					++OutPropertyCount;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BuildInstance(AActor* Actor, const bool bChangedOnly, int32& OutPropertyCount)
	{
		TSharedPtr<FJsonObject> Instance = MakeShared<FJsonObject>();
		Instance->SetStringField(TEXT("actor_name"), Actor->GetName());
		Instance->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		Instance->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());

		TArray<TSharedPtr<FJsonValue>> Properties;
		TSet<const UObject*> Visited;
		CollectProperties(Actor, Actor, Actor->GetArchetype(), FString(), bChangedOnly, /*bRecurseOwned=*/true, Visited, Properties, OutPropertyCount);
		Properties.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("property")).Compare(B->AsObject()->GetStringField(TEXT("property")), ESearchCase::CaseSensitive) < 0;
		});
		Instance->SetArrayField(TEXT("properties"), Properties);
		return Instance;
	}

	/** Packages this call loaded; released when the call ends so no stale in-memory copy of a map outlives it. */
	struct FLoadedMapScope
	{
		TArray<UPackage*> Loaded;

		~FLoadedMapScope()
		{
			if (Loaded.IsEmpty())
			{
				return;
			}
			for (UPackage* Package : Loaded)
			{
				if (!IsValid(Package))
				{
					continue;
				}
				if (UWorld* World = UWorld::FindWorldInPackage(Package))
				{
					// The editor initializes every world it loads as an inactive world (components registered,
					// subsystems up); tear that down the way the world was created before dropping it.
					if (World->IsInitialized())
					{
						World->DestroyWorld(/*bInformEngineOfWorld=*/false);
					}
					World->ClearFlags(RF_Standalone | RF_Public);
					World->MarkAsGarbage();
				}
				Package->ClearFlags(RF_Standalone);
				Package->MarkAsGarbage();
				ResetLoaders(Package);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	};
}

FCortexCommandResult FCortexLevelInstanceExportOps::ExportClassInstances(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexLevelInstanceExportPrivate;

	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class"), ClassName) || ClassName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: class"));
	}

	const TArray<TSharedPtr<FJsonValue>>* MapValues = nullptr;
	if (!Params->TryGetArrayField(TEXT("maps"), MapValues) || MapValues->Num() == 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("maps must be a non-empty array of map package paths"));
	}
	TArray<FString> MapPackages;
	for (const TSharedPtr<FJsonValue>& Value : *MapValues)
	{
		FString Map;
		if (!Value.IsValid() || !Value->TryGetString(Map) || Map.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("maps must be an array of non-empty map package paths"));
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(Map);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Not a valid map package path: %s"), *Map));
		}
		MapPackages.AddUnique(PackageName);
	}

	bool bChangedOnly = true;
	if (Params->HasField(TEXT("changed_only")) && !Params->TryGetBoolField(TEXT("changed_only"), bChangedOnly))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("changed_only must be a boolean"));
	}

	FString OutPath;
	if (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: out_path"));
	}
	// An unusable out_path is refused before any map is read.
	FCortexResolvedFilePath ResolvedOutPath;
	FString PathErrorCode;
	FString PathErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(OutPath, ResolvedOutPath, PathErrorCode, PathErrorMessage))
	{
		return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
	}

	FCortexCommandResult ClassError;
	UClass* ActorClass = FCortexLevelUtils::ResolveActorClass(ClassName, ClassError);
	if (!ActorClass)
	{
		return ClassError;
	}

	for (const FString& PackageName : MapPackages)
	{
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound,
				FString::Printf(TEXT("Map not found: %s"), *PackageName));
		}
	}

	FLoadedMapScope LoadedMaps;
	TArray<TSharedPtr<FJsonValue>> MapEntries;
	int32 TotalInstances = 0;
	int32 TotalProperties = 0;

	for (const FString& PackageName : MapPackages)
	{
		const bool bAlreadyResident = FindPackage(nullptr, *PackageName) != nullptr;
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		if (Package && !bAlreadyResident)
		{
			LoadedMaps.Loaded.Add(Package);
		}
		UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
		if (!World || !World->PersistentLevel)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Package is not a map: %s"), *PackageName));
		}

		ULevel* Level = World->PersistentLevel;
		if (World->IsPartitionedWorld())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation,
				FString::Printf(TEXT("%s is a World Partition map: its actors live in external actor packages and cannot be read without opening the map"), *PackageName));
		}
		// A non-partitioned level's external actors are loaded into Level->Actors by the engine when the level loads,
		// so they are read like any other actor; only World Partition maps (refused above) keep actors unloaded.
		// For a map this call loaded, a package-count mismatch means the engine did not load every external actor
		// package found on disk, so the export would be incomplete. A map that was already resident (for example the
		// open editor world) legitimately differs from disk through unsaved edits and is read as it is.
		if (!bAlreadyResident && Level->IsUsingExternalActors())
		{
			FString ExternalDirectory;
			TArray<FString> ExternalFiles;
			if (FPackageName::TryConvertLongPackageNameToFilename(ULevel::GetExternalActorsPath(PackageName), ExternalDirectory))
			{
				IFileManager::Get().FindFilesRecursive(ExternalFiles, *ExternalDirectory, TEXT("*.uasset"), true, false);
			}
			TSet<const UPackage*> LoadedExternalPackages;
			for (const AActor* Actor : Level->Actors)
			{
				if (Actor && Actor->IsPackageExternal())
				{
					LoadedExternalPackages.Add(Actor->GetExternalPackage());
				}
			}
			if (ExternalFiles.Num() > LoadedExternalPackages.Num())
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation,
					FString::Printf(TEXT("%s has %d external actor package(s) on disk but only %d were loaded with the map; the export would be incomplete"),
						*PackageName, ExternalFiles.Num(), LoadedExternalPackages.Num()));
			}
		}

		TArray<AActor*> Matches;
		for (AActor* Actor : Level->Actors)
		{
			if (IsValid(Actor) && Actor->IsA(ActorClass))
			{
				Matches.Add(Actor);
			}
		}
		Matches.Sort([](const AActor& A, const AActor& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> InstanceValues;
		for (AActor* Actor : Matches)
		{
			InstanceValues.Add(MakeShared<FJsonValueObject>(BuildInstance(Actor, bChangedOnly, TotalProperties)));
		}
		TotalInstances += InstanceValues.Num();

		TSharedPtr<FJsonObject> MapEntry = MakeShared<FJsonObject>();
		MapEntry->SetStringField(TEXT("map"), PackageName);
		MapEntry->SetArrayField(TEXT("instances"), InstanceValues);
		MapEntries.Add(MakeShared<FJsonValueObject>(MapEntry));
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("class"), ActorClass->GetPathName());
	Report->SetBoolField(TEXT("changed_only"), bChangedOnly);
	Report->SetArrayField(TEXT("maps"), MapEntries);

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Report);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode, WriteResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("maps"), MapPackages.Num());
	Totals->SetNumberField(TEXT("instances"), TotalInstances);
	Totals->SetNumberField(TEXT("properties"), TotalProperties);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("out_path"), ResolvedOutPath.AbsolutePath);
	Data->SetObjectField(TEXT("totals"), Totals);
	return FCortexCommandRouter::Success(Data);
}
