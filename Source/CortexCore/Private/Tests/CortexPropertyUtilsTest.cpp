#include "Misc/AutomationTest.h"
#include "CortexPropertyUtils.h"
#include "GameFramework/Actor.h"
#include "Editor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPropertyUtilsResolveTest,
	"Cortex.Core.PropertyUtils.ResolveBasicProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPropertyUtilsResolveTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("No editor world available - skipping"));
		return true;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(TEXT("CortexPropertyTestActor"));
	AActor* TestActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	TestNotNull(TEXT("Should spawn test actor"), TestActor);

	if (!TestActor)
	{
		return true;
	}

	FProperty* OutProperty = nullptr;
	void* OutValuePtr = nullptr;
	const bool bResolved = FCortexPropertyUtils::ResolvePropertyPath(TestActor, TEXT("bHidden"), OutProperty, OutValuePtr);
	TestTrue(TEXT("Should resolve bHidden property"), bResolved);
	TestNotNull(TEXT("Property should not be null"), OutProperty);
	TestNotNull(TEXT("Value pointer should not be null"), OutValuePtr);

	TestActor->Destroy();
	return true;
}
