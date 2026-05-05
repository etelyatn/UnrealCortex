#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace CortexBPComponentDefaultsTest
{
	void MarkBlueprintPackageAsGarbage(const FString& AssetPath);

	struct FFixture
	{
		FString BlueprintName;
		FString Dir;
		FString AssetPath;
		FString ComponentName = TEXT("OwnedStaticMesh");
		FCortexBPCommandHandler Handler;
		bool bIsValid = false;
		FString SetupError;

		FFixture()
		{
			const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
			BlueprintName = FString::Printf(TEXT("BP_CompDefaultsTest_%s"), *UniqueSuffix);
			Dir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CompDefaults_%s"), *UniqueSuffix);
			AssetPath = FString::Printf(TEXT("%s/%s"), *Dir, *BlueprintName);

			TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
			CreateParams->SetStringField(TEXT("name"), BlueprintName);
			CreateParams->SetStringField(TEXT("path"), Dir);
			CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
			FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
			if (!CreateResult.bSuccess)
			{
				SetupError = FString::Printf(TEXT("Failed to create fixture Blueprint: %s"), *CreateResult.ErrorMessage);
				return;
			}

			TSharedPtr<FJsonObject> AddComponentParams = MakeShared<FJsonObject>();
			AddComponentParams->SetStringField(TEXT("asset_path"), AssetPath);
			AddComponentParams->SetStringField(TEXT("component_class"), TEXT("StaticMeshComponent"));
			AddComponentParams->SetStringField(TEXT("component_name"), ComponentName);
			AddComponentParams->SetBoolField(TEXT("compile"), true);
			FCortexCommandResult AddComponentResult = Handler.Execute(TEXT("add_scs_component"), AddComponentParams);
			if (!AddComponentResult.bSuccess)
			{
				SetupError = FString::Printf(TEXT("Failed to add fixture SCS component: %s"), *AddComponentResult.ErrorMessage);
				return;
			}
			if (AddComponentResult.Data.IsValid())
			{
				AddComponentResult.Data->TryGetStringField(TEXT("variable_name"), ComponentName);
			}
			bIsValid = true;
		}

		~FFixture()
		{
			MarkBlueprintPackageAsGarbage(AssetPath);
		}

		FCortexCommandResult SetComponentDefaults(const TSharedPtr<FJsonObject>& Properties, bool bCompile = true, bool bSave = false)
		{
			if (!bIsValid)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, SetupError);
			}

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetStringField(TEXT("component_name"), ComponentName);
			Params->SetObjectField(TEXT("properties"), Properties);
			Params->SetBoolField(TEXT("compile"), bCompile);
			Params->SetBoolField(TEXT("save"), bSave);
			return Handler.Execute(TEXT("set_component_defaults"), Params);
		}
	};

	void MarkBlueprintPackageAsGarbage(const FString& AssetPath)
	{
		const FString PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
		if (UObject* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
		{
			BP->GetOutermost()->MarkAsGarbage();
		}
		if (UPackage* Package = FindPackage(nullptr, *PackagePath))
		{
			Package->MarkAsGarbage();
		}
	}

	struct FScopedBlueprintCleanup
	{
		FString AssetPath;

		explicit FScopedBlueprintCleanup(FString InAssetPath)
			: AssetPath(MoveTemp(InAssetPath))
		{
		}

		~FScopedBlueprintCleanup()
		{
			MarkBlueprintPackageAsGarbage(AssetPath);
		}
	};

	TSharedPtr<FJsonObject> MakeVector(double X, double Y, double Z)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("X"), X);
		Value->SetNumberField(TEXT("Y"), Y);
		Value->SetNumberField(TEXT("Z"), Z);
		return Value;
	}

	TSharedPtr<FJsonObject> MakeRotator(double Pitch, double Yaw, double Roll)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("Pitch"), Pitch);
		Value->SetNumberField(TEXT("Yaw"), Yaw);
		Value->SetNumberField(TEXT("Roll"), Roll);
		return Value;
	}

	UStaticMeshComponent* FindStaticMeshTemplate(UBlueprint* Blueprint, const FString& ComponentName)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return nullptr;
		}

		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == ComponentName)
			{
				return Cast<UStaticMeshComponent>(Node->ComponentTemplate);
			}
		}
		return nullptr;
	}

	int32 GetPropertiesSet(const FCortexCommandResult& Result)
	{
		double PropertiesSet = 0.0;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetNumberField(TEXT("properties_set"), PropertiesSet);
		}
		return static_cast<int32>(PropertiesSet);
	}

	FString JoinErrorStrings(const FCortexCommandResult& Result)
	{
		FString JoinedErrors;
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("errors"), Errors) || Errors == nullptr)
		{
			return JoinedErrors;
		}

		for (const TSharedPtr<FJsonValue>& ErrorValue : *Errors)
		{
			if (!ErrorValue.IsValid())
			{
				continue;
			}

			if (!JoinedErrors.IsEmpty())
			{
				JoinedErrors += TEXT("\n");
			}
			JoinedErrors += ErrorValue->AsString();
		}
		return JoinedErrors;
	}

	bool DoesAssetPackageFileExist(const FString& AssetPath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			PackageFilename,
			FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}

		return IFileManager::Get().FileExists(*PackageFilename);
	}

	bool ValidateFixture(FAutomationTestBase& Test, const FFixture& Fixture)
	{
		if (!Test.TestTrue(TEXT("fixture setup succeeds"), Fixture.bIsValid))
		{
			if (!Fixture.SetupError.IsEmpty())
			{
				Test.AddError(Fixture.SetupError);
			}
			return false;
		}
		return true;
	}

	UPackage* FindLoadedPackageForAsset(const FString& AssetPath)
	{
		return FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(AssetPath));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsObjectRefsTest,
	"Cortex.Blueprint.SetComponentDefaults.ObjectRefs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsObjectRefsTest::RunTest(const FString& Parameters)
{
	CortexBPComponentDefaultsTest::FFixture Fixture;
	if (!CortexBPComponentDefaultsTest::ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("StaticMesh"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Properties->SetStringField(TEXT("OverrideMaterials[0]"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties, true, true);
	TestTrue(TEXT("set_component_defaults succeeds"), Result.bSuccess);
	TestEqual(TEXT("properties_set is 2"), CortexBPComponentDefaultsTest::GetPropertiesSet(Result), 2);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = CortexBPComponentDefaultsTest::FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("static mesh template exists"), Template);
	TestNotNull(TEXT("static mesh was set through setter"), Template ? Template->GetStaticMesh().Get() : nullptr);
	TestTrue(TEXT("override material array has slot 0"), Template && Template->OverrideMaterials.Num() > 0);
	TestNotNull(TEXT("override material was set"), Template && Template->OverrideMaterials.Num() > 0 ? Template->OverrideMaterials[0].Get() : nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsJsonValuesTest,
	"Cortex.Blueprint.SetComponentDefaults.JsonValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsJsonValuesTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("RelativeLocation"), MakeVector(100.0, 0.0, 50.0));
	Properties->SetObjectField(TEXT("RelativeRotation"), MakeRotator(0.0, 90.0, 0.0));
	Properties->SetBoolField(TEXT("bVisible"), false);

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties);
	TestTrue(TEXT("JSON-valued properties succeed"), Result.bSuccess);
	TestEqual(TEXT("properties_set is 3"), GetPropertiesSet(Result), 3);
	if (Result.Data.IsValid())
	{
		bool bPartialFailure = true;
		Result.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure);
		TestFalse(TEXT("partial_failure false for clean write"), bPartialFailure);
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("template exists"), Template);
	TestEqual(TEXT("relative location persisted in template"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector(100.0, 0.0, 50.0));
	TestEqual(TEXT("relative yaw persisted in template"), Template ? Template->GetRelativeRotation().Yaw : 0.0, 90.0);
	TestFalse(TEXT("visible flag persisted in template"), Template ? Template->GetVisibleFlag() : true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsRejectsInvalidScalarJsonTest,
	"Cortex.Blueprint.SetComponentDefaults.RejectsInvalidScalarJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsRejectsInvalidScalarJsonTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RelativeLocation = MakeShared<FJsonObject>();
	RelativeLocation->SetStringField(TEXT("X"), TEXT("not a number"));
	RelativeLocation->SetNumberField(TEXT("Y"), 0.0);
	RelativeLocation->SetNumberField(TEXT("Z"), 50.0);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetField(TEXT("bVisible"), MakeShared<FJsonValueString>(TEXT("not a bool")));
	Properties->SetObjectField(TEXT("RelativeLocation"), RelativeLocation);
	Properties->SetNumberField(TEXT("Mobility"), 999.0);

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties, false, false);
	TestTrue(TEXT("invalid scalar JSON is reported as partial failure"), Result.bSuccess);
	TestEqual(TEXT("invalid scalar JSON sets no properties"), GetPropertiesSet(Result), 0);

	bool bPartialFailure = false;
	TestTrue(TEXT("partial_failure present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("partial_failure true"), bPartialFailure);

	const FString JoinedErrors = JoinErrorStrings(Result);
	TestTrue(TEXT("bVisible error is reported"), JoinedErrors.Contains(TEXT("bVisible")));
	TestTrue(TEXT("RelativeLocation error is reported"), JoinedErrors.Contains(TEXT("RelativeLocation")));
	TestTrue(TEXT("Mobility error is reported"), JoinedErrors.Contains(TEXT("Mobility")));

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("template exists"), Template);
	TestTrue(TEXT("invalid bool write does not mutate visibility"), Template ? Template->GetVisibleFlag() : false);
	TestEqual(TEXT("invalid struct write does not mutate location"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector::ZeroVector);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsSingleStaleFingerprintTest,
	"Cortex.Blueprint.SetComponentDefaults.SingleStaleFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsSingleStaleFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bVisible"), false);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
	Params->SetStringField(TEXT("component_name"), Fixture.ComponentName);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeShared<FJsonObject>());

	FCortexCommandResult Result = Fixture.Handler.Execute(TEXT("set_component_defaults"), Params);
	TestFalse(TEXT("single stale fingerprint rejects write"), Result.bSuccess);
	TestEqual(TEXT("single stale fingerprint error code"), Result.ErrorCode, CortexErrorCodes::StalePrecondition);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("template exists"), Template);
	TestTrue(TEXT("rejected stale write does not mutate template"), Template ? Template->GetVisibleFlag() : false);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsPartialFailureTest,
	"Cortex.Blueprint.SetComponentDefaults.PartialFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsPartialFailureTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("RelativeLocation"), TEXT("not a vector"));
	Properties->SetBoolField(TEXT("bVisible"), false);

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties);
	TestTrue(TEXT("per-property failure keeps command success"), Result.bSuccess);
	TestEqual(TEXT("one property set"), GetPropertiesSet(Result), 1);

	bool bPartialFailure = false;
	bool bSaved = true;
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("partial_failure present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("partial_failure true"), bPartialFailure);
	TestTrue(TEXT("saved field present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("saved"), bSaved));
	TestFalse(TEXT("partial failures are not saved"), bSaved);
	TestTrue(TEXT("errors present"), Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 1);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("template exists"), Template);
	TestEqual(TEXT("invalid relative location was not applied"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector::ZeroVector);
	TestFalse(TEXT("valid visible flag was applied"), Template ? Template->GetVisibleFlag() : true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsInvalidObjectPathTest,
	"Cortex.Blueprint.SetComponentDefaults.InvalidObjectPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsInvalidObjectPathTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("StaticMesh"), TEXT("/Game/NoSuchFolder/SM_Missing.SM_Missing"));
	Properties->SetStringField(TEXT("OverrideMaterials[0]"), TEXT("/Game/NoSuchFolder/M_Missing.M_Missing"));

	UPackage* Package = FindLoadedPackageForAsset(Fixture.AssetPath);
	TestNotNull(TEXT("fixture package is loaded"), Package);
	if (Package)
	{
		Package->SetDirtyFlag(false);
	}

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties);
	TestTrue(TEXT("invalid object paths are per-property failures"), Result.bSuccess);
	TestEqual(TEXT("no properties set"), GetPropertiesSet(Result), 0);

	bool bPartialFailure = false;
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("partial_failure present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("partial_failure true"), bPartialFailure);
	TestTrue(TEXT("two errors present"), Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 2);
	const FString ErrorText = JoinErrorStrings(Result);
	TestTrue(TEXT("errors identify missing static mesh path"), ErrorText.Contains(TEXT("/Game/NoSuchFolder/SM_Missing.SM_Missing")));
	TestTrue(TEXT("errors identify missing material path"), ErrorText.Contains(TEXT("/Game/NoSuchFolder/M_Missing.M_Missing")));
	TestTrue(TEXT("errors identify static mesh property or path"), ErrorText.Contains(TEXT("StaticMesh")) || ErrorText.Contains(TEXT("SM_Missing")));
	TestTrue(TEXT("errors identify override material property or path"), ErrorText.Contains(TEXT("OverrideMaterials[0]")) || ErrorText.Contains(TEXT("M_Missing")));
	TestFalse(TEXT("all-invalid defaults do not dirty package"), Package ? Package->IsDirty() : true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsFailedSerializerPreservesPreviousValueTest,
	"Cortex.Blueprint.SetComponentDefaults.FailedSerializerPreservesPreviousValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsFailedSerializerPreservesPreviousValueTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetObjectField(TEXT("RelativeLocation"), MakeVector(9.0, 8.0, 7.0));
	FCortexCommandResult InitialResult = Fixture.SetComponentDefaults(InitialProperties, false, false);
	TestTrue(TEXT("initial location write succeeds"), InitialResult.bSuccess);
	TestEqual(TEXT("initial write sets one property"), GetPropertiesSet(InitialResult), 1);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestNotNull(TEXT("template exists"), Template);
	TestEqual(TEXT("initial relative location applied"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector(9.0, 8.0, 7.0));

	TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
	InvalidProperties->SetStringField(TEXT("RelativeLocation"), TEXT("not a vector"));
	FCortexCommandResult InvalidResult = Fixture.SetComponentDefaults(InvalidProperties, false, false);
	TestTrue(TEXT("failed serializer write keeps command success"), InvalidResult.bSuccess);
	TestEqual(TEXT("failed serializer write sets no properties"), GetPropertiesSet(InvalidResult), 0);

	TestEqual(TEXT("failed serializer write preserves previous value"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector(9.0, 8.0, 7.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsSerializerWarningsAreFailuresTest,
	"Cortex.Blueprint.SetComponentDefaults.SerializerWarningsAreFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsSerializerWarningsAreFailuresTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetStringField(TEXT("OverrideMaterials[0]"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	FCortexCommandResult InitialResult = Fixture.SetComponentDefaults(InitialProperties, false, false);
	TestTrue(TEXT("initial material write succeeds"), InitialResult.bSuccess);
	TestEqual(TEXT("initial material write sets one property"), GetPropertiesSet(InitialResult), 1);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(Blueprint, Fixture.ComponentName);
	TestTrue(TEXT("template has material slot 0"), Template && Template->OverrideMaterials.Num() > 0);
	UObject* PreviousMaterial = Template && Template->OverrideMaterials.Num() > 0 ? Template->OverrideMaterials[0].Get() : nullptr;
	TestNotNull(TEXT("previous material is set"), PreviousMaterial);

	TArray<TSharedPtr<FJsonValue>> BadMaterials;
	BadMaterials.Add(MakeShared<FJsonValueString>(TEXT("/Game/NoSuchFolder/M_Missing.M_Missing")));
	TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
	InvalidProperties->SetArrayField(TEXT("OverrideMaterials"), BadMaterials);

	FCortexCommandResult InvalidResult = Fixture.SetComponentDefaults(InvalidProperties, false, false);
	TestTrue(TEXT("serializer-warning write keeps command success"), InvalidResult.bSuccess);
	TestEqual(TEXT("serializer-warning write sets no properties"), GetPropertiesSet(InvalidResult), 0);

	bool bPartialFailure = false;
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("partial_failure present"), InvalidResult.Data.IsValid() && InvalidResult.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("serializer warnings become partial failure"), bPartialFailure);
	TestTrue(TEXT("errors present"), InvalidResult.Data.IsValid() && InvalidResult.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 1);
	TestTrue(TEXT("error names material property"), JoinErrorStrings(InvalidResult).Contains(TEXT("OverrideMaterials")));

	TestTrue(TEXT("material slot still exists"), Template && Template->OverrideMaterials.Num() > 0);
	TestTrue(
		TEXT("warning failure preserves previous material"),
		Template && Template->OverrideMaterials.Num() > 0 && Template->OverrideMaterials[0].Get() == PreviousMaterial);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsRejectsInstancedReferenceTest,
	"Cortex.Blueprint.SetComponentDefaults.RejectsInstancedReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsRejectsInstancedReferenceTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> EmptyAssetUserData;
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetArrayField(TEXT("AssetUserData"), EmptyAssetUserData);

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties, false, false);
	TestTrue(TEXT("instanced-reference rejection keeps command success"), Result.bSuccess);
	TestEqual(TEXT("instanced-reference property is not set"), GetPropertiesSet(Result), 0);

	bool bPartialFailure = false;
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("partial_failure present"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("instanced-reference property is partial failure"), bPartialFailure);
	TestTrue(TEXT("errors present"), Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() == 1);
	TestTrue(TEXT("error identifies instanced references"), JoinErrorStrings(Result).Contains(TEXT("instanced")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsInheritedTemplateRejectedTest,
	"Cortex.Blueprint.SetComponentDefaults.InheritedTemplateRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsInheritedTemplateRejectedTest::RunTest(const FString& Parameters)
{
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_InheritedCompDefaults_%s"), *UniqueSuffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_InheritedCompDefaults_%s"), *UniqueSuffix);
	const FString AssetPath = FString::Printf(TEXT("%s/%s"), *Dir, *BlueprintName);
	CortexBPComponentDefaultsTest::FScopedBlueprintCleanup Cleanup(AssetPath);
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), BlueprintName);
	CreateParams->SetStringField(TEXT("path"), Dir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	CreateParams->SetStringField(TEXT("parent_class"), TEXT("StaticMeshActor"));
	TestTrue(TEXT("create inherited-component fixture succeeds"), Handler.Execute(TEXT("create"), CreateParams).bSuccess);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bVisible"), false);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));
	Params->SetObjectField(TEXT("properties"), Properties);

	FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
	TestFalse(TEXT("inherited/native component template is rejected"), Result.bSuccess);
	TestEqual(TEXT("inherited rejection uses component error"), Result.ErrorCode, CortexErrorCodes::ComponentNotFound);
	TestTrue(TEXT("inherited rejection is actionable"), Result.ErrorMessage.Contains(TEXT("owned SCS")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsCompileSaveTest,
	"Cortex.Blueprint.SetComponentDefaults.CompileSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsCompileSaveTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPComponentDefaultsTest;

	FFixture Fixture;
	if (!ValidateFixture(*this, Fixture))
	{
		return false;
	}

	const FString AssetPath = Fixture.AssetPath;
	const FString ComponentName = Fixture.ComponentName;
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("RelativeLocation"), MakeVector(12.0, 34.0, 56.0));

	FCortexCommandResult Result = Fixture.SetComponentDefaults(Properties, true, true);
	TestTrue(TEXT("compile/save write succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		bool bCompiled = false;
		bool bSaved = false;
		Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
		Result.Data->TryGetBoolField(TEXT("saved"), bSaved);
		TestTrue(TEXT("compiled true"), bCompiled);
		TestTrue(TEXT("saved true"), bSaved);
	}

	TestTrue(TEXT("saved package file exists"), DoesAssetPackageFileExist(AssetPath));
	MarkBlueprintPackageAsGarbage(AssetPath);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	UBlueprint* ReloadedBlueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	UStaticMeshComponent* Template = FindStaticMeshTemplate(ReloadedBlueprint, ComponentName);
	TestNotNull(TEXT("reloaded template exists"), Template);
	TestEqual(TEXT("saved location survives reload"), Template ? Template->GetRelativeLocation() : FVector::ZeroVector, FVector(12.0, 34.0, 56.0));

	return true;
}
