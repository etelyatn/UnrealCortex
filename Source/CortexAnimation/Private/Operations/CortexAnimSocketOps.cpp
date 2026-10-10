#include "Operations/CortexAnimSocketOps.h"

#include "Animation/Skeleton.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimMutationUtils.h"
#include "ScopedTransaction.h"

namespace
{
struct FSocketSelector
{
	int32 Index = INDEX_NONE;
	FName SocketName;
	FName BoneName;
};

TSharedPtr<FJsonObject> SocketNameDetails(const FString& Field, const FString& AssetPath)
{
	return FCortexAnimMutationUtils::MakeFieldDetails(Field, AssetPath);
}

TSharedPtr<FJsonObject> VectorToJson(const FVector& Value)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("x"), Value.X);
	Data->SetNumberField(TEXT("y"), Value.Y);
	Data->SetNumberField(TEXT("z"), Value.Z);
	return Data;
}

TSharedPtr<FJsonObject> RotatorToJson(const FRotator& Value)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("pitch"), Value.Pitch);
	Data->SetNumberField(TEXT("yaw"), Value.Yaw);
	Data->SetNumberField(TEXT("roll"), Value.Roll);
	return Data;
}

TSharedPtr<FJsonObject> SocketToJson(const USkeletalMeshSocket& Socket, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("socket_name"), Socket.SocketName.ToString());
	Data->SetStringField(TEXT("bone_name"), Socket.BoneName.ToString());
	Data->SetObjectField(TEXT("location"), VectorToJson(Socket.RelativeLocation));
	Data->SetObjectField(TEXT("rotation"), RotatorToJson(Socket.RelativeRotation));
	Data->SetObjectField(TEXT("scale"), VectorToJson(Socket.RelativeScale));
	return Data;
}

TSharedPtr<FJsonObject> MissingSocketJson()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	return Data;
}

TSharedPtr<FJsonObject> SelectorToJson(const FSocketSelector& Selector)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("index"), Selector.Index);
	Data->SetStringField(TEXT("socket_name"), Selector.SocketName.ToString());
	Data->SetStringField(TEXT("bone_name"), Selector.BoneName.ToString());
	return Data;
}

bool TryReadSocketSelector(
	const TSharedPtr<FJsonObject>& Params,
	FSocketSelector& OutSelector,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* Selector = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("selector"), Selector) || Selector == nullptr || !Selector->IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: selector (object with index, socket_name, bone_name)"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector")));
		return false;
	}

	double RawIndex = 0.0;
	FString SocketName;
	FString BoneName;
	if (!(*Selector)->TryGetNumberField(TEXT("index"), RawIndex)
		|| !FMath::IsFinite(RawIndex)
		|| RawIndex < 0.0
		|| RawIndex > static_cast<double>(TNumericLimits<int32>::Max())
		|| FMath::FloorToDouble(RawIndex) != RawIndex)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.index must be a non-negative integer"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.index")));
		return false;
	}
	if (!(*Selector)->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.socket_name must be a non-empty string"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.socket_name")));
		return false;
	}
	if (!(*Selector)->TryGetStringField(TEXT("bone_name"), BoneName) || BoneName.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.bone_name must be a non-empty string"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.bone_name")));
		return false;
	}

	OutSelector.Index = static_cast<int32>(RawIndex);
	OutSelector.SocketName = FName(*SocketName);
	OutSelector.BoneName = FName(*BoneName);
	return true;
}

int32 FindSocketExact(const USkeleton* Skeleton, const FSocketSelector& Selector)
{
	if (Skeleton == nullptr || !Skeleton->Sockets.IsValidIndex(Selector.Index))
	{
		return INDEX_NONE;
	}

	const USkeletalMeshSocket* Socket = Skeleton->Sockets[Selector.Index];
	return Socket != nullptr
		&& Socket->SocketName == Selector.SocketName
		&& Socket->BoneName == Selector.BoneName
		? Selector.Index
		: INDEX_NONE;
}

bool TryReadFiniteNumber(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	double& OutValue)
{
	return Object.IsValid()
		&& Object->TryGetNumberField(FieldName, OutValue)
		&& FMath::IsFinite(OutValue);
}

bool TryReadVectorField(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	FVector& OutValue,
	bool& bOutPresent,
	FCortexCommandResult& OutError,
	bool bRequired)
{
	bOutPresent = Params.IsValid() && Params->HasField(FieldName);
	if (!bOutPresent)
	{
		return !bRequired;
	}

	const TSharedPtr<FJsonObject>* Object = nullptr;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	if (!Params->TryGetObjectField(FieldName, Object)
		|| Object == nullptr
		|| !TryReadFiniteNumber(*Object, TEXT("x"), X)
		|| !TryReadFiniteNumber(*Object, TEXT("y"), Y)
		|| !TryReadFiniteNumber(*Object, TEXT("z"), Z)
		|| !FMath::IsFinite(static_cast<float>(X))
		|| !FMath::IsFinite(static_cast<float>(Y))
		|| !FMath::IsFinite(static_cast<float>(Z)))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must contain finite x, y, and z numbers"), FieldName),
			FCortexAnimMutationUtils::MakeFieldDetails(FieldName));
		return false;
	}

	OutValue = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
	return true;
}

