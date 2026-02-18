#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassDetailPropertiesTest,
	"Cortex.Reflect.ClassDetail.Properties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassDetailPropertiesTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	Params->SetBoolField(TEXT("include_inherited"), false);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestTrue(TEXT("class_detail should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray;
		TestTrue(TEXT("Should have properties array"),
			Result.Data->TryGetArrayField(TEXT("properties"), PropertiesArray));

		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray;
		TestTrue(TEXT("Should have functions array"),
			Result.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));

		const TArray<TSharedPtr<FJsonValue>>* InterfacesArray;
		TestTrue(TEXT("Should have interfaces array"),
			Result.Data->TryGetArrayField(TEXT("interfaces"), InterfacesArray));

		FString Parent;
		Result.Data->TryGetStringField(TEXT("parent"), Parent);
		TestEqual(TEXT("Parent should be APawn"), Parent, TEXT("APawn"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassDetailPropertyFlagsTest,
	"Cortex.Reflect.ClassDetail.PropertyFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassDetailPropertyFlagsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	Params->SetBoolField(TEXT("include_inherited"), false);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray;
		if (Result.Data->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject>& FirstProp = (*PropertiesArray)[0]->AsObject();
			TestTrue(TEXT("Property should have name"), FirstProp->HasField(TEXT("name")));
			TestTrue(TEXT("Property should have type"), FirstProp->HasField(TEXT("type")));
			TestTrue(TEXT("Property should have flags"), FirstProp->HasField(TEXT("flags")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassDetailIncludeInheritedTest,
	"Cortex.Reflect.ClassDetail.IncludeInherited",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassDetailIncludeInheritedTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	TSharedPtr<FJsonObject> ParamsOwn = MakeShared<FJsonObject>();
	ParamsOwn->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	ParamsOwn->SetBoolField(TEXT("include_inherited"), false);
	FCortexCommandResult ResultOwn = Handler.Execute(TEXT("class_detail"), ParamsOwn);

	TSharedPtr<FJsonObject> ParamsAll = MakeShared<FJsonObject>();
	ParamsAll->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	ParamsAll->SetBoolField(TEXT("include_inherited"), true);
	FCortexCommandResult ResultAll = Handler.Execute(TEXT("class_detail"), ParamsAll);

	TestTrue(TEXT("Both should succeed"), ResultOwn.bSuccess && ResultAll.bSuccess);

	if (ResultOwn.Data.IsValid() && ResultAll.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* OwnProps;
		const TArray<TSharedPtr<FJsonValue>>* AllProps;
		ResultOwn.Data->TryGetArrayField(TEXT("properties"), OwnProps);
		ResultAll.Data->TryGetArrayField(TEXT("properties"), AllProps);

		if (OwnProps && AllProps)
		{
			TestTrue(TEXT("include_inherited=true should return more properties"),
				AllProps->Num() >= OwnProps->Num());
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassDetailComponentsTest,
	"Cortex.Reflect.ClassDetail.Components",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassDetailComponentsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	Params->SetBoolField(TEXT("include_inherited"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ComponentsArray;
		if (TestTrue(TEXT("Should have components array"),
			Result.Data->TryGetArrayField(TEXT("components"), ComponentsArray)))
		{
			TestTrue(TEXT("ACharacter should have components"), ComponentsArray->Num() > 0);

			if (ComponentsArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>& First = (*ComponentsArray)[0]->AsObject();
				TestTrue(TEXT("Component should have name"), First->HasField(TEXT("name")));
				TestTrue(TEXT("Component should have type"), First->HasField(TEXT("type")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassDetailBlueprintChildCountTest,
	"Cortex.Reflect.ClassDetail.BlueprintChildCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassDetailBlueprintChildCountTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("AActor"));
	Params->SetBoolField(TEXT("include_inherited"), false);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have blueprint_children_count"),
			Result.Data->HasField(TEXT("blueprint_children_count")));
	}

	return true;
}
