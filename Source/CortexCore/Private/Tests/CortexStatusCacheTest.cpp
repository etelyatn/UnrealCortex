#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStatusCacheFieldsTest,
	"Cortex.Core.Status.CacheFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStatusCacheFieldsTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();
	TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Router.Execute(TEXT("get_status"), ParamsObj);
	TestTrue(TEXT("get_status should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have caches field"), Result.Data->HasField(TEXT("caches")));

		const TSharedPtr<FJsonObject>* Caches = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("caches"), Caches))
		{
			TestTrue(TEXT("caches should have reflect"), (*Caches)->HasField(TEXT("reflect")));
			TestTrue(TEXT("caches should have blueprint"), (*Caches)->HasField(TEXT("blueprint")));

			const TSharedPtr<FJsonObject>* ReflectCache = nullptr;
			if ((*Caches)->TryGetObjectField(TEXT("reflect"), ReflectCache))
			{
				TestTrue(TEXT("reflect cache should have 'warm' field"), (*ReflectCache)->HasField(TEXT("warm")));
			}
		}
	}

	return true;
}
