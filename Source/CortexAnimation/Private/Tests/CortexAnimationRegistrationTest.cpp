#include "Misc/AutomationTest.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationModuleLoadTest,
	"Cortex.Animation.ModuleLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationModuleLoadTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CortexAnimation module should load"), FModuleManager::Get().ModuleExists(TEXT("CortexAnimation")));
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("CortexAnimation"));
	TestTrue(TEXT("CortexAnimation module should be loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("CortexAnimation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCapabilitiesReadOnlyTest,
	"Cortex.Animation.Capabilities.ReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCapabilitiesReadOnlyTest::RunTest(const FString& Parameters)
{
	FCortexAnimationCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	TSet<FString> Names;
	for (const FCortexCommandInfo& Command : Commands)
	{
		Names.Add(Command.Name);
	}

	TestEqual(TEXT("Phase A exposes five commands"), Commands.Num(), 5);
	TestTrue(TEXT("list_assets exists"), Names.Contains(TEXT("list_assets")));
	TestTrue(TEXT("get_sequence_info exists"), Names.Contains(TEXT("get_sequence_info")));
	TestTrue(TEXT("get_montage_info exists"), Names.Contains(TEXT("get_montage_info")));
	TestTrue(TEXT("get_skeleton_info exists"), Names.Contains(TEXT("get_skeleton_info")));
	TestTrue(TEXT("get_animbp_info exists"), Names.Contains(TEXT("get_animbp_info")));

	const TSet<FString> Forbidden = {
		TEXT("add_named_notify"),
		TEXT("add_notify"),
		TEXT("add_curve"),
		TEXT("add_montage_section"),
		TEXT("add_socket"),
		TEXT("save_asset")
	};
	for (const FString& ForbiddenName : Forbidden)
	{
		TestFalse(*FString::Printf(TEXT("Phase A must not expose %s"), *ForbiddenName), Names.Contains(ForbiddenName));
	}

	return true;
}
