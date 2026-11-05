#pragma once

#include "CoreMinimal.h"
#include "CortexBPCommandHandler.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

namespace CortexBPList119Test
{
	/** Unique temp folder under /Game/Temp/Cortex119/List so the Asset Registry sees saved assets. */
	inline FString MakeUniqueDir(const FString& Leaf)
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
		return FString::Printf(TEXT("/Game/Temp/Cortex119/List/%s_%s"), *Leaf, *Suffix);
	}

	/** Create and save a Blueprint through the public create command. Exactly one of Type/ParentClass should be non-empty. */
	inline bool CreateBlueprint(
		FCortexBPCommandHandler& Handler,
		const FString& Dir,
		const FString& Name,
		const FString& Type,
		const FString& ParentClass)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("path"), Dir);
		if (!Type.IsEmpty())
		{
			Params->SetStringField(TEXT("type"), Type);
		}
		if (!ParentClass.IsEmpty())
		{
			Params->SetStringField(TEXT("parent_class"), ParentClass);
		}
		return Handler.Execute(TEXT("create"), Params).bSuccess;
	}

	/** Run blueprint.list scoped to Dir with optional type / exclude_types. */
	inline FCortexCommandResult RunList(
		FCortexBPCommandHandler& Handler,
		const FString& Dir,
		const FString& Type,
		const TArray<TSharedPtr<FJsonValue>>* ExcludeTypes)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Dir);
		if (!Type.IsEmpty())
		{
			Params->SetStringField(TEXT("type"), Type);
		}
		if (ExcludeTypes)
		{
			Params->SetArrayField(TEXT("exclude_types"), *ExcludeTypes);
		}
		return Handler.Execute(TEXT("list"), Params);
	}

	inline TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	/** Sorted asset names of the rows in a successful list result. */
	inline TArray<FString> RowNames(const FCortexCommandResult& Result)
	{
		TArray<FString> Names;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("blueprints"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				FString Name;
				if (Row.IsValid() && Row->AsObject().IsValid()
					&& Row->AsObject()->TryGetStringField(TEXT("name"), Name))
				{
					Names.Add(Name);
				}
			}
		}
		Names.Sort();
		return Names;
	}

	/** Row object for a given asset name, or null. */
	inline TSharedPtr<FJsonObject> FindRow(const FCortexCommandResult& Result, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("blueprints"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				FString RowName;
				if (Row.IsValid() && Row->AsObject().IsValid()
					&& Row->AsObject()->TryGetStringField(TEXT("name"), RowName) && RowName == Name)
				{
					return Row->AsObject();
				}
			}
		}
		return nullptr;
	}

	inline FString RowString(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field)
	{
		FString Value;
		if (Row.IsValid())
		{
			Row->TryGetStringField(Field, Value);
		}
		return Value;
	}

	/** Delete a Blueprint created by a test (force). Callers delete children before parents. */
	inline void DeleteBlueprint(FCortexBPCommandHandler& Handler, const FString& Dir, const FString& Name)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *Dir, *Name));
		Params->SetBoolField(TEXT("force"), true);
		Handler.Execute(TEXT("delete"), Params);
	}
}

namespace CortexBPList119Test
{
	/** Remove the (now empty) on-disk folder of a test's content directory after its assets were deleted. */
	inline void RemoveTestDirectory(const FString& Dir)
	{
		FString DiskPath;
		if (FPackageName::TryConvertLongPackageNameToFilename(Dir, DiskPath))
		{
			IFileManager::Get().DeleteDirectory(*DiskPath, false, true);
		}
	}
}
