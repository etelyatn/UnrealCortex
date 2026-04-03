#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
	FCortexCommandRouter CreateLifecycleRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
			MakeShared<FCortexLevelCommandHandler>());
		return Router;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelInvalidPathTest,
	"Cortex.Level.Lifecycle.CreateLevel.InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelInvalidPathTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("InvalidPath/NoSlash"));

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestFalse(TEXT("Should fail for invalid path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_PARAMETER"), Result.ErrorCode, TEXT("INVALID_PARAMETER"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelListTemplatesTest,
	"Cortex.Level.Lifecycle.ListTemplates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListTemplatesTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("level.list_templates"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_templates should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Templates = nullptr;
		TestTrue(TEXT("Should have templates array"), Result.Data->TryGetArrayField(TEXT("templates"), Templates));

		if (Templates)
		{
			TestTrue(TEXT("Should have at least one template"), Templates->Num() > 0);

			for (const TSharedPtr<FJsonValue>& Value : *Templates)
			{
				const TSharedPtr<FJsonObject>* TemplateObj = nullptr;
				if (Value->TryGetObject(TemplateObj) && TemplateObj && TemplateObj->IsValid())
				{
					TestTrue(TEXT("Template should have name"), (*TemplateObj)->HasField(TEXT("name")));
					TestTrue(TEXT("Template should have path"), (*TemplateObj)->HasField(TEXT("path")));
				}
			}
		}
	}

	return true;
}
