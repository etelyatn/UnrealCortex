#pragma once

#include "CoreMinimal.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

/** Shared helpers of the snapshot export/compare tests. */
namespace CortexBPSnapshotTestSupport
{
	inline FString NewSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	}

	/** Transient Blueprint under /Temp (found in memory by FCortexBPAssetOps::LoadBlueprint). */
	inline UBlueprint* MakeBlueprint(const FString& Name, UClass* ParentClass)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), *Name));
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None);
	}

	inline void Retire(UObject* Asset)
	{
		if (Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
	}

	inline FString OutputPath(const FString& Label, const FString& Suffix)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("CortexSnapshotTest"), FString::Printf(TEXT("%s_%s.json"), *Label, *Suffix)));
	}

	inline TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path, FString* OutText = nullptr)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		if (OutText)
		{
			*OutText = Text;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	inline FCortexCommandResult Export(
		const FString& AssetPath, const FString& OutPath, const TArray<FString>* Sections = nullptr)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("out_path"), OutPath);
		if (Sections)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Section : *Sections)
			{
				Values.Add(MakeShared<FJsonValueString>(Section));
			}
			Params->SetArrayField(TEXT("sections"), Values);
		}
		return Handler.Execute(TEXT("export_snapshot"), Params);
	}

	/** Record of the snapshot document's section whose "key" equals Key, or null. */
	inline TSharedPtr<FJsonObject> FindRecord(const TSharedPtr<FJsonObject>& Snapshot, const TCHAR* Section, const FString& Key)
	{
		const TSharedPtr<FJsonObject>* Sections = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		if (!Snapshot.IsValid() || !Snapshot->TryGetObjectField(TEXT("sections"), Sections)
			|| !(*Sections)->TryGetArrayField(Section, Records))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Records)
		{
			const TSharedPtr<FJsonObject> Record = Value->AsObject();
			if (Record.IsValid() && Record->GetStringField(TEXT("key")) == Key)
			{
				return Record;
			}
		}
		return nullptr;
	}

	inline const TArray<TSharedPtr<FJsonValue>>* SectionRecords(const TSharedPtr<FJsonObject>& Snapshot, const TCHAR* Section)
	{
		const TSharedPtr<FJsonObject>* Sections = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		if (Snapshot.IsValid() && Snapshot->TryGetObjectField(TEXT("sections"), Sections)
			&& (*Sections)->TryGetArrayField(Section, Records))
		{
			return Records;
		}
		return nullptr;
	}
}
