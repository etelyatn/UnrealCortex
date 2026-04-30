#include "CortexAssetFingerprint.h"

#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "IO/IoHash.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace
{
bool TryComputeSavedPackageHashByName(const FString& PackageName, FString& OutHash)
{
	OutHash.Empty();

	if (PackageName.IsEmpty())
	{
		return false;
	}

	FString PackageFilename;
	if (!FPackageName::DoesPackageExist(PackageName, &PackageFilename) || PackageFilename.IsEmpty())
	{
		return false;
	}

	TArray64<uint8> PackageBytes;
	if (!FFileHelper::LoadFileToArray(PackageBytes, *PackageFilename))
	{
		return false;
	}

	OutHash = LexToString(FIoHash::HashBuffer(PackageBytes.GetData(), PackageBytes.Num()));
	return !OutHash.IsEmpty();
}

TOptional<uint32> BuildSemanticObjectSignature(const UObject* Object)
{
	if (Object == nullptr)
	{
		return TOptional<uint32>();
	}

	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		return GetTypeHash(static_cast<uint32>(Blueprint->Status));
	}

	const UPackage* Package = Object->GetPackage();
	if (Package == nullptr || !Package->IsDirty())
	{
		return TOptional<uint32>();
	}

	const TSharedPtr<FJsonObject> StateJson = FCortexSerializer::NonDefaultPropertiesToJson(Object, 1);
	if (!StateJson.IsValid() || StateJson->Values.Num() == 0)
	{
		return TOptional<uint32>();
	}

	FString StateText;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&StateText);
	if (!FJsonSerializer::Serialize(StateJson.ToSharedRef(), Writer) || StateText.IsEmpty())
	{
		return TOptional<uint32>();
	}

	return GetTypeHash(StateText);
}
}

TSharedPtr<FJsonObject> FCortexAssetFingerprint::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("package_saved_hash"), PackageSavedHash);
	Json->SetBoolField(TEXT("is_dirty"), bIsDirty);
	Json->SetStringField(TEXT("dirty_epoch"), LexToString(DirtyEpoch));
	Json->SetBoolField(TEXT("not_ready"), bNotReady);

	if (CompiledSignatureCrc.IsSet())
	{
		Json->SetNumberField(TEXT("compiled_signature_crc"), static_cast<double>(CompiledSignatureCrc.GetValue()));
	}

	return Json;
}

FCortexAssetFingerprint MakePackageAssetFingerprint(
	const UPackage* Package,
	TOptional<uint32> CompiledSignatureCrc)
{
	return MakePackageNameAssetFingerprint(
		Package ? Package->GetName() : FString(),
		Package && Package->IsDirty(),
		CompiledSignatureCrc);
}

FCortexAssetFingerprint MakePackageNameAssetFingerprint(
	const FString& PackageName,
	bool bIsDirty,
	TOptional<uint32> CompiledSignatureCrc)
{
	FCortexAssetFingerprint Fingerprint;
	Fingerprint.bIsDirty = bIsDirty;
	Fingerprint.CompiledSignatureCrc = CompiledSignatureCrc;
	TryComputeSavedPackageHashByName(PackageName, Fingerprint.PackageSavedHash);
	Fingerprint.bNotReady = PackageName.IsEmpty()
		|| (Fingerprint.PackageSavedHash.IsEmpty() && !Fingerprint.CompiledSignatureCrc.IsSet());
	return Fingerprint;
}

FCortexAssetFingerprint MakeObjectAssetFingerprint(
	const UObject* Object,
	TOptional<uint32> CompiledSignatureCrc)
{
	if (!CompiledSignatureCrc.IsSet())
	{
		CompiledSignatureCrc = BuildSemanticObjectSignature(Object);
	}

	return MakePackageNameAssetFingerprint(
		Object && Object->GetPackage() ? Object->GetPackage()->GetName() : FString(),
		Object && Object->GetPackage() && Object->GetPackage()->IsDirty(),
		CompiledSignatureCrc);
}
