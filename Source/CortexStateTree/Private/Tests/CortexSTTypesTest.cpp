#include "Misc/AutomationTest.h"
#include "CortexSTTypes.h"
#include "CortexEditorUtils.h"
#include "CortexStateTreeTestUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace CortexSTTypesTest
{
	struct FScopedMountedRoot
	{
		FString Root;
		FString PhysicalDir;

		explicit FScopedMountedRoot(const FString& InRoot)
			: Root(InRoot)
		{
			PhysicalDir = FPaths::ProjectSavedDir() / TEXT("CortexStateTreeMountedPathTests") / Root.RightChop(1);
			IFileManager::Get().MakeDirectory(*PhysicalDir, true);
			FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			FCortexEditorUtils::AddTestWritableContentRoot(Root);
		}

		~FScopedMountedRoot()
		{
			FCortexEditorUtils::RemoveTestWritableContentRoot(Root);
			FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSTValidateWritablePackageTest,
	"Cortex.StateTree.Types.ValidateWritablePackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSTValidateWritablePackageTest::RunTest(const FString& Parameters)
{
	using namespace CortexSTTypesTest;

	FString PackageName;
	FCortexCommandResult Error;

	TestEqual(TEXT("Relative asset path normalizes under /Game"),
		CortexST::NormalizeAssetPath(TEXT("StateTrees/ST_Test")),
		TEXT("/Game/StateTrees/ST_Test"));

	TestTrue(TEXT("Relative /Game package path is writable"),
		CortexST::ValidateWritablePackage(TEXT("StateTrees/ST_Test"), PackageName, Error));
	TestEqual(TEXT("Relative path resolves package name"),
		PackageName,
		TEXT("/Game/StateTrees/ST_Test"));

	PackageName.Reset();
	Error = FCortexCommandResult();
	TestFalse(TEXT("/Engine package path is rejected for writes"),
		CortexST::ValidateWritablePackage(TEXT("/Engine/BasicShapes/Cube"), PackageName, Error));
	TestEqual(TEXT("/Engine rejection uses invalid field"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("/Engine rejection names original root"), Error.ErrorMessage.Contains(TEXT("/Engine")));

	PackageName.Reset();
	Error = FCortexCommandResult();
	TestFalse(TEXT("Unknown mounted root is rejected for writes"),
		CortexST::ValidateWritablePackage(TEXT("/MissingPlugin/StateTrees/ST_Test"), PackageName, Error));
	TestEqual(TEXT("Unknown root rejection uses invalid field"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Unknown root rejection names original root"), Error.ErrorMessage.Contains(TEXT("/MissingPlugin")));

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MountRoot = FString::Printf(TEXT("/CortexSTMount%s"), *Suffix);
	FScopedMountedRoot Mount(MountRoot);

	PackageName.Reset();
	Error = FCortexCommandResult();
	const FString MountedPath = MountRoot / TEXT("StateTrees/ST_Mounted");
	TestTrue(TEXT("Explicit mounted project root is writable"),
		CortexST::ValidateWritablePackage(MountedPath, PackageName, Error));
	TestEqual(TEXT("Mounted path preserves package root"),
		PackageName,
		MountedPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSTExpectedFingerprintRequiredTest,
	"Cortex.StateTree.Types.ExpectedFingerprintRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSTExpectedFingerprintRequiredTest::RunTest(const FString& Parameters)
{
	UPackage* Asset = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexSTFingerprint_%s"), *CortexStateTreeTest::MakeSuffix()));
	TestNotNull(TEXT("Transient asset created"), Asset);

	FCortexCommandResult Error;
	TestFalse(TEXT("Missing expected_fingerprint is rejected"),
		CortexST::CheckExpectedFingerprint(Asset, MakeShared<FJsonObject>(), Error));
	TestEqual(TEXT("Missing expected_fingerprint uses stale precondition"),
		Error.ErrorCode,
		CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("Missing expected_fingerprint message is explicit"),
		Error.ErrorMessage.Contains(TEXT("requires expected_fingerprint")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(Asset));
	Error = FCortexCommandResult();
	TestTrue(TEXT("Matching fingerprint passes"),
		CortexST::CheckExpectedFingerprint(Asset, Params, Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSTValidationPayloadTest,
	"Cortex.StateTree.Types.ValidationPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSTValidationPayloadTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Payload = CortexST::MakeValidationPayload(
		false,
		{ TEXT("Missing root state"), TEXT("State name duplicated") },
		{ TEXT("Transition priority defaulted") });

	TestTrue(TEXT("Payload allocated"), Payload.IsValid());
	if (!Payload.IsValid())
	{
		return false;
	}

	bool bValid = true;
	TestTrue(TEXT("Payload includes valid flag"), Payload->TryGetBoolField(TEXT("valid"), bValid));
	TestFalse(TEXT("Valid flag matches input"), bValid);

	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("Payload includes errors array"), Payload->TryGetArrayField(TEXT("errors"), Errors) && Errors != nullptr);
	if (Errors)
	{
		TestEqual(TEXT("Error array length matches input"), Errors->Num(), 2);
		TestEqual(TEXT("First error matches input"), (*Errors)[0]->AsString(), FString(TEXT("Missing root state")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	TestTrue(TEXT("Payload includes warnings array"), Payload->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr);
	if (Warnings)
	{
		TestEqual(TEXT("Warning array length matches input"), Warnings->Num(), 1);
		TestEqual(TEXT("First warning matches input"), (*Warnings)[0]->AsString(), FString(TEXT("Transition priority defaulted")));
	}

	return true;
}
