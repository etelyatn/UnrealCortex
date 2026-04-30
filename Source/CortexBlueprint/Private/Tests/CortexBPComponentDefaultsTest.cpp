#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsTest,
	"Cortex.Blueprint.SetComponentDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_CompDefaultsTest_%s"), *UniqueSuffix);
	const FString TestPath = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CompDefaults_%s"), *UniqueSuffix);
	const FString TestBPPath = FString::Printf(TEXT("%s/%s"), *TestPath, *BlueprintName);

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), BlueprintName);
		CreateParams->SetStringField(TEXT("path"), TestPath);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
		CreateParams->SetStringField(TEXT("parent_class"), TEXT("StaticMeshActor"));
		FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		TestTrue(TEXT("create should succeed"), CreateResult.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("StaticMesh"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		Properties->SetStringField(TEXT("OverrideMaterials[0]"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
		Params->SetObjectField(TEXT("properties"), Properties);

		FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
		TestTrue(TEXT("set_component_defaults should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			double PropsSet = 0.0;
			Result.Data->TryGetNumberField(TEXT("properties_set"), PropsSet);
			TestEqual(TEXT("properties_set should be 2"), static_cast<int32>(PropsSet), 2);

			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			bool bHasErrors = Result.Data->TryGetArrayField(TEXT("errors"), Errors)
				&& Errors != nullptr
				&& Errors->Num() > 0;
			TestFalse(TEXT("set_component_defaults should not return errors"), bHasErrors);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));
		Params->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
		TestFalse(TEXT("missing asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: struct properties (FVector RelativeLocation, FRotator RelativeRotation) set via JsonToProperty.
	// Pre-fix this returned errors[0..N] = "Asset not found" because every value was treated as a path.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("X"), 100.0);
		Location->SetNumberField(TEXT("Y"), 200.0);
		Location->SetNumberField(TEXT("Z"), 300.0);
		Properties->SetObjectField(TEXT("RelativeLocation"), Location);

		TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
		Rotation->SetNumberField(TEXT("Pitch"), 0.0);
		Rotation->SetNumberField(TEXT("Yaw"), 90.0);
		Rotation->SetNumberField(TEXT("Roll"), 0.0);
		Properties->SetObjectField(TEXT("RelativeRotation"), Rotation);

		Params->SetObjectField(TEXT("properties"), Properties);

		FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
		TestTrue(TEXT("set_component_defaults with struct values should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			double PropsSet = 0.0;
			Result.Data->TryGetNumberField(TEXT("properties_set"), PropsSet);
			TestEqual(TEXT("properties_set should be 2 (Location + Rotation)"),
				static_cast<int32>(PropsSet), 2);

			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			bool bHasErrors = Result.Data->TryGetArrayField(TEXT("errors"), Errors)
				&& Errors != nullptr
				&& Errors->Num() > 0;
			TestFalse(TEXT("struct properties should not produce 'Asset not found' errors"), bHasErrors);
		}
	}

	// Test: scalar bool property — pre-fix this also failed with "Asset not found: true".
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetBoolField(TEXT("bVisible"), false);
		Params->SetObjectField(TEXT("properties"), Properties);

		FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
		TestTrue(TEXT("set_component_defaults with bool value should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			double PropsSet = 0.0;
			Result.Data->TryGetNumberField(TEXT("properties_set"), PropsSet);
			TestEqual(TEXT("properties_set should be 1 (bVisible)"),
				static_cast<int32>(PropsSet), 1);

			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			bool bHasErrors = Result.Data->TryGetArrayField(TEXT("errors"), Errors)
				&& Errors != nullptr
				&& Errors->Num() > 0;
			TestFalse(TEXT("bool property should not produce 'Asset not found' error"), bHasErrors);
		}
	}

	// Test: type mismatch — passing a string to an FVector property should error gracefully (not crash).
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("component_name"), TEXT("StaticMeshComponent0"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("RelativeLocation"), TEXT("not_a_vector"));
		Params->SetObjectField(TEXT("properties"), Properties);

		FCortexCommandResult Result = Handler.Execute(TEXT("set_component_defaults"), Params);
		// Top-level call still succeeds (per-property errors accumulate in errors[]).
		TestTrue(TEXT("type mismatch should still return success at command level"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			double PropsSet = 0.0;
			Result.Data->TryGetNumberField(TEXT("properties_set"), PropsSet);
			TestEqual(TEXT("properties_set should be 0 on type mismatch"),
				static_cast<int32>(PropsSet), 0);

			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			bool bHasErrors = Result.Data->TryGetArrayField(TEXT("errors"), Errors)
				&& Errors != nullptr
				&& Errors->Num() > 0;
			TestTrue(TEXT("type mismatch should populate errors array"), bHasErrors);
		}
	}

	const FString PackagePath = FPackageName::ObjectPathToPackageName(TestBPPath);
	if (FindPackage(nullptr, *PackagePath) || FPackageName::DoesPackageExist(PackagePath))
	{
		if (UObject* BP = LoadObject<UBlueprint>(nullptr, *TestBPPath))
		{
			BP->GetOutermost()->MarkAsGarbage();
		}
	}

	return true;
}
