#include "Misc/AutomationTest.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

namespace
{
struct FCortexSocketTestAssetGuard
{
	USkeleton* Skeleton = nullptr;

	~FCortexSocketTestAssetGuard()
	{
		UPackage* Package = Skeleton != nullptr ? Skeleton->GetPackage() : nullptr;
		const FString PackageName = Package != nullptr ? Package->GetName() : FString();
		if (!PackageName.IsEmpty())
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			if (FPaths::FileExists(Filename))
			{
				IFileManager::Get().Delete(*Filename, false, true, true);
			}
			const FString Directory = FPaths::GetPath(Filename);
			if (FPaths::DirectoryExists(Directory))
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, true);
			}
		}

		if (Skeleton != nullptr)
		{
			Skeleton->Sockets.Reset();
			Skeleton->ClearFlags(RF_Public | RF_Standalone);
			Skeleton->MarkAsGarbage();
		}
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Skeleton = nullptr;
	}
};

FCortexCommandRouter CreateSocketRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

FString MakeSocketSuffix()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
}

USkeleton* CreateSocketTestAsset(FAutomationTestBase& Test, FCortexSocketTestAssetGuard& Guard, FString& OutAssetPath)
{
	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimSocket_%s/US_SkeletonTest"), *MakeSocketSuffix());
	UPackage* Package = CreatePackage(*PackageName);
	Test.TestNotNull(TEXT("test skeleton package created"), Package);
	if (Package == nullptr)
	{
		return nullptr;
	}

	USkeleton* Skeleton = NewObject<USkeleton>(
		Package,
		USkeleton::StaticClass(),
		TEXT("US_SkeletonTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	Test.TestNotNull(TEXT("test skeleton created"), Skeleton);
	if (Skeleton == nullptr)
	{
		return nullptr;
	}

	{
		FReferenceSkeletonModifier Modifier(Skeleton);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_r")), TEXT("hand_r"), 0), FTransform::Identity);
	}

	FAssetRegistryModule::AssetCreated(Skeleton);
	Package->SetDirtyFlag(false);
	Guard.Skeleton = Skeleton;
	OutAssetPath = Skeleton->GetPathName();
	return Skeleton;
}

TSharedPtr<FJsonObject> SocketFingerprintFor(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

TSharedPtr<FJsonObject> SocketCurrentFingerprint(const FCortexCommandResult& Result)
{
	if (!Result.bSuccess || !Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("current_fingerprint")))
	{
		return MakeShared<FJsonObject>();
	}
	return Result.Data->GetObjectField(TEXT("current_fingerprint"));
}

TSharedPtr<FJsonObject> VectorJson(const FVector& Value)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("x"), Value.X);
	Data->SetNumberField(TEXT("y"), Value.Y);
	Data->SetNumberField(TEXT("z"), Value.Z);
	return Data;
}

TSharedPtr<FJsonObject> RotatorJson(const FRotator& Value)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("pitch"), Value.Pitch);
	Data->SetNumberField(TEXT("yaw"), Value.Yaw);
	Data->SetNumberField(TEXT("roll"), Value.Roll);
	return Data;
}

TSharedPtr<FJsonObject> AddSocketParams(
	const FString& AssetPath,
	const FString& SocketName,
	const FString& BoneName,
	const FVector& Location,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("socket_name"), SocketName);
	Params->SetStringField(TEXT("bone_name"), BoneName);
	Params->SetObjectField(TEXT("location"), VectorJson(Location));
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonObject> SocketSelector(int32 Index, const FString& SocketName, const FString& BoneName)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetNumberField(TEXT("index"), Index);
	Selector->SetStringField(TEXT("socket_name"), SocketName);
	Selector->SetStringField(TEXT("bone_name"), BoneName);
	return Selector;
}

TSharedPtr<FJsonObject> SetSocketTransformParams(
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& Selector,
	const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), Selector);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("save"), false);
	return Params;
}

TSharedPtr<FJsonObject> RemoveSocketParams(
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& Selector,
	const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), Selector);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("save"), false);
	return Params;
}

TSharedPtr<FJsonObject> SkeletonInfoParams(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	return Params;
}

