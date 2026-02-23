#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Engine/Blueprint.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"

namespace
{
	FString CreateTestBlueprint(FCortexBPCommandHandler& Handler, const FString& Suffix)
	{
		const FString Name = FString::Printf(TEXT("BP_CDOTest_%s"), *Suffix);
		const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPCDOTest_%s"), *Suffix);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("path"), Dir);
		Params->SetStringField(TEXT("type"), TEXT("Actor"));

		const FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			return FString();
		}

		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		return AssetPath;
	}

	void CleanupTestBlueprint(const FString& AssetPath)
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (Blueprint)
		{
			Blueprint->GetOutermost()->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsMissingPathTest,
	"Cortex.Blueprint.ClassDefaults.Get.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail without blueprint_path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Get.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/NonExistent/BP_Nope"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail for non-existent Blueprint"), Result.bSuccess);
	TestEqual(TEXT("Error code should be BLUEPRINT_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsDiscoveryTest,
	"Cortex.Blueprint.ClassDefaults.Get.Discovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsDiscoveryTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestTrue(TEXT("Discovery should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		TestTrue(TEXT("Should have properties object"),
			Result.Data->TryGetObjectField(TEXT("properties"), PropsObj));

		double Count = 0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("Should discover at least one property"), Count > 0);

		FString ParentClass;
		Result.Data->TryGetStringField(TEXT("parent_class"), ParentClass);
		TestEqual(TEXT("parent_class should be Actor"), ParentClass, TEXT("Actor"));
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsSpecificPropertyTest,
	"Cortex.Blueprint.ClassDefaults.Get.SpecificProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsSpecificPropertyTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicates")));
	Params->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestTrue(TEXT("Get specific property should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			const TSharedPtr<FJsonObject>* PropDetail = nullptr;
			TestTrue(TEXT("Should have bReplicates in results"),
				(*PropsObj)->TryGetObjectField(TEXT("bReplicates"), PropDetail));

			if (PropDetail && *PropDetail)
			{
				FString Type;
				(*PropDetail)->TryGetStringField(TEXT("type"), Type);
				TestFalse(TEXT("type should not be empty"), Type.IsEmpty());

				TestTrue(TEXT("value should be present"),
					(*PropDetail)->HasField(TEXT("value")));
			}
		}
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsPropertyNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Get.PropertyNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsPropertyNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicatess")));
	Params->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail for misspelled property"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PROPERTY_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::PropertyNotFound);

	if (Result.ErrorDetails.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
		TestTrue(TEXT("Should have suggestions array"),
			Result.ErrorDetails->TryGetArrayField(TEXT("suggestions"), Suggestions));
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsMissingPropertiesTest,
	"Cortex.Blueprint.ClassDefaults.Set.MissingProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsMissingPropertiesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Any/BP_Test"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail without properties"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsPropertyNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Set.PropertyNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsPropertyNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("NopeProperty"), true);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail for non-existent property"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PROPERTY_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::PropertyNotFound);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsTypeMismatchTest,
	"Cortex.Blueprint.ClassDefaults.Set.TypeMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsTypeMismatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("RootComponent"), 123.0);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail with type mismatch"), Result.bSuccess);
	TestEqual(TEXT("Error code should be TYPE_MISMATCH"),
		Result.ErrorCode, CortexErrorCodes::TypeMismatch);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsSuccessTest,
	"Cortex.Blueprint.ClassDefaults.Set.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsSuccessTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bReplicates"), true);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	SetParams->SetBoolField(TEXT("compile"), false);
	SetParams->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult SetResult = Handler.Execute(TEXT("set_class_defaults"), SetParams);

	TestTrue(TEXT("Setting class defaults should succeed"), SetResult.bSuccess);
	if (SetResult.Data.IsValid())
	{
		bool bCompiled = true;
		SetResult.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
		TestFalse(TEXT("compiled should be false when compile=false"), bCompiled);

		bool bSaved = true;
		SetResult.Data->TryGetBoolField(TEXT("saved"), bSaved);
		TestFalse(TEXT("saved should be false when save=false"), bSaved);
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicates")));
	GetParams->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_class_defaults"), GetParams);
	TestTrue(TEXT("Get after set should succeed"), GetResult.bSuccess);

	CleanupTestBlueprint(AssetPath);
	return true;
}
