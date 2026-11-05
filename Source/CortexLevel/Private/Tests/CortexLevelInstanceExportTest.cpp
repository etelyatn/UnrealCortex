// level.export_class_instances reads every placed instance of an actor class (subclasses included) from
// saved map packages WITHOUT opening them: the editor world, its dirty state and the map packages stay
// untouched. Per instance it reports the editable properties that differ from the class default
// (changed_only, default true) or all editable properties.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexLevelCommandHandler.h"
#include "CortexLevelInstanceExportTestActors.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	FString NewSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	}

	FString MapFilename(const FString& PackageName)
	{
		return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	}

	/** A saved map, written from a throwaway in-memory world that is fully released afterwards. */
	struct FSavedMapFixture
	{
		FString PackageName;
		FString Filename;
		bool bUseExternalActors = false;
		TArray<FString> ExtraFiles;

		/** Spawn callback fills the world; the world is then saved and released from memory. */
		bool Create(const FString& InPackageName, TFunctionRef<void(UWorld*)> Populate, bool bExternal = false)
		{
			PackageName = InPackageName;
			Filename = MapFilename(PackageName);
			bUseExternalActors = bExternal;

			UPackage* Package = CreatePackage(*PackageName);
			UWorld* World = Package
				? UWorld::CreateWorld(EWorldType::Inactive, false, FName(*FPackageName::GetShortName(PackageName)), Package)
				: nullptr;
			if (!World)
			{
				return false;
			}
			World->SetFlags(RF_Public | RF_Standalone);
			if (bExternal && World->PersistentLevel)
			{
				World->PersistentLevel->SetUseExternalActors(true);
			}
			Populate(World);

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			bool bSaved = UPackage::SavePackage(Package, World, *Filename, SaveArgs);
			if (bSaved && bExternal && World->PersistentLevel)
			{
				// External actors live in their own packages; save each one.
				for (AActor* Actor : World->PersistentLevel->Actors)
				{
					UPackage* ExternalPackage = Actor ? Actor->GetExternalPackage() : nullptr;
					if (!ExternalPackage)
					{
						continue;
					}
					const FString ExternalFile = FPackageName::LongPackageNameToFilename(
						ExternalPackage->GetName(), FPackageName::GetAssetPackageExtension());
					ExtraFiles.Add(ExternalFile);
					FSavePackageArgs ExternalArgs;
					ExternalArgs.TopLevelFlags = RF_Standalone;
					bSaved &= UPackage::SavePackage(ExternalPackage, nullptr, *ExternalFile, ExternalArgs);
				}
			}

			// Release everything so the command has to load the map from disk.
			World->DestroyWorld(false);
			World->ClearFlags(RF_Standalone | RF_Public);
			World->MarkAsGarbage();
			Package->ClearFlags(RF_Standalone);
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
			ResetLoaders(Package);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			return bSaved && FindPackage(nullptr, *PackageName) == nullptr;
		}

		void Cleanup() const
		{
			IFileManager::Get().Delete(*Filename, false, true);
			for (const FString& Extra : ExtraFiles)
			{
				IFileManager::Get().Delete(*Extra, false, true);
			}
			const FString ExternalRoot = FPackageName::LongPackageNameToFilename(
				ULevel::GetExternalActorsPath(PackageName));
			IFileManager::Get().DeleteDirectory(*ExternalRoot, false, true);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Filename), false, true);
		}
	};

	AActor* Spawn(UWorld* World, UClass* Class, const TCHAR* Name, const TCHAR* Label)
	{
		FActorSpawnParameters Parameters;
		Parameters.Name = FName(Name);
		Parameters.OverrideLevel = World->PersistentLevel;
		AActor* Actor = World->SpawnActor<AActor>(Class, FTransform::Identity, Parameters);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	FCortexCommandResult RunExport(
		const FString& ClassName, const TArray<FString>& Maps, const FString& OutPath, const TOptional<bool>& ChangedOnly)
	{
		FCortexLevelCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class"), ClassName);
		TArray<TSharedPtr<FJsonValue>> MapValues;
		for (const FString& Map : Maps)
		{
			MapValues.Add(MakeShared<FJsonValueString>(Map));
		}
		Params->SetArrayField(TEXT("maps"), MapValues);
		Params->SetStringField(TEXT("out_path"), OutPath);
		if (ChangedOnly.IsSet())
		{
			Params->SetBoolField(TEXT("changed_only"), ChangedOnly.GetValue());
		}
		return Handler.Execute(TEXT("export_class_instances"), Params);
	}

	TSharedPtr<FJsonObject> FindMap(const TSharedPtr<FJsonObject>& Report, const FString& Map)
	{
		const TArray<TSharedPtr<FJsonValue>>* Maps = nullptr;
		if (Report.IsValid() && Report->TryGetArrayField(TEXT("maps"), Maps))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Maps)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry.IsValid() && Entry->GetStringField(TEXT("map")) == Map)
				{
					return Entry;
				}
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindInstance(const TSharedPtr<FJsonObject>& MapEntry, const FString& ActorName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
		if (MapEntry.IsValid() && MapEntry->TryGetArrayField(TEXT("instances"), Instances))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Instances)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry.IsValid() && Entry->GetStringField(TEXT("actor_name")) == ActorName)
				{
					return Entry;
				}
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindProperty(const TSharedPtr<FJsonObject>& Instance, const FString& Property)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Instance.IsValid() && Instance->TryGetArrayField(TEXT("properties"), Properties))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Properties)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry.IsValid() && Entry->GetStringField(TEXT("property")) == Property)
				{
					return Entry;
				}
			}
		}
		return nullptr;
	}

	int32 CountArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Field, Array) ? Array->Num() : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelExportClassInstancesTest,
	"Cortex.Level.ExportClassInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelExportClassInstancesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}

	const FString Suffix = NewSuffix();
	const FString Dir = FString::Printf(TEXT("/Game/Temp/InstanceExport_%s"), *Suffix);
	const FString MapAName = Dir + TEXT("/MapA");
	const FString MapBName = Dir + TEXT("/MapB");
	const FString OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("CortexInstanceExportTest"), FString::Printf(TEXT("instances_%s.json"), *Suffix)));

	FSavedMapFixture MapA;
	FSavedMapFixture MapB;
	ON_SCOPE_EXIT
	{
		MapA.Cleanup();
		MapB.Cleanup();
		IFileManager::Get().Delete(*OutPath, false, true);
	};

	const bool bCreatedA = MapA.Create(MapAName, [](UWorld* World)
	{
		AActor* Overridden = Spawn(World, ACortexLevelInstanceExportTestActor::StaticClass(), TEXT("StationOverridden"), TEXT("Overridden"));
		Cast<ACortexLevelInstanceExportTestActor>(Overridden)->Count = 7;
		Spawn(World, ACortexLevelInstanceExportTestActor::StaticClass(), TEXT("StationDefault"), TEXT("Default"));
		Spawn(World, AActor::StaticClass(), TEXT("UnrelatedActor"), TEXT("Unrelated"));
	});
	const bool bCreatedB = MapB.Create(MapBName, [](UWorld* World)
	{
		AActor* Child = Spawn(World, ACortexLevelInstanceExportTestChildActor::StaticClass(), TEXT("StationChild"), TEXT("Child"));
		Cast<ACortexLevelInstanceExportTestActor>(Child)->Note = TEXT("changed");
	});
	if (!TestTrue(TEXT("saved map fixtures created and released from memory"), bCreatedA && bCreatedB))
	{
		return false;
	}

	// The editor world is a third map; snapshot everything the export must not disturb.
	UWorld* ActiveWorld = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("an editor world is open"), ActiveWorld))
	{
		return false;
	}
	UPackage* ActivePackage = ActiveWorld->GetOutermost();
	const FString ActiveWorldPath = ActiveWorld->GetPathName();
	const bool bActiveDirtyBefore = ActivePackage->IsDirty();
	const FDateTime StampA = IFileManager::Get().GetTimeStamp(*MapA.Filename);
	const FDateTime StampB = IFileManager::Get().GetTimeStamp(*MapB.Filename);
	const int64 SizeA = IFileManager::Get().FileSize(*MapA.Filename);
	const int64 SizeB = IFileManager::Get().FileSize(*MapB.Filename);

	const FString ClassName = TEXT("CortexLevelInstanceExportTestActor");

	// changed_only defaults to true.
	const FCortexCommandResult Default = RunExport(ClassName, {MapAName, MapBName}, OutPath, TOptional<bool>());
	if (!TestTrue(TEXT("export succeeds"), Default.bSuccess))
	{
		return false;
	}
	TestFalse(TEXT("response does not echo instance bodies"), Default.Data->HasField(TEXT("maps")) || Default.Data->HasField(TEXT("instances")));
	const TSharedPtr<FJsonObject>* Totals = nullptr;
	if (TestTrue(TEXT("response carries totals"), Default.Data->TryGetObjectField(TEXT("totals"), Totals)))
	{
		TestEqual(TEXT("totals.maps"), static_cast<int32>((*Totals)->GetNumberField(TEXT("maps"))), 2);
		TestEqual(TEXT("totals.instances"), static_cast<int32>((*Totals)->GetNumberField(TEXT("instances"))), 3);
		TestEqual(TEXT("totals.properties counts reported overrides"), static_cast<int32>((*Totals)->GetNumberField(TEXT("properties"))), 2);
	}

	const TSharedPtr<FJsonObject> Report = ReadJson(OutPath);
	if (!TestNotNull(TEXT("report file parses"), Report.Get()))
	{
		return false;
	}
	TestEqual(TEXT("changed_only echoed"), Report->GetBoolField(TEXT("changed_only")), true);

	const TSharedPtr<FJsonObject> ReportA = FindMap(Report, MapAName);
	const TSharedPtr<FJsonObject> ReportB = FindMap(Report, MapBName);
	if (!TestTrue(TEXT("both maps reported"), ReportA.IsValid() && ReportB.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("MapA has the two placed instances and not the unrelated actor"), CountArray(ReportA, TEXT("instances")), 2);
	TestEqual(TEXT("MapB has the subclass instance"), CountArray(ReportB, TEXT("instances")), 1);

	const TSharedPtr<FJsonObject> Overridden = FindInstance(ReportA, TEXT("StationOverridden"));
	const TSharedPtr<FJsonObject> Untouched = FindInstance(ReportA, TEXT("StationDefault"));
	const TSharedPtr<FJsonObject> Child = FindInstance(ReportB, TEXT("StationChild"));
	if (!TestTrue(TEXT("instances found by actor name"), Overridden.IsValid() && Untouched.IsValid() && Child.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("actor label"), Overridden->GetStringField(TEXT("actor_label")), FString(TEXT("Overridden")));
	TestTrue(TEXT("class path names the native class"), Overridden->GetStringField(TEXT("class")).EndsWith(TEXT("CortexLevelInstanceExportTestActor")));
	TestTrue(TEXT("subclass instance reports its own class"), Child->GetStringField(TEXT("class")).EndsWith(TEXT("CortexLevelInstanceExportTestChildActor")));

	TestEqual(TEXT("overridden instance reports exactly one property"), CountArray(Overridden, TEXT("properties")), 1);
	const TSharedPtr<FJsonObject> CountProperty = FindProperty(Overridden, TEXT("Count"));
	if (TestTrue(TEXT("Count override reported"), CountProperty.IsValid()))
	{
		TestEqual(TEXT("Count value"), static_cast<int32>(CountProperty->GetNumberField(TEXT("value"))), 7);
		TestEqual(TEXT("Count archetype value"), static_cast<int32>(CountProperty->GetNumberField(TEXT("archetype_value"))), 1);
	}
	TestEqual(TEXT("untouched instance reports no property"), CountArray(Untouched, TEXT("properties")), 0);
	TestEqual(TEXT("subclass instance reports exactly one property"), CountArray(Child, TEXT("properties")), 1);
	const TSharedPtr<FJsonObject> NoteProperty = FindProperty(Child, TEXT("Note"));
	if (TestTrue(TEXT("Note override reported"), NoteProperty.IsValid()))
	{
		TestEqual(TEXT("Note value"), NoteProperty->GetStringField(TEXT("value")), FString(TEXT("changed")));
		TestEqual(TEXT("Note archetype value"), NoteProperty->GetStringField(TEXT("archetype_value")), FString(TEXT("base")));
	}

	// changed_only:false reports all editable properties, never transient or non-editable ones.
	const FCortexCommandResult All = RunExport(ClassName, {MapAName}, OutPath, TOptional<bool>(false));
	if (TestTrue(TEXT("changed_only:false export succeeds"), All.bSuccess))
	{
		const TSharedPtr<FJsonObject> AllReport = ReadJson(OutPath);
		const TSharedPtr<FJsonObject> AllUntouched = FindInstance(FindMap(AllReport, MapAName), TEXT("StationDefault"));
		if (TestTrue(TEXT("untouched instance present"), AllUntouched.IsValid()))
		{
			const TSharedPtr<FJsonObject> AllCount = FindProperty(AllUntouched, TEXT("Count"));
			const TSharedPtr<FJsonObject> AllNote = FindProperty(AllUntouched, TEXT("Note"));
			TestTrue(TEXT("Count and Note reported although equal to the archetype"), AllCount.IsValid() && AllNote.IsValid());
			if (AllCount.IsValid())
			{
				TestEqual(TEXT("equal Count value"), static_cast<int32>(AllCount->GetNumberField(TEXT("value"))), 1);
			}
			TestFalse(TEXT("transient property excluded"), FindProperty(AllUntouched, TEXT("Volatile")).IsValid());
			TestFalse(TEXT("non-editable property excluded"), FindProperty(AllUntouched, TEXT("Hidden")).IsValid());
			TestFalse(TEXT("EditConst component reference excluded"), FindProperty(AllUntouched, TEXT("Marker")).IsValid());
			TestFalse(TEXT("editor-volatile ActorLabel excluded"), FindProperty(AllUntouched, TEXT("ActorLabel")).IsValid());
		}
	}

	// Nothing the export touched changed: editor world, its dirty flag, the map files, no resident map packages.
	TestTrue(TEXT("editor world unchanged"), GEditor->GetEditorWorldContext().World() == ActiveWorld
		&& ActiveWorld->GetPathName() == ActiveWorldPath);
	TestEqual(TEXT("editor world dirty flag unchanged"), ActivePackage->IsDirty(), bActiveDirtyBefore);
	TestTrue(TEXT("MapA file unchanged"), IFileManager::Get().GetTimeStamp(*MapA.Filename) == StampA && IFileManager::Get().FileSize(*MapA.Filename) == SizeA);
	TestTrue(TEXT("MapB file unchanged"), IFileManager::Get().GetTimeStamp(*MapB.Filename) == StampB && IFileManager::Get().FileSize(*MapB.Filename) == SizeB);
	TestNull(TEXT("MapA package is not left resident"), FindPackage(nullptr, *MapAName));
	TestNull(TEXT("MapB package is not left resident"), FindPackage(nullptr, *MapBName));

	// Refusals.
	const FCortexCommandResult UnknownClass = RunExport(TEXT("NoSuchActorClass_XYZ"), {MapAName}, OutPath, TOptional<bool>());
	TestFalse(TEXT("unknown class refused"), UnknownClass.bSuccess);
	TestEqual(TEXT("unknown class code"), UnknownClass.ErrorCode, CortexErrorCodes::ClassNotFound);

	const FCortexCommandResult UnknownMap = RunExport(ClassName, {Dir + TEXT("/NoSuchMap")}, OutPath, TOptional<bool>());
	TestFalse(TEXT("unknown map refused"), UnknownMap.bSuccess);
	TestEqual(TEXT("unknown map code"), UnknownMap.ErrorCode, CortexErrorCodes::AssetNotFound);

	const FCortexCommandResult BadOutPath = RunExport(ClassName, {MapAName}, TEXT("C:/Windows/Temp/cortex_instances_forbidden.json"), TOptional<bool>());
	TestFalse(TEXT("out_path outside the allowed roots refused"), BadOutPath.bSuccess);
	TestEqual(TEXT("out_path refusal code"), BadOutPath.ErrorCode, CortexErrorCodes::InvalidFilePath);

	const FCortexCommandResult NoMaps = RunExport(ClassName, {}, OutPath, TOptional<bool>());
	TestFalse(TEXT("empty maps refused"), NoMaps.bSuccess);
	TestEqual(TEXT("empty maps code"), NoMaps.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelExportClassInstancesExternalActorsTest,
	"Cortex.Level.ExportClassInstancesExternalActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelExportClassInstancesExternalActorsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}

	const FString Suffix = NewSuffix();
	const FString MapName = FString::Printf(TEXT("/Game/Temp/InstanceExportExternal_%s/MapExternal"), *Suffix);
	const FString OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("CortexInstanceExportTest"), FString::Printf(TEXT("external_%s.json"), *Suffix)));

	FSavedMapFixture Map;
	ON_SCOPE_EXIT
	{
		Map.Cleanup();
		IFileManager::Get().Delete(*OutPath, false, true);
	};

	const bool bCreated = Map.Create(MapName, [](UWorld* World)
	{
		Spawn(World, ACortexLevelInstanceExportTestActor::StaticClass(), TEXT("StationExternal"), TEXT("External"));
	}, /*bExternal=*/true);
	if (!TestTrue(TEXT("external-actor map fixture created"), bCreated))
	{
		return false;
	}
	if (!TestTrue(TEXT("fixture wrote at least one external actor package"), Map.ExtraFiles.Num() > 0))
	{
		return false;
	}

	// Until the Asset Registry knows the freshly written actor files the engine loads none of them: that must be a
	// refusal, never a successful export that silently lacks the actor.
	const FCortexCommandResult Unscanned = RunExport(TEXT("CortexLevelInstanceExportTestActor"), {MapName}, OutPath, TOptional<bool>());
	if (Unscanned.bSuccess)
	{
		TestTrue(TEXT("an export that succeeds lists the external actor"),
			FindInstance(FindMap(ReadJson(OutPath), MapName), TEXT("StationExternal")).IsValid());
	}
	else
	{
		TestEqual(TEXT("an incomplete external actor load is refused"), Unscanned.ErrorCode, CortexErrorCodes::UnsupportedOperation);
	}
	TestNull(TEXT("map package is not left resident after the first export"), FindPackage(nullptr, *MapName));

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanModifiedAssetFiles(Map.ExtraFiles);

	// The engine loads a non-partitioned level's external actors with the level, so they are read like any other actor.
	const FCortexCommandResult Result = RunExport(TEXT("CortexLevelInstanceExportTestActor"), {MapName}, OutPath, TOptional<bool>());
	if (!TestTrue(TEXT("non-World-Partition map with external actors is exported"), Result.bSuccess))
	{
		AddInfo(FString::Printf(TEXT("export message: %s"), *Result.ErrorMessage));
		return false;
	}
	const TSharedPtr<FJsonObject> External = FindInstance(FindMap(ReadJson(OutPath), MapName), TEXT("StationExternal"));
	TestTrue(TEXT("the external actor's instance is in the report"), External.IsValid());
	TestEqual(TEXT("the unchanged external actor reports no override"), CountArray(External, TEXT("properties")), 0);
	TestNull(TEXT("map package is not left resident"), FindPackage(nullptr, *MapName));
	return true;
}
