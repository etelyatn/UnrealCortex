
#include "Misc/AutomationTest.h"
#include "CortexTcpServer.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexServerAccessorsTest,
	"Cortex.Core.TcpServer.Accessors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCortexServerAccessorsTest::RunTest(const FString& Parameters)
{
	const int32 TestPort = 18850;
	FCortexCommandRouter Router;
	FCortexTcpServer Server;

	// Before start: port should be 0, client count should be 0
	TestEqual(TEXT("BoundPort should be 0 before start"), Server.GetBoundPort(), 0);
	TestEqual(TEXT("ClientCount should be 0 before start"), Server.GetClientCount(), 0);
	TestFalse(TEXT("IsRunning should be false before start"), Server.IsRunning());

	// Start the server
	const bool bStarted = Server.Start(TestPort,
		[&Router](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return Router.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("Server should start"), bStarted);

	if (!bStarted)
	{
		return false;
	}

	// After start: port should be set, running should be true
	TestTrue(TEXT("IsRunning should be true after start"), Server.IsRunning());
	TestTrue(TEXT("BoundPort should be >= TestPort"), Server.GetBoundPort() >= TestPort);
	TestEqual(TEXT("ClientCount should still be 0"), Server.GetClientCount(), 0);

	// Stop
	Server.Stop();
	TestFalse(TEXT("IsRunning should be false after stop"), Server.IsRunning());
	TestEqual(TEXT("BoundPort should be 0 after stop"), Server.GetBoundPort(), 0);
	TestEqual(TEXT("ClientCount should be 0 after stop"), Server.GetClientCount(), 0);

	return true;
}
