#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "ICortexDomainHandler.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRouterNullParamsTest,
	"Cortex.Core.Router.NullParamsReachHandlerAsEmptyObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRouterNullParamsTest::RunTest(const FString& Parameters)
{
	class FParamsProbeHandler : public ICortexDomainHandler
	{
	public:
		virtual FCortexCommandResult Execute(
			const FString& Command,
			const TSharedPtr<FJsonObject>& Params,
			FDeferredResponseCallback DeferredCallback = nullptr) override
		{
			(void)Command;
			(void)DeferredCallback;
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("params_valid"), Params.IsValid());
			Data->SetNumberField(TEXT("param_count"), Params.IsValid() ? Params->Values.Num() : -1);
			return FCortexCommandRouter::Success(Data);
		}

		virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
		{
			return { FCortexCommandInfo{ TEXT("probe"), TEXT("Reports whether params were provided") } };
		}
	};

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("nullparams"), TEXT("Null Params Test"), TEXT("1.0.0"), MakeShared<FParamsProbeHandler>());

	// The TCP server passes a null pointer when a request has no "params" object.
	const FCortexCommandResult Result = Router.Execute(TEXT("nullparams.probe"), TSharedPtr<FJsonObject>());
	TestTrue(TEXT("Command without params should still dispatch"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Handler should receive a valid params object"), Result.Data->GetBoolField(TEXT("params_valid")));
		TestEqual(TEXT("Substituted params object should be empty"),
			static_cast<int32>(Result.Data->GetNumberField(TEXT("param_count"))), 0);
	}

	return true;
}
