#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Operations/CortexAnimAssetUtils.h"

class FJsonObject;
class UAnimMontage;
class UAnimSequence;
class USkeleton;

struct FCortexAnimMutationUtils
{
	static TSharedPtr<FJsonObject> MakeFieldDetails(const FString& Field, const FString& AssetPath = FString());

	static bool TryReadOptionalBool(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool bDefault,
		bool& bOutValue,
		FCortexCommandResult& OutError);

	static bool ReadFiniteNumber(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		double& OutValue,
		FCortexCommandResult& OutError,
		bool bRequired = true);

	static bool ValidateSequenceTime(
		UAnimSequence* Sequence,
		const FString& FieldName,
		double Time,
		FCortexCommandResult& OutError);

	static bool ValidateMontageTime(
		UAnimMontage* Montage,
		const FString& FieldName,
		double Time,
		FCortexCommandResult& OutError);

	static bool PrepareSequenceMutation(
		const TSharedPtr<FJsonObject>& Params,
		FCortexAnimResolvedAsset& OutResolved,
		UAnimSequence*& OutSequence,
		bool& bOutDryRun,
		bool& bOutSave,
		FCortexCommandResult& OutError);

	static bool PrepareMontageMutation(
		const TSharedPtr<FJsonObject>& Params,
		FCortexAnimResolvedAsset& OutResolved,
		UAnimMontage*& OutMontage,
		bool& bOutDryRun,
		bool& bOutSave,
		FCortexCommandResult& OutError);

	static bool PrepareSkeletonMutation(
		const TSharedPtr<FJsonObject>& Params,
		FCortexAnimResolvedAsset& OutResolved,
		USkeleton*& OutSkeleton,
		bool& bOutDryRun,
		bool& bOutSave,
		FCortexCommandResult& OutError);

	static bool SaveIfRequested(
		UAnimSequence* Sequence,
		bool bSave,
		TArray<FString>& OutSavedPackages,
		FCortexCommandResult& OutError);

	static bool SaveMontageIfRequested(
		UAnimMontage* Montage,
		bool bSave,
		TArray<FString>& OutSavedPackages,
		FCortexCommandResult& OutError);

	static bool SaveSkeletonIfRequested(
		USkeleton* Skeleton,
		bool bSave,
		TArray<FString>& OutSavedPackages,
		FCortexCommandResult& OutError);

	static TSharedPtr<FJsonObject> MakeMutationResponse(
		const FCortexAnimResolvedAsset& Resolved,
		const FString& Operation,
		const TSharedPtr<FJsonObject>& Selector,
		bool bChanged,
		bool bDirtyBefore,
		bool bDirtyAfter,
		bool bSaved,
		const TArray<FString>& SavedPackages,
		const TSharedPtr<FJsonObject>& Before,
		const TSharedPtr<FJsonObject>& After,
		UObject* Asset);
};
