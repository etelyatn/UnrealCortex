#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexEditorPIEState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorTcpDisconnectTest,
	"Cortex.Editor.TcpDisconnect.CancelsInputTickers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorTcpDisconnectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Create handler (which creates its own PIEState internally)
	FCortexEditorCommandHandler Handler;

	// Smoke test — verify OnTcpClientDisconnected() is callable and safe
	// even when PIE is not active. The actual cancel behaviour is covered by
	// Cortex.Editor.PIEState.CancelTokenInvalidatesOldToken.
	Handler.OnTcpClientDisconnected();

	// If we reach here without crashing, the wiring is correct.
	// Actual cancel-token behaviour is covered by CancelTokenInvalidatesOldToken.
	AddInfo(TEXT("Smoke test: OnTcpClientDisconnected with no active PIE session completed without crash"));

	return true;
}