bool TryReadRotatorField(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	FRotator& OutValue,
	bool& bOutPresent,
	FCortexCommandResult& OutError,
	bool bRequired)
{
	bOutPresent = Params.IsValid() && Params->HasField(FieldName);
	if (!bOutPresent)
	{
		return !bRequired;
	}

	const TSharedPtr<FJsonObject>* Object = nullptr;
	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
	if (!Params->TryGetObjectField(FieldName, Object)
		|| Object == nullptr
		|| !TryReadFiniteNumber(*Object, TEXT("pitch"), Pitch)
		|| !TryReadFiniteNumber(*Object, TEXT("yaw"), Yaw)
		|| !TryReadFiniteNumber(*Object, TEXT("roll"), Roll)
		|| !FMath::IsFinite(static_cast<float>(Pitch))
		|| !FMath::IsFinite(static_cast<float>(Yaw))
		|| !FMath::IsFinite(static_cast<float>(Roll)))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must contain finite pitch, yaw, and roll numbers"), FieldName),
			FCortexAnimMutationUtils::MakeFieldDetails(FieldName));
		return false;
	}

	OutValue = FRotator(static_cast<float>(Pitch), static_cast<float>(Yaw), static_cast<float>(Roll));
	return true;
}

FCortexCommandResult SocketNotFound(const FCortexAnimResolvedAsset& Resolved)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::AssetNotFound,
		TEXT("Socket selector did not match an existing skeleton socket"),
		SocketNameDetails(TEXT("selector"), Resolved.AssetPath));
}
}

FCortexCommandResult FCortexAnimSocketOps::AddSocket(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FCortexAnimResolvedAsset Resolved;
	USkeleton* Skeleton = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSkeletonMutation(Params, Resolved, Skeleton, bDryRun, bSave, Error))
	{
		return Error;
	}

	FString SocketNameString;
	FString BoneNameString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("socket_name"), SocketNameString) || SocketNameString.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: socket_name (non-empty string)"), SocketNameDetails(TEXT("socket_name"), Resolved.AssetPath));
	}
	if (!Params->TryGetStringField(TEXT("bone_name"), BoneNameString) || BoneNameString.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: bone_name (non-empty string)"), SocketNameDetails(TEXT("bone_name"), Resolved.AssetPath));
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	bool bLocationPresent = false;
	bool bRotationPresent = false;
	bool bScalePresent = false;
	if (!TryReadVectorField(Params, TEXT("location"), Location, bLocationPresent, Error, false)
		|| !TryReadRotatorField(Params, TEXT("rotation"), Rotation, bRotationPresent, Error, false)
		|| !TryReadVectorField(Params, TEXT("scale"), Scale, bScalePresent, Error, false))
	{
		return Error;
	}
	(void)bLocationPresent;
	(void)bRotationPresent;
	(void)bScalePresent;

	const FName SocketName(*SocketNameString);
	const FName BoneName(*BoneNameString);
	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("bone_name does not exist in the skeleton reference skeleton"),
			SocketNameDetails(TEXT("bone_name"), Resolved.AssetPath));
	}
	for (const USkeletalMeshSocket* ExistingSocket : Skeleton->Sockets)
	{
		if (ExistingSocket != nullptr && ExistingSocket->SocketName == SocketName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::AssetAlreadyExists,
				TEXT("Skeleton socket name already exists"),
				SocketNameDetails(TEXT("socket_name"), Resolved.AssetPath));
		}
	}

	const bool bDirtyBefore = Skeleton->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = MissingSocketJson();
	FSocketSelector PlannedSelector;
	PlannedSelector.Index = Skeleton->Sockets.Num();
	PlannedSelector.SocketName = SocketName;
	PlannedSelector.BoneName = BoneName;
	TSharedPtr<FJsonObject> PlannedAfter = MakeShared<FJsonObject>();
	PlannedAfter->SetBoolField(TEXT("exists"), true);
	PlannedAfter->SetNumberField(TEXT("index"), PlannedSelector.Index);
	PlannedAfter->SetStringField(TEXT("socket_name"), SocketNameString);
	PlannedAfter->SetStringField(TEXT("bone_name"), BoneNameString);
	PlannedAfter->SetObjectField(TEXT("location"), VectorToJson(Location));
	PlannedAfter->SetObjectField(TEXT("rotation"), RotatorToJson(Rotation));
	PlannedAfter->SetObjectField(TEXT("scale"), VectorToJson(Scale));

	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("add_socket"), SelectorToJson(PlannedSelector), true, bDirtyBefore, bDirtyBefore, false, {}, Before, PlannedAfter, Skeleton));
	}

	USkeletalMeshSocket* NewSocket = nullptr;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Skeleton Socket")));
		Skeleton->Modify();
		NewSocket = NewObject<USkeletalMeshSocket>(Skeleton, NAME_None, RF_Transactional);
		NewSocket->SocketName = SocketName;
		NewSocket->BoneName = BoneName;
		NewSocket->RelativeLocation = Location;
		NewSocket->RelativeRotation = Rotation;
		NewSocket->RelativeScale = Scale;
		Skeleton->Sockets.Add(NewSocket);
		Skeleton->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveSkeletonIfRequested(Skeleton, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("add_socket"), SelectorToJson(PlannedSelector), true, bDirtyBefore, Skeleton->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, SocketToJson(*NewSocket, Skeleton->Sockets.Find(NewSocket)), Skeleton));
}

