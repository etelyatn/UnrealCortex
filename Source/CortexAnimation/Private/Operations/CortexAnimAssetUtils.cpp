#include "Operations/CortexAnimAssetUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
bool HasObjectSuffix(const FString& AssetPath)
{
	const int32 LastSlash = AssetPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	return LastSlash != INDEX_NONE && AssetPath.Mid(LastSlash + 1).Contains(TEXT("."));
}
}

int32 FCortexAnimAssetUtils::ReadLimit(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MaxValue)
{
	double Raw = 0.0;
	if (!Params.IsValid() || !Params->TryGetNumberField(FieldName, Raw))
	{
		return DefaultValue;
	}

	return FMath::Clamp(static_cast<int32>(Raw), 1, MaxValue);
}

TSharedPtr<FJsonObject> FCortexAnimAssetUtils::MakeLimitedArray(const TArray<TSharedPtr<FJsonValue>>& Items, int32 Limit)
{
	const int32 Count = Items.Num();
	const int32 Returned = FMath::Min(Count, Limit);

	TArray<TSharedPtr<FJsonValue>> ReturnedItems;
	ReturnedItems.Reserve(Returned);
	for (int32 Index = 0; Index < Returned; ++Index)
	{
		ReturnedItems.Add(Items[Index]);
	}

	TSharedPtr<FJsonObject> Collection = MakeShared<FJsonObject>();
	Collection->SetNumberField(TEXT("count"), Count);
	Collection->SetNumberField(TEXT("returned"), Returned);
	Collection->SetBoolField(TEXT("truncated"), Count > Returned);
	Collection->SetArrayField(TEXT("items"), ReturnedItems);
	return Collection;
}

TSharedPtr<FJsonObject> FCortexAnimAssetUtils::MakeErrorDetails(
	const FString& Field,
	const FString& AssetPath,
	const FString& ExpectedType,
	const FString& ActualType)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("field"), Field);
	Details->SetStringField(TEXT("asset_path"), AssetPath);
	Details->SetStringField(TEXT("expected_type"), ExpectedType);
	Details->SetStringField(TEXT("actual_type"), ActualType);
	Details->SetStringField(TEXT("retry"), TEXT("Call anim.list_assets to find a supported animation asset and retry."));
	return Details;
}

void FCortexAnimAssetUtils::SetCommonAssetFields(TSharedPtr<FJsonObject>& Data, const FCortexAnimResolvedAsset& Resolved)
{
	Data->SetStringField(TEXT("asset_path"), Resolved.AssetPath);
	Data->SetStringField(TEXT("asset_type"), Resolved.AssetType);
	Data->SetStringField(TEXT("name"), Resolved.AssetData.AssetName.ToString());
	if (Resolved.Asset != nullptr)
	{
		Data->SetObjectField(TEXT("fingerprint"), MakeObjectAssetFingerprint(Resolved.Asset).ToJson());
	}
}

bool FCortexAnimAssetUtils::IsSupportedAssetType(const FString& AssetType)
{
	return AssetType == TEXT("AnimSequence")
		|| AssetType == TEXT("AnimMontage")
		|| AssetType == TEXT("Skeleton")
		|| AssetType == TEXT("AnimBlueprint");
}

UClass* FCortexAnimAssetUtils::ClassForAssetType(const FString& AssetType)
{
	if (AssetType == TEXT("AnimSequence"))
	{
		return UAnimSequence::StaticClass();
	}
	if (AssetType == TEXT("AnimMontage"))
	{
		return UAnimMontage::StaticClass();
	}
	if (AssetType == TEXT("Skeleton"))
	{
		return USkeleton::StaticClass();
	}
	if (AssetType == TEXT("AnimBlueprint"))
	{
		return UAnimBlueprint::StaticClass();
	}

	return nullptr;
}

