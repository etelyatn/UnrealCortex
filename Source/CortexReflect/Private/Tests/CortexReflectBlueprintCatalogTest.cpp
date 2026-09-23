#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "Operations/CortexReflectOps.h"
#include "UObject/CoreRedirects.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectBlueprintCatalogTest,
	"Cortex.Reflect.BlueprintCatalog.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectBlueprintCatalogTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("blueprint_catalog"), Params);
	TestTrue(TEXT("blueprint_catalog should succeed for AActor"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return true;
	}

	bool bComplete = false;
	TestTrue(TEXT("Catalog should report completeness"),
		Result.Data->TryGetBoolField(TEXT("complete"), bComplete));
	TestTrue(TEXT("Catalog should be complete after initial gather"), bComplete);
	int32 InvalidAssetCount = INDEX_NONE;
	TestTrue(TEXT("Catalog should report the invalid asset count"),
		Result.Data->TryGetNumberField(TEXT("invalid_asset_count"), InvalidAssetCount));
	TestEqual(TEXT("Valid saved assets should not produce tag diagnostics"), InvalidAssetCount, 0);
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	TestTrue(TEXT("Catalog should return structured diagnostics"),
		Result.Data->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
	if (Diagnostics)
	{
		TestEqual(TEXT("Valid saved assets should have no diagnostics"), Diagnostics->Num(), 0);
	}

	const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
	TestTrue(TEXT("Catalog should contain class records"),
		Result.Data->TryGetArrayField(TEXT("classes"), Classes));
	if (!Classes)
	{
		return true;
	}

	bool bFoundSimpleActor = false;
	TSet<FString> GeneratedClassPaths;
	FString PreviousSortKey;
	for (int32 Index = 0; Index < Classes->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> ClassData = (*Classes)[Index]->AsObject();
		TestTrue(FString::Printf(TEXT("Class record %d should be an object"), Index),
			ClassData.IsValid());
		if (!ClassData.IsValid())
		{
			continue;
		}

		FString Name;
		FString GeneratedClassPath;
		FString ParentName;
		FString ParentClassPath;
		FString AssetPath;
		TestTrue(TEXT("Record should contain name"),
			ClassData->TryGetStringField(TEXT("name"), Name));
		TestTrue(TEXT("Record should contain generated_class_path"),
			ClassData->TryGetStringField(TEXT("generated_class_path"), GeneratedClassPath));
		TestTrue(TEXT("Record should contain parent_name"),
			ClassData->TryGetStringField(TEXT("parent_name"), ParentName));
		TestTrue(TEXT("Record should contain parent_class_path"),
			ClassData->TryGetStringField(TEXT("parent_class_path"), ParentClassPath));
		TestTrue(TEXT("Record should contain asset_path"),
			ClassData->TryGetStringField(TEXT("asset_path"), AssetPath));
		TestTrue(TEXT("Actor Blueprint records should include a parent"),
			!ParentName.IsEmpty() && !ParentClassPath.IsEmpty());

		TestFalse(TEXT("Generated class path should not contain SKEL_"),
			GeneratedClassPath.Contains(TEXT("SKEL_")));
		TestFalse(TEXT("Generated class path should not contain REINST_"),
			GeneratedClassPath.Contains(TEXT("REINST_")));
		TestTrue(TEXT("Generated class paths should be unique"),
			!GeneratedClassPaths.Contains(GeneratedClassPath));
		GeneratedClassPaths.Add(GeneratedClassPath);

		const FString SortKey = AssetPath.ToLower() + TEXT("|") + GeneratedClassPath;
		if (!PreviousSortKey.IsEmpty())
		{
			TestTrue(TEXT("Catalog records should be sorted by asset and class path"),
				PreviousSortKey.Compare(SortKey, ESearchCase::CaseSensitive) <= 0);
		}
		PreviousSortKey = SortKey;

		if (AssetPath.Contains(TEXT("/Game/Blueprints/BP_SimpleActor")))
		{
			bFoundSimpleActor = true;
		}
	}

	TestTrue(TEXT("Saved BP_SimpleActor should appear in the project catalog"), bFoundSimpleActor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectBlueprintCatalogTagParsingTest,
	"Cortex.Reflect.BlueprintCatalog.TagParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectBlueprintCatalogTagParsingTest::RunTest(const FString& Parameters)
{
	FCortexBlueprintCatalogClassPaths Parsed;
	TArray<FString> InvalidFields;
	const bool bValid = FCortexReflectOps::ParseBlueprintCatalogClassPaths(
		TEXT("BlueprintGeneratedClass'/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C'"),
		TEXT("Class'/Script/Engine.Actor'"),
		TEXT("Class'/Script/Engine.Actor'"),
		Parsed,
		InvalidFields
	);
	TestTrue(TEXT("Valid export-form class tags should parse"), bValid);
	TestEqual(TEXT("Generated class path should be normalized"),
		Parsed.GeneratedClassPath.ToString(), TEXT("/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C"));
	TestEqual(TEXT("Parent class path should be normalized"),
		Parsed.ParentClassPath.ToString(), TEXT("/Script/Engine.Actor"));
	TestEqual(TEXT("Native parent class path should be normalized"),
		Parsed.NativeParentClassPath.ToString(), TEXT("/Script/Engine.Actor"));

	FCortexBlueprintCatalogClassPaths Malformed;
	TArray<FString> MalformedFields;
	const bool bMalformedValid = FCortexReflectOps::ParseBlueprintCatalogClassPaths(
		TEXT(""),
		TEXT("Class'/Script/Engine.Actor'"),
		TEXT("Class'/Script/Engine.Actor'"),
		Malformed,
		MalformedFields
	);
	TestFalse(TEXT("Missing generated class tag should be rejected"), bMalformedValid);
	TestTrue(TEXT("Missing generated class tag should be diagnosed"),
		MalformedFields.Contains(TEXT("GeneratedClassPath")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectBlueprintCatalogIncompleteMembershipTest,
	"Cortex.Reflect.BlueprintCatalog.IncompleteMembership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectBlueprintCatalogIncompleteMembershipTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Missing parent metadata leaves membership unresolved"),
		FCortexReflectOps::HasUnresolvedBlueprintCatalogMembership(false, false, false));
	TestFalse(TEXT("A valid parent outside the requested tree proves an asset is unrelated"),
		FCortexReflectOps::HasUnresolvedBlueprintCatalogMembership(false, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectBlueprintCatalogCoreRedirectTest,
	"Cortex.Reflect.BlueprintCatalog.CoreRedirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectBlueprintCatalogCoreRedirectTest::RunTest(const FString& Parameters)
{
	const FCoreRedirect Redirect(
		ECoreRedirectFlags::Type_Class,
		TEXT("/Script/CortexOldCatalogTest.OldParent"),
		TEXT("/Script/CortexNewCatalogTest.NewParent")
	);
	const TArray<FCoreRedirect> Redirects = { Redirect };
	const FString Source = TEXT("CortexReflect BlueprintCatalog automation test");
	const bool bAdded = FCoreRedirects::AddRedirectList(Redirects, Source);
	TestTrue(TEXT("Test class redirect should register"), bAdded);
	if (!bAdded)
	{
		return true;
	}

	const FTopLevelAssetPath OldPath(FName(TEXT("/Script/CortexOldCatalogTest")), FName(TEXT("OldParent")));
	const FTopLevelAssetPath Resolved = FCortexReflectOps::ResolveBlueprintCatalogClassRedirect(OldPath);
	FCoreRedirects::RemoveRedirectList(Redirects, Source);
	TestEqual(TEXT("Class redirect should normalize stale Blueprint parent tags"),
		Resolved.ToString(), TEXT("/Script/CortexNewCatalogTest.NewParent"));
	return true;
}