FCortexCommandResult FCortexAnimSocketOps::SetSocketTransform(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FSocketSelector Selector;
	if (!TryReadSocketSelector(Params, Selector, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	USkeleton* Skeleton = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSkeletonMutation(Params, Resolved, Skeleton, bDryRun, bSave, Error))
	{
		return Error;
	}

	const int32 SocketIndex = FindSocketExact(Skeleton, Selector);
	if (SocketIndex == INDEX_NONE)
	{
		return SocketNotFound(Resolved);
	}

	USkeletalMeshSocket* Socket = Skeleton->Sockets[SocketIndex];
	FVector Location = Socket->RelativeLocation;
	FRotator Rotation = Socket->RelativeRotation;
	FVector Scale = Socket->RelativeScale;
	bool bLocationPresent = false;
	bool bRotationPresent = false;
	bool bScalePresent = false;
	if (!TryReadVectorField(Params, TEXT("location"), Location, bLocationPresent, Error, false)
		|| !TryReadRotatorField(Params, TEXT("rotation"), Rotation, bRotationPresent, Error, false)
		|| !TryReadVectorField(Params, TEXT("scale"), Scale, bScalePresent, Error, false))
	{
		return Error;
	}
	const bool bDirtyBefore = Skeleton->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = SocketToJson(*Socket, SocketIndex);
	TSharedPtr<FJsonObject> PlannedAfter = MakeShared<FJsonObject>();
	PlannedAfter->SetBoolField(TEXT("exists"), true);
	PlannedAfter->SetNumberField(TEXT("index"), SocketIndex);
	PlannedAfter->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
	PlannedAfter->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
	PlannedAfter->SetObjectField(TEXT("location"), VectorToJson(Location));
	PlannedAfter->SetObjectField(TEXT("rotation"), RotatorToJson(Rotation));
	PlannedAfter->SetObjectField(TEXT("scale"), VectorToJson(Scale));
	const bool bChanged = !Location.Equals(Socket->RelativeLocation)
		|| !Rotation.Equals(Socket->RelativeRotation)
		|| !Scale.Equals(Socket->RelativeScale);

	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("set_socket_transform"), SelectorToJson(Selector), bChanged, bDirtyBefore, bDirtyBefore, false, {}, Before, PlannedAfter, Skeleton));
	}

	if (!bChanged)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("set_socket_transform"), SelectorToJson(Selector), false, bDirtyBefore, bDirtyBefore, false, {}, Before, Before, Skeleton));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Skeleton Socket Transform")));
		Skeleton->Modify();
		Socket->Modify();
		Socket->RelativeLocation = Location;
		Socket->RelativeRotation = Rotation;
		Socket->RelativeScale = Scale;
		Skeleton->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveSkeletonIfRequested(Skeleton, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("set_socket_transform"), SelectorToJson(Selector), true, bDirtyBefore, Skeleton->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, SocketToJson(*Socket, SocketIndex), Skeleton));
}

FCortexCommandResult FCortexAnimSocketOps::RemoveSocket(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FSocketSelector Selector;
	if (!TryReadSocketSelector(Params, Selector, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	USkeleton* Skeleton = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSkeletonMutation(Params, Resolved, Skeleton, bDryRun, bSave, Error))
	{
		return Error;
	}

	const int32 SocketIndex = FindSocketExact(Skeleton, Selector);
	if (SocketIndex == INDEX_NONE)
	{
		return SocketNotFound(Resolved);
	}

	USkeletalMeshSocket* Socket = Skeleton->Sockets[SocketIndex];
	const TSharedPtr<FJsonObject> Before = SocketToJson(*Socket, SocketIndex);
	const TSharedPtr<FJsonObject> After = MissingSocketJson();
	const bool bDirtyBefore = Skeleton->GetPackage()->IsDirty();
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("remove_socket"), SelectorToJson(Selector), true, bDirtyBefore, bDirtyBefore, false, {}, Before, After, Skeleton));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Skeleton Socket")));
		Skeleton->Modify();
		Skeleton->Sockets.Remove(Socket);
		Skeleton->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveSkeletonIfRequested(Skeleton, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("remove_socket"), SelectorToJson(Selector), true, bDirtyBefore, Skeleton->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, After, Skeleton));
}