int32 SocketIndexByName(const USkeleton* Skeleton, const FName& SocketName)
{
	if (Skeleton == nullptr)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Skeleton->Sockets.Num(); ++Index)
	{
		if (Skeleton->Sockets[Index] != nullptr && Skeleton->Sockets[Index]->SocketName == SocketName)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TSharedPtr<FJsonObject> SocketAfter(const FCortexCommandResult& Result)
{
	return Result.bSuccess && Result.Data.IsValid() ? Result.Data->GetObjectField(TEXT("after")) : MakeShared<FJsonObject>();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSocketAuthoringAddInspectTest,
	"Cortex.Animation.SocketAuthoring.AddInspectAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSocketAuthoringAddInspectTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexSocketTestAssetGuard Guard;
	USkeleton* Skeleton = CreateSocketTestAsset(*this, Guard, AssetPath);
	if (Skeleton == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateSocketRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Muzzle"), TEXT("hand_r"), FVector(1.0, 2.0, 3.0), SocketFingerprintFor(Skeleton)));
	TestTrue(TEXT("add skeleton socket succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		return false;
	}

	TestEqual(TEXT("socket added to skeleton"), Skeleton->Sockets.Num(), 1);
	TestEqual(TEXT("socket outer is skeleton"), Skeleton->Sockets[0]->GetOuter(), static_cast<UObject*>(Skeleton));
	TestTrue(TEXT("socket is transactional"), Skeleton->Sockets[0]->HasAnyFlags(RF_Transactional));
	const TSharedPtr<FJsonObject> After = SocketAfter(Add);
	TestEqual(TEXT("socket name readback"), After->GetStringField(TEXT("socket_name")), FString(TEXT("Cortex_Muzzle")));
	TestEqual(TEXT("bone name readback"), After->GetStringField(TEXT("bone_name")), FString(TEXT("hand_r")));
	TestEqual(TEXT("location x readback"), After->GetObjectField(TEXT("location"))->GetNumberField(TEXT("x")), 1.0);

	FCortexCommandResult Inspect = Router.Execute(TEXT("anim.get_skeleton_info"), SkeletonInfoParams(AssetPath));
	TestTrue(TEXT("skeleton inspection succeeds"), Inspect.bSuccess);
	if (Inspect.bSuccess && Inspect.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Sockets = Inspect.Data->GetObjectField(TEXT("sockets"));
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TestTrue(TEXT("sockets collection exists"), Sockets->TryGetArrayField(TEXT("items"), Items));
		TestEqual(TEXT("one socket is inspected"), Items != nullptr ? Items->Num() : INDEX_NONE, 1);
		if (Items != nullptr && Items->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
			TestTrue(TEXT("socket_name inspection field exists"), Item->HasField(TEXT("socket_name")));
			TestTrue(TEXT("transform inspection fields exist"), Item->HasField(TEXT("location")) && Item->HasField(TEXT("rotation")) && Item->HasField(TEXT("scale")));
		}
	}

	FCortexCommandResult Duplicate = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Muzzle"), TEXT("hand_r"), FVector::ZeroVector, SocketCurrentFingerprint(Add)));
	TestFalse(TEXT("duplicate socket name rejects"), Duplicate.bSuccess);
	TestEqual(TEXT("duplicate socket error"), Duplicate.ErrorCode, FString(CortexErrorCodes::AssetAlreadyExists));

	FCortexCommandResult UnknownBone = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Unknown"), TEXT("missing_bone"), FVector::ZeroVector, SocketCurrentFingerprint(Add)));
	TestFalse(TEXT("unknown bone rejects"), UnknownBone.bSuccess);
	TestEqual(TEXT("unknown bone error"), UnknownBone.ErrorCode, FString(CortexErrorCodes::InvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSocketAuthoringTransformValidationTest,
	"Cortex.Animation.SocketAuthoring.TransformValidationAndDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSocketAuthoringTransformValidationTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexSocketTestAssetGuard Guard;
	USkeleton* Skeleton = CreateSocketTestAsset(*this, Guard, AssetPath);
	if (Skeleton == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateSocketRouter();
	FCortexCommandResult DryRun = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Dry"), TEXT("hand_r"), FVector(1.0, 2.0, 3.0), SocketFingerprintFor(Skeleton), true));
	TestTrue(TEXT("dry-run add succeeds"), DryRun.bSuccess);
	TestEqual(TEXT("dry-run does not mutate sockets"), Skeleton->Sockets.Num(), 0);
	TestFalse(TEXT("dry-run does not dirty package"), Skeleton->GetPackage()->IsDirty());

	TSharedPtr<FJsonObject> StaleFingerprint = SocketFingerprintFor(Skeleton);
	StaleFingerprint->SetBoolField(TEXT("is_dirty"), true);
	FCortexCommandResult Stale = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Stale"), TEXT("hand_r"), FVector::ZeroVector, StaleFingerprint));
	TestFalse(TEXT("stale add rejects"), Stale.bSuccess);
	TestEqual(TEXT("stale add error"), Stale.ErrorCode, FString(CortexErrorCodes::StalePrecondition));

	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Transform"), TEXT("hand_r"), FVector(4.0, 5.0, 6.0), SocketFingerprintFor(Skeleton)));
	TestTrue(TEXT("baseline socket add succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Malformed = SetSocketTransformParams(AssetPath, SocketSelector(0, TEXT("Cortex_Transform"), TEXT("hand_r")), SocketCurrentFingerprint(Add));
	TSharedPtr<FJsonObject> IncompleteVector = MakeShared<FJsonObject>();
	IncompleteVector->SetNumberField(TEXT("x"), 1.0);
	Malformed->SetObjectField(TEXT("location"), IncompleteVector);
	FCortexCommandResult MalformedResult = Router.Execute(TEXT("anim.set_socket_transform"), Malformed);
	TestFalse(TEXT("malformed vector rejects"), MalformedResult.bSuccess);
	TestEqual(TEXT("malformed vector error"), MalformedResult.ErrorCode, FString(CortexErrorCodes::InvalidField));

	TSharedPtr<FJsonObject> LargeIndex = SetSocketTransformParams(AssetPath, SocketSelector(0, TEXT("Cortex_Transform"), TEXT("hand_r")), SocketCurrentFingerprint(Add));
	LargeIndex->GetObjectField(TEXT("selector"))->SetNumberField(TEXT("index"), static_cast<double>(TNumericLimits<int32>::Max()) + 1.0);
	FCortexCommandResult LargeIndexResult = Router.Execute(TEXT("anim.set_socket_transform"), LargeIndex);
	TestFalse(TEXT("oversized selector index rejects"), LargeIndexResult.bSuccess);
	TestEqual(TEXT("oversized selector index error"), LargeIndexResult.ErrorCode, FString(CortexErrorCodes::InvalidField));

	TSharedPtr<FJsonObject> Omitted = SetSocketTransformParams(AssetPath, SocketSelector(0, TEXT("Cortex_Transform"), TEXT("hand_r")), SocketCurrentFingerprint(Add));
	FCortexCommandResult OmittedResult = Router.Execute(TEXT("anim.set_socket_transform"), Omitted);
	TestTrue(TEXT("omitted transform update is a no-op"), OmittedResult.bSuccess);
	if (OmittedResult.bSuccess && OmittedResult.Data.IsValid())
	{
		bool bChanged = true;
		TestTrue(TEXT("omitted transform reports changed false"), OmittedResult.Data->TryGetBoolField(TEXT("changed"), bChanged) && !bChanged);
	}

	TSharedPtr<FJsonObject> Partial = SetSocketTransformParams(AssetPath, SocketSelector(0, TEXT("Cortex_Transform"), TEXT("hand_r")), SocketCurrentFingerprint(Add));
	Partial->SetObjectField(TEXT("rotation"), RotatorJson(FRotator(1.0, 2.0, 3.0)));
	FCortexCommandResult Updated = Router.Execute(TEXT("anim.set_socket_transform"), Partial);
	TestTrue(TEXT("partial rotation update succeeds"), Updated.bSuccess);
	if (Updated.bSuccess)
	{
		const TSharedPtr<FJsonObject> UpdatedLocation = SocketAfter(Updated)->GetObjectField(TEXT("location"));
		const TSharedPtr<FJsonObject> UpdatedRotation = SocketAfter(Updated)->GetObjectField(TEXT("rotation"));
		TestEqual(TEXT("omitted location is retained"), UpdatedLocation->GetNumberField(TEXT("x")), 4.0);
		TestEqual(TEXT("rotation pitch readback"), UpdatedRotation->GetNumberField(TEXT("pitch")), 1.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSocketAuthoringSaveReloadTest,
	"Cortex.Animation.SocketAuthoring.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSocketAuthoringSaveReloadTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexSocketTestAssetGuard Guard;
	USkeleton* Skeleton = CreateSocketTestAsset(*this, Guard, AssetPath);
	if (Skeleton == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateSocketRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_SaveReload"), TEXT("hand_r"), FVector(7.0, 8.0, 9.0), SocketFingerprintFor(Skeleton), false, true));
	TestTrue(TEXT("saved socket add succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		return false;
	}
	TestFalse(TEXT("saved socket leaves package clean"), Skeleton->GetPackage()->IsDirty());

	Skeleton->ClearFlags(RF_Public | RF_Standalone);
	Skeleton->MarkAsGarbage();
	Guard.Skeleton = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	USkeleton* Reloaded = LoadObject<USkeleton>(nullptr, *AssetPath);
	Guard.Skeleton = Reloaded;
	TestNotNull(TEXT("saved skeleton reloads"), Reloaded);
	if (Reloaded == nullptr)
	{
		return false;
	}

	TestEqual(TEXT("saved socket reloads"), SocketIndexByName(Reloaded, FName(TEXT("Cortex_SaveReload"))), 0);
	FCortexCommandResult Inspect = Router.Execute(TEXT("anim.get_skeleton_info"), SkeletonInfoParams(AssetPath));
	TestTrue(TEXT("reloaded socket inspects"), Inspect.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSocketAuthoringUndoRedoTest,
	"Cortex.Animation.SocketAuthoring.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSocketAuthoringUndoRedoTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Socket Undo Setup")));
	FString AssetPath;
	FCortexSocketTestAssetGuard Guard;
	USkeleton* Skeleton = CreateSocketTestAsset(*this, Guard, AssetPath);
	if (Skeleton == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateSocketRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_socket"),
		AddSocketParams(AssetPath, TEXT("Cortex_Undo"), TEXT("hand_r"), FVector::ZeroVector, SocketFingerprintFor(Skeleton)));
	TestTrue(TEXT("undo add succeeds"), Add.bSuccess);
	TestEqual(TEXT("add created one socket"), Skeleton->Sockets.Num(), 1);
	TestTrue(TEXT("undo add transaction succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo add removes socket"), Skeleton->Sockets.Num(), 0);
	TestTrue(TEXT("redo add transaction succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo add restores socket"), Skeleton->Sockets.Num(), 1);

	TSharedPtr<FJsonObject> Update = SetSocketTransformParams(AssetPath, SocketSelector(0, TEXT("Cortex_Undo"), TEXT("hand_r")), SocketFingerprintFor(Skeleton));
	Update->SetObjectField(TEXT("location"), VectorJson(FVector(10.0, 11.0, 12.0)));
	FCortexCommandResult Updated = Router.Execute(TEXT("anim.set_socket_transform"), Update);
	TestTrue(TEXT("undo update succeeds"), Updated.bSuccess);
	TestTrue(TEXT("updated location applied"), Skeleton->Sockets[0]->RelativeLocation.Equals(FVector(10.0, 11.0, 12.0)));
	TestTrue(TEXT("undo update transaction succeeds"), GEditor->UndoTransaction());
	TestTrue(TEXT("undo update restores location"), Skeleton->Sockets[0]->RelativeLocation.Equals(FVector::ZeroVector));
	TestTrue(TEXT("redo update transaction succeeds"), GEditor->RedoTransaction());
	TestTrue(TEXT("redo update restores new location"), Skeleton->Sockets[0]->RelativeLocation.Equals(FVector(10.0, 11.0, 12.0)));

	USkeletalMeshSocket* RemovedSocket = Skeleton->Sockets[0];
	FCortexCommandResult Remove = Router.Execute(
		TEXT("anim.remove_socket"),
		RemoveSocketParams(AssetPath, SocketSelector(0, TEXT("Cortex_Undo"), TEXT("hand_r")), SocketFingerprintFor(Skeleton)));
	TestTrue(TEXT("undo remove succeeds"), Remove.bSuccess);
	TestEqual(TEXT("remove deletes socket"), Skeleton->Sockets.Num(), 0);
	TestTrue(TEXT("undo remove transaction succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo remove restores socket"), Skeleton->Sockets.Num(), 1);
	TestTrue(TEXT("undo remove restores the same socket object"), Skeleton->Sockets[0] == RemovedSocket);
	TestTrue(TEXT("undo remove restores socket transform"), Skeleton->Sockets[0]->RelativeLocation.Equals(FVector(10.0, 11.0, 12.0)));
	TestTrue(TEXT("redo remove transaction succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo remove deletes socket"), Skeleton->Sockets.Num(), 0);

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Socket Undo Cleanup")));
	return true;
}