bool FCortexAnimAssetUtils::ListAnimationAssets(
	const FString& AssetType,
	const FString& Path,
	const FString& Query,
	TArray<FAssetData>& OutAssets,
	FCortexCommandResult& OutError)
{
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Asset Registry is not available yet; open the editor and retry anim.list_assets"));
		return false;
	}
	if (AssetRegistry->IsLoadingAssets())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Asset Registry is still loading assets; retry anim.list_assets after discovery completes"));
		return false;
	}

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*(Path.IsEmpty() ? FString(TEXT("/Game")) : Path)));

	if (IsSupportedAssetType(AssetType))
	{
		if (UClass* Class = ClassForAssetType(AssetType))
		{
			Filter.ClassPaths.Add(Class->GetClassPathName());
		}
	}
	else
	{
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UAnimMontage::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(USkeleton::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
	}

	TArray<FAssetData> Assets;
	AssetRegistry->GetAssets(Filter, Assets);
	if (!Query.IsEmpty())
	{
		Assets = Assets.FilterByPredicate([&Query](const FAssetData& AssetData)
		{
			return AssetData.GetObjectPathString().Contains(Query, ESearchCase::IgnoreCase)
				|| AssetData.PackageName.ToString().Contains(Query, ESearchCase::IgnoreCase);
		});
	}

	Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});

	OutAssets = MoveTemp(Assets);
	return true;
}

template <typename AssetT>
bool FCortexAnimAssetUtils::ResolveRequiredAsset(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* ExpectedType,
	FCortexAnimResolvedAsset& OutResolved,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), OutResolved.RequestedPath) || OutResolved.RequestedPath.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path (string)"));
		return false;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(OutResolved.RequestedPath);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid package path: %s"), *OutResolved.RequestedPath),
			MakeErrorDetails(TEXT("asset_path"), OutResolved.RequestedPath, ExpectedType, TEXT("")));
		return false;
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Asset Registry is not available yet; open the editor and retry"));
		return false;
	}
	if (AssetRegistry->IsLoadingAssets())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Asset Registry is still loading assets; retry after discovery completes"));
		return false;
	}

	FAssetData AssetData;
	if (HasObjectSuffix(OutResolved.RequestedPath))
	{
		AssetData = AssetRegistry->GetAssetByObjectPath(FSoftObjectPath(OutResolved.RequestedPath));
	}
	else
	{
		TArray<FAssetData> PackageAssets;
		AssetRegistry->GetAssetsByPackageName(FName(*PackageName), PackageAssets);
		if (PackageAssets.Num() > 0)
		{
			AssetData = PackageAssets[0];
		}
	}

	if (!AssetData.IsValid() || !FPackageName::DoesPackageExist(PackageName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Asset not found: %s"), *OutResolved.RequestedPath),
			MakeErrorDetails(TEXT("asset_path"), OutResolved.RequestedPath, ExpectedType, TEXT("")));
		return false;
	}

	OutResolved.AssetData = AssetData;
	OutResolved.AssetPath = OutResolved.AssetData.GetObjectPathString();
	OutResolved.PackageName = OutResolved.AssetData.PackageName.ToString();
	OutResolved.AssetType = OutResolved.AssetData.AssetClassPath.GetAssetName().ToString();

	UObject* Asset = OutResolved.AssetData.GetSoftObjectPath().ResolveObject();
	if (Asset == nullptr)
	{
		Asset = FindObject<UObject>(nullptr, *OutResolved.AssetPath);
	}
	if (Asset == nullptr)
	{
		Asset = OutResolved.AssetData.GetSoftObjectPath().TryLoad();
	}

	AssetT* TypedAsset = Cast<AssetT>(Asset);
	if (TypedAsset == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Failed to load %s as %s"), *OutResolved.AssetPath, ExpectedType),
			MakeErrorDetails(TEXT("asset_path"), OutResolved.AssetPath, ExpectedType, OutResolved.AssetType));
		return false;
	}

	OutResolved.Asset = TypedAsset;
	OutResolved.AssetType = ExpectedType;
	return true;
}

template bool FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimSequence>(
	const TSharedPtr<FJsonObject>&, const TCHAR*, FCortexAnimResolvedAsset&, FCortexCommandResult&);
template bool FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimMontage>(
	const TSharedPtr<FJsonObject>&, const TCHAR*, FCortexAnimResolvedAsset&, FCortexCommandResult&);
template bool FCortexAnimAssetUtils::ResolveRequiredAsset<USkeleton>(
	const TSharedPtr<FJsonObject>&, const TCHAR*, FCortexAnimResolvedAsset&, FCortexCommandResult&);
template bool FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimBlueprint>(
	const TSharedPtr<FJsonObject>&, const TCHAR*, FCortexAnimResolvedAsset&, FCortexCommandResult&);
