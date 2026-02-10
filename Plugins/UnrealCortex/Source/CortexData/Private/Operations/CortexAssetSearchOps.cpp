
#include "Operations/CortexAssetSearchOps.h"
#include "CortexDataModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FCortexCommandResult FCortexDataAssetSearchOps::SearchAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FString ClassFilter;
	FString PathFilter;
	int32 Limit = 50;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("query"), Query);
		Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);

		int32 ParamLimit = 0;
		if (Params->TryGetNumberField(TEXT("limit"), ParamLimit) && ParamLimit > 0)
		{
			Limit = ParamLimit;
		}
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available")
		);
	}

	FARFilter Filter;
	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> AssetDataList;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	int32 Count = 0;

	for (const FAssetData& AssetData : AssetDataList)
	{
		if (Count >= Limit)
		{
			break;
		}

		FString AssetName = AssetData.AssetName.ToString();
		FString AssetPath = AssetData.GetObjectPathString();
		FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

		// Query filter (case-insensitive text search on name and path)
		if (!Query.IsEmpty())
		{
			if (!AssetName.Contains(Query, ESearchCase::IgnoreCase)
				&& !AssetPath.Contains(Query, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Class filter
		if (!ClassFilter.IsEmpty() && ClassName != ClassFilter)
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), AssetPath);
		Entry->SetStringField(TEXT("class_name"), ClassName);
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
		Count++;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("assets"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());
	Data->SetNumberField(TEXT("total_before_limit"), AssetDataList.Num());

	return FCortexCommandRouter::Success(Data);
}
