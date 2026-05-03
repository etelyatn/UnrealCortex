#include "Operations/CortexAssetFingerprintOps.h"

#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAssetOps.h"
#include "Dom/JsonObject.h"

FCortexCommandResult FCortexAssetFingerprintOps::AssetFingerprint(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
	if (!Params->TryGetArrayField(TEXT("paths"), Paths) || Paths == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: paths (array)"));
	}

	TArray<TSharedPtr<FJsonValue>> Fingerprints;
	Fingerprints.Reserve(Paths->Num());

	for (const TSharedPtr<FJsonValue>& PathValue : *Paths)
	{
		FString AssetPath;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		FCortexAssetFingerprint Fingerprint;
		if (!PathValue.IsValid() || !PathValue->TryGetString(AssetPath) || AssetPath.IsEmpty())
		{
			AssetPath.Empty();
			Fingerprint.bNotReady = true;
		}
		else
		{
			const FAssetData AssetData = FCortexAssetOps::ResolveLiteralAssetPath(AssetPath);
			if (AssetData.IsValid())
			{
				UObject* LoadedAsset = AssetData.GetSoftObjectPath().ResolveObject();
				Fingerprint = LoadedAsset != nullptr
					? MakeObjectAssetFingerprint(LoadedAsset)
					: MakePackageNameAssetFingerprint(AssetData.PackageName.ToString());
			}
			else
			{
				Fingerprint.bNotReady = true;
			}
		}

		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetObjectField(TEXT("fingerprint"), Fingerprint.ToJson());
		Fingerprints.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("fingerprints"), Fingerprints);
	Data->SetNumberField(TEXT("count"), Fingerprints.Num());
	return FCortexCommandRouter::Success(Data);
}
