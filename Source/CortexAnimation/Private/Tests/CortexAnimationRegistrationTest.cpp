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

	TestEqual(TEXT("Phase C animation slice exposes twenty-three commands"), Commands.Num(), 23);
	TestTrue(TEXT("list_assets exists"), Names.Contains(TEXT("list_assets")));
	TestTrue(TEXT("get_sequence_info exists"), Names.Contains(TEXT("get_sequence_info")));
	TestTrue(TEXT("get_montage_info exists"), Names.Contains(TEXT("get_montage_info")));
	TestTrue(TEXT("get_skeleton_info exists"), Names.Contains(TEXT("get_skeleton_info")));
	TestTrue(TEXT("get_animbp_info exists"), Names.Contains(TEXT("get_animbp_info")));
	TestTrue(TEXT("add_named_notify exists"), Names.Contains(TEXT("add_named_notify")));
	TestTrue(TEXT("update_named_notify exists"), Names.Contains(TEXT("update_named_notify")));
	TestTrue(TEXT("remove_named_notify exists"), Names.Contains(TEXT("remove_named_notify")));
	TestTrue(TEXT("add_curve exists"), Names.Contains(TEXT("add_curve")));
	TestTrue(TEXT("set_curve_keys exists"), Names.Contains(TEXT("set_curve_keys")));
	TestTrue(TEXT("remove_curve exists"), Names.Contains(TEXT("remove_curve")));
	TestTrue(TEXT("add_montage_section exists"), Names.Contains(TEXT("add_montage_section")));
	TestTrue(TEXT("update_montage_section exists"), Names.Contains(TEXT("update_montage_section")));
	TestTrue(TEXT("remove_montage_section exists"), Names.Contains(TEXT("remove_montage_section")));
	TestTrue(TEXT("add_socket exists"), Names.Contains(TEXT("add_socket")));
	TestTrue(TEXT("set_socket_transform exists"), Names.Contains(TEXT("set_socket_transform")));
	TestTrue(TEXT("remove_socket exists"), Names.Contains(TEXT("remove_socket")));
	TestTrue(TEXT("add_notify exists"), Names.Contains(TEXT("add_notify")));
	TestTrue(TEXT("update_notify exists"), Names.Contains(TEXT("update_notify")));
	TestTrue(TEXT("remove_notify exists"), Names.Contains(TEXT("remove_notify")));
	TestTrue(TEXT("add_notify_state exists"), Names.Contains(TEXT("add_notify_state")));
	TestTrue(TEXT("update_notify_state exists"), Names.Contains(TEXT("update_notify_state")));
	TestTrue(TEXT("remove_notify_state exists"), Names.Contains(TEXT("remove_notify_state")));

	const TSet<FString> Forbidden = {
		TEXT("save_asset")
	};
	for (const FString& ForbiddenName : Forbidden)
	{
		TestFalse(*FString::Printf(TEXT("Phase A must not expose %s"), *ForbiddenName), Names.Contains(ForbiddenName));
	}

	return true;
}
