#include "CortexBlueprintModule.h"
#include "CortexBPToolbarExtension.h"
#include "CortexCoreModule.h"
#include "Operations/CortexBPSerializationOps.h"
#include "ICortexCommandRegistry.h"
#include "CortexBPCommandHandler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/TimerHandle.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogCortexBlueprint);

void FCortexBlueprintModule::StartupModule()
{
	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module starting up"));

	ICortexCommandRegistry& Registry =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRegistry();

	Registry.RegisterDomain(
		TEXT("blueprint"),
		TEXT("Cortex Blueprint"),
		TEXT("1.0.0"),
		MakeShared<FCortexBPCommandHandler>()
	);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")
	).Get();

	if (AssetRegistry.IsLoadingAssets())
	{
		OnFilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddRaw(
			this,
			&FCortexBlueprintModule::OnAssetRegistryReady
		);
	}
	else
	{
		OnAssetRegistryReady();
	}

	FCortexBPToolbarExtension::Register();

	// Bind serialization handler for BP-to-C++ conversion
	FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	CoreModule.SetSerializationHandler(
		FOnCortexSerializationRequested::CreateStatic(&FCortexBPSerializationOps::Serialize));

	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint registered with CortexCore"));
}

void FCortexBlueprintModule::ShutdownModule()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
	{
		FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
		CoreModule.ClearSerializationHandler();
	}

	FCortexBPToolbarExtension::Unregister();

	if (GEditor != nullptr)
	{
		GEditor->GetTimerManager()->ClearTimer(CacheWriteTimerHandle);
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")
		).Get();
		AssetRegistry.OnAssetAdded().Remove(OnAssetAddedHandle);
		AssetRegistry.OnAssetUpdated().Remove(OnAssetUpdatedHandle);
		AssetRegistry.OnAssetRemoved().Remove(OnAssetRemovedHandle);
		AssetRegistry.OnFilesLoaded().Remove(OnFilesLoadedHandle);
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("CortexBlueprint module shutting down"));
}

void FCortexBlueprintModule::OnAssetRegistryReady()
{
	RebuildBlueprintCache();
	bInitialCacheBuilt = true;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")
	).Get();

	OnAssetAddedHandle = AssetRegistry.OnAssetAdded().AddRaw(this, &FCortexBlueprintModule::OnAssetChanged);
	OnAssetUpdatedHandle = AssetRegistry.OnAssetUpdated().AddRaw(this, &FCortexBlueprintModule::OnAssetChanged);
	OnAssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddRaw(this, &FCortexBlueprintModule::OnAssetRemoved);
}

void FCortexBlueprintModule::OnAssetChanged(const FAssetData& AssetData)
{
	if (!bInitialCacheBuilt)
	{
		return;
	}

	UClass* AssetClass = FindObject<UClass>(nullptr, *AssetData.AssetClassPath.ToString());
	if (AssetClass == nullptr || !AssetClass->IsChildOf(UBlueprint::StaticClass()))
	{
		return;
	}

	if (GEditor != nullptr)
	{
		GEditor->GetTimerManager()->ClearTimer(CacheWriteTimerHandle);
		GEditor->GetTimerManager()->SetTimer(
			CacheWriteTimerHandle,
			FTimerDelegate::CreateRaw(this, &FCortexBlueprintModule::WriteBlueprintCache),
			2.0f,
			false
		);
	}
}

void FCortexBlueprintModule::OnAssetRemoved(const FAssetData& AssetData)
{
	OnAssetChanged(AssetData);
}

void FCortexBlueprintModule::RebuildBlueprintCache()
{
	WriteBlueprintCache();
}

void FCortexBlueprintModule::WriteBlueprintCache()
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")
	).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> BlueprintAssets;
	AssetRegistry.GetAssets(Filter, BlueprintAssets);

	TSharedPtr<FJsonObject> CacheObj = MakeShared<FJsonObject>();
	CacheObj->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	CacheObj->SetNumberField(TEXT("blueprint_count"), BlueprintAssets.Num());

	TArray<TSharedPtr<FJsonValue>> BlueprintsArray;
	for (const FAssetData& Asset : BlueprintAssets)
	{
		TSharedPtr<FJsonObject> BPEntry = MakeShared<FJsonObject>();
		BPEntry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		BPEntry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		BPEntry->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());

		FString ParentClassPath;
		if (Asset.GetTagValue(FBlueprintTags::ParentClassPath, ParentClassPath))
		{
			BPEntry->SetStringField(TEXT("parent_class"), ParentClassPath);
		}

		BlueprintsArray.Add(MakeShared<FJsonValueObject>(BPEntry));
	}
	CacheObj->SetArrayField(TEXT("blueprints"), BlueprintsArray);

	const FString CacheDir = FPaths::ProjectSavedDir() / TEXT("Cortex");
	IFileManager::Get().MakeDirectory(*CacheDir, true);

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(CacheObj.ToSharedRef(), Writer);

	const FString CachePath = CacheDir / TEXT("blueprint-cache.json");
	const FString TempPath = CachePath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(JsonString, *TempPath))
	{
		UE_LOG(LogCortexBlueprint, Warning, TEXT("Failed to write blueprint cache temp file: %s"), *TempPath);
		return;
	}

	if (!IFileManager::Get().Move(*CachePath, *TempPath, true, true))
	{
		UE_LOG(LogCortexBlueprint, Warning, TEXT("Failed to move blueprint cache temp file: %s"), *TempPath);
		return;
	}

	UE_LOG(
		LogCortexBlueprint,
		Log,
		TEXT("Blueprint cache written: %d blueprints -> %s"),
		BlueprintAssets.Num(),
		*CachePath
	);
}

IMPLEMENT_MODULE(FCortexBlueprintModule, CortexBlueprint)
