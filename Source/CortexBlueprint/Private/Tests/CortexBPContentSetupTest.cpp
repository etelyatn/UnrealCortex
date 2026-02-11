#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EdGraphSchema_K2.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPContentSetupTest,
	"Cortex.Blueprint.ContentSetup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

namespace
{
	bool CreateAndSaveBlueprint(
		FAutomationTestBase* Test,
		const FString& PackagePath,
		const FString& AssetName,
		UClass* ParentClass,
		EBlueprintType BPType,
		UBlueprint*& OutBP)
	{
		// Skip if already exists
		UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *(PackagePath / AssetName));
		if (Existing)
		{
			OutBP = Existing;
			return true;
		}

		UPackage* Package = CreatePackage(*(PackagePath / AssetName));
		if (!Package)
		{
			Test->AddError(FString::Printf(TEXT("Failed to create package: %s"), *(PackagePath / AssetName)));
			return false;
		}

		OutBP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPType,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None
		);

		if (!OutBP)
		{
			Test->AddError(FString::Printf(TEXT("Failed to create Blueprint: %s"), *AssetName));
			return false;
		}

		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(OutBP);

		FString Filename = FPackageName::LongPackageNameToFilename(
			PackagePath / AssetName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		if (!UPackage::SavePackage(Package, OutBP, *Filename, SaveArgs))
		{
			Test->AddError(FString::Printf(TEXT("Failed to save package: %s"), *Filename));
			return false;
		}

		return true;
	}
}

bool FCortexBPContentSetupTest::RunTest(const FString& Parameters)
{
	const FString BasePath = TEXT("/Game/Blueprints");

	// BP_SimpleActor — basic Actor BP
	{
		UBlueprint* BP = nullptr;
		bool bCreated = CreateAndSaveBlueprint(
			this, BasePath, TEXT("BP_SimpleActor"),
			AActor::StaticClass(), BPTYPE_Normal, BP);
		TestTrue(TEXT("BP_SimpleActor created"), bCreated);
		TestNotNull(TEXT("BP_SimpleActor is valid"), BP);
	}

	// BP_ComplexActor — Actor BP with variables and a function
	{
		UBlueprint* BP = nullptr;
		bool bCreated = CreateAndSaveBlueprint(
			this, BasePath, TEXT("BP_ComplexActor"),
			AActor::StaticClass(), BPTYPE_Normal, BP);
		TestTrue(TEXT("BP_ComplexActor created"), bCreated);
		TestNotNull(TEXT("BP_ComplexActor is valid"), BP);

		if (BP)
		{
			// Add variables if they don't already exist
			auto HasVariable = [&BP](const FString& Name) -> bool
			{
				for (const FBPVariableDescription& Var : BP->NewVariables)
				{
					if (Var.VarName == FName(*Name))
					{
						return true;
					}
				}
				return false;
			};

			if (!HasVariable(TEXT("Health")))
			{
				FEdGraphPinType FloatType;
				FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
				FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
				FBlueprintEditorUtils::AddMemberVariable(BP, FName("Health"), FloatType);
			}

			if (!HasVariable(TEXT("DisplayName")))
			{
				FEdGraphPinType StringType;
				StringType.PinCategory = UEdGraphSchema_K2::PC_String;
				FBlueprintEditorUtils::AddMemberVariable(BP, FName("DisplayName"), StringType);
			}

			if (!HasVariable(TEXT("bIsActive")))
			{
				FEdGraphPinType BoolType;
				BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
				FBlueprintEditorUtils::AddMemberVariable(BP, FName("bIsActive"), BoolType);
			}

			// Add CalculateDamage function if it doesn't exist
			bool bHasFunc = false;
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == TEXT("CalculateDamage"))
				{
					bHasFunc = true;
					break;
				}
			}

			if (!bHasFunc)
			{
				UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
					BP, FName("CalculateDamage"),
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, false, nullptr);
			}

			FKismetEditorUtilities::CompileBlueprint(BP);

			// Re-save with variables and function
			UPackage* Package = BP->GetOutermost();
			FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Standalone;
			UPackage::SavePackage(Package, BP, *Filename, SaveArgs);
		}
	}

	// BP_SimpleComponent — basic ActorComponent BP
	{
		UBlueprint* BP = nullptr;
		bool bCreated = CreateAndSaveBlueprint(
			this, BasePath, TEXT("BP_SimpleComponent"),
			UActorComponent::StaticClass(), BPTYPE_Normal, BP);
		TestTrue(TEXT("BP_SimpleComponent created"), bCreated);
		TestNotNull(TEXT("BP_SimpleComponent is valid"), BP);
	}

	// BP_SimpleFunctionLibrary — basic FunctionLibrary BP
	{
		UBlueprint* BP = nullptr;
		bool bCreated = CreateAndSaveBlueprint(
			this, BasePath, TEXT("BP_SimpleFunctionLibrary"),
			UBlueprintFunctionLibrary::StaticClass(), BPTYPE_FunctionLibrary, BP);
		TestTrue(TEXT("BP_SimpleFunctionLibrary created"), bCreated);
		TestNotNull(TEXT("BP_SimpleFunctionLibrary is valid"), BP);
	}

	// BP_SimpleInterface — basic Interface BP
	{
		UBlueprint* BP = nullptr;
		bool bCreated = CreateAndSaveBlueprint(
			this, BasePath, TEXT("BP_SimpleInterface"),
			UInterface::StaticClass(), BPTYPE_Interface, BP);
		TestTrue(TEXT("BP_SimpleInterface created"), bCreated);
		TestNotNull(TEXT("BP_SimpleInterface is valid"), BP);
	}

	// Verify all assets exist on disk
	FString ContentDir = FPaths::ProjectContentDir() / TEXT("Blueprints");
	TestTrue(TEXT("BP_SimpleActor.uasset exists"),
		FPaths::FileExists(ContentDir / TEXT("BP_SimpleActor.uasset")));
	TestTrue(TEXT("BP_ComplexActor.uasset exists"),
		FPaths::FileExists(ContentDir / TEXT("BP_ComplexActor.uasset")));
	TestTrue(TEXT("BP_SimpleComponent.uasset exists"),
		FPaths::FileExists(ContentDir / TEXT("BP_SimpleComponent.uasset")));
	TestTrue(TEXT("BP_SimpleFunctionLibrary.uasset exists"),
		FPaths::FileExists(ContentDir / TEXT("BP_SimpleFunctionLibrary.uasset")));
	TestTrue(TEXT("BP_SimpleInterface.uasset exists"),
		FPaths::FileExists(ContentDir / TEXT("BP_SimpleInterface.uasset")));

	return true;
}
