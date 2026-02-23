#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPComponentDefaultsTest,
	"Cortex.Blueprint.SetComponentDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPComponentDefaultsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestPath = TEXT("/Game/Temp/CortexBPTest_CompDefaults");
	const FString TestBPPath = TestPath + TEXT("/BP_CompDefaultsTest");

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), TEXT("BP_CompDefaultsTest"));
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
