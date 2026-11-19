#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "ICortexDomainHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CortexRouterDeclaredParamsTest
{
	/** Counts dispatches so a test can prove a refused call never reached the handler. */
	class FProbeHandler : public ICortexDomainHandler
	{
	public:
		int32 ExecuteCount = 0;

		virtual FCortexCommandResult Execute(
			const FString& Command,
			const TSharedPtr<FJsonObject>& Params,
			FDeferredResponseCallback DeferredCallback = nullptr) override
		{
			(void)Params;
			(void)DeferredCallback;
			++ExecuteCount;
			if (Command == TEXT("probe") || Command == TEXT("bare") || Command == TEXT("loose") || Command == TEXT("strict"))
			{
				return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
			}
			return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("probe handler: unknown command"));
		}

		virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
		{
			return {
				FCortexCommandInfo{ TEXT("probe"), TEXT("Declares one optional param") }
					.Optional(TEXT("name"), TEXT("string"), TEXT("Name")),
				FCortexCommandInfo{ TEXT("bare"), TEXT("Declares no params") },
				FCortexCommandInfo{ TEXT("loose"), TEXT("Required param, not opted in") }
					.Required(TEXT("id"), TEXT("string"), TEXT("Id")),
				FCortexCommandInfo{ TEXT("strict"), TEXT("Required param, opted in") }
					.Required(TEXT("id"), TEXT("string"), TEXT("Id"))
					.StrictParams(),
			};
		}
	};

	TSharedPtr<FJsonObject> MakeParams(std::initializer_list<const TCHAR*> Keys)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		for (const TCHAR* Key : Keys)
		{
			Params->SetStringField(Key, TEXT("value"));
		}
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRouterDeclaredParamsUnknownTest,
	"Cortex.Core.Router.DeclaredParams.UnknownParamRefusedBeforeHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRouterDeclaredParamsUnknownTest::RunTest(const FString& Parameters)
{
	using namespace CortexRouterDeclaredParamsTest;

	FCortexCommandRouter Router;
	const TSharedPtr<FProbeHandler> Handler = MakeShared<FProbeHandler>();
	Router.RegisterDomain(TEXT("probe"), TEXT("Probe"), TEXT("1.0.0"), Handler);

	const FCortexCommandResult Refused = Router.Execute(TEXT("probe.probe"), MakeParams({ TEXT("name"), TEXT("bogus") }));
	TestFalse(TEXT("An undeclared param is refused for a command that never opted in"), Refused.bSuccess);
	TestEqual(TEXT("Refusal uses INVALID_FIELD"), Refused.ErrorCode, FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("Refusal names the unknown field"), Refused.ErrorMessage.Contains(TEXT("'bogus'")));
	TestTrue(TEXT("Refusal lists the accepted params"), Refused.ErrorMessage.Contains(TEXT("Accepted params: name")));
	TestEqual(TEXT("A refused call never reaches the handler"), Handler->ExecuteCount, 0);

	const FCortexCommandResult Accepted = Router.Execute(
		TEXT("probe.probe"), MakeParams({ TEXT("name"), TEXT("_telemetry_parallel") }));
	TestTrue(TEXT("Declared and router-owned underscore params are accepted"), Accepted.bSuccess);
	TestEqual(TEXT("The accepted call reaches the handler"), Handler->ExecuteCount, 1);

	const FCortexCommandResult Unknown = Router.Execute(TEXT("probe.not_declared"), MakeParams({ TEXT("anything") }));
	TestEqual(TEXT("A command absent from the table is left to the handler"), Unknown.ErrorMessage,
		FString(TEXT("probe handler: unknown command")));

	const FString OversizedKey = FString::ChrN(4000, TEXT('k'));
	TSharedPtr<FJsonObject> OversizedParams = MakeShared<FJsonObject>();
	OversizedParams->SetStringField(OversizedKey, TEXT("value"));
	const FCortexCommandResult Oversized = Router.Execute(TEXT("probe.probe"), OversizedParams);
	TestFalse(TEXT("An oversized undeclared param is refused"), Oversized.bSuccess);
	TestFalse(TEXT("The refusal does not echo an oversized caller-supplied name"),
		Oversized.ErrorMessage.Contains(OversizedKey.Left(65)));
	TestTrue(TEXT("The refusal reports the oversized name's length"), Oversized.ErrorMessage.Contains(TEXT("name length 4000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRouterDeclaredParamsBareTest,
	"Cortex.Core.Router.DeclaredParams.CommandWithoutParamsRefusesAnyKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRouterDeclaredParamsBareTest::RunTest(const FString& Parameters)
{
	using namespace CortexRouterDeclaredParamsTest;

	FCortexCommandRouter Router;
	const TSharedPtr<FProbeHandler> Handler = MakeShared<FProbeHandler>();
	Router.RegisterDomain(TEXT("probe"), TEXT("Probe"), TEXT("1.0.0"), Handler);

	const FCortexCommandResult Refused = Router.Execute(TEXT("probe.bare"), MakeParams({ TEXT("limit") }));
	TestFalse(TEXT("A command declaring no params refuses any caller key"), Refused.bSuccess);
	TestTrue(TEXT("Refusal names the field"), Refused.ErrorMessage.Contains(TEXT("'limit'")));
	TestTrue(TEXT("Refusal states that no params are accepted"), Refused.ErrorMessage.Contains(TEXT("Accepted params: none")));

	TestTrue(TEXT("Empty params are accepted"), Router.Execute(TEXT("probe.bare"), MakeParams({})).bSuccess);
	TestTrue(TEXT("Missing params object is accepted"), Router.Execute(TEXT("probe.bare"), nullptr).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRouterDeclaredParamsRequiredTest,
	"Cortex.Core.Router.DeclaredParams.RequiredPresenceOnlyForStrictCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRouterDeclaredParamsRequiredTest::RunTest(const FString& Parameters)
{
	using namespace CortexRouterDeclaredParamsTest;

	FCortexCommandRouter Router;
	const TSharedPtr<FProbeHandler> Handler = MakeShared<FProbeHandler>();
	Router.RegisterDomain(TEXT("probe"), TEXT("Probe"), TEXT("1.0.0"), Handler);

	TestTrue(TEXT("Required presence is left to the handler for an unopted command"),
		Router.Execute(TEXT("probe.loose"), MakeParams({})).bSuccess);

	const FCortexCommandResult Missing = Router.Execute(TEXT("probe.strict"), MakeParams({}));
	TestFalse(TEXT("A StrictParams command refuses a missing required param"), Missing.bSuccess);
	TestEqual(TEXT("Missing-required refusal message"), Missing.ErrorMessage,
		FString(TEXT("Missing required param: id (string)")));
	TestEqual(TEXT("Only the unopted call reached the handler"), Handler->ExecuteCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRouterDeclaredParamsBatchTest,
	"Cortex.Core.Router.DeclaredParams.BatchStepRefusedIndividually",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRouterDeclaredParamsBatchTest::RunTest(const FString& Parameters)
{
	using namespace CortexRouterDeclaredParamsTest;

	FCortexCommandRouter Router;
	const TSharedPtr<FProbeHandler> Handler = MakeShared<FProbeHandler>();
	Router.RegisterDomain(TEXT("probe"), TEXT("Probe"), TEXT("1.0.0"), Handler);

	auto MakeStep = [](const TSharedPtr<FJsonObject>& StepParams)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("command"), TEXT("probe.probe"));
		Step->SetObjectField(TEXT("params"), StepParams);
		return MakeShared<FJsonValueObject>(Step);
	};

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("commands"), {
		MakeStep(MakeParams({ TEXT("bogus") })),
		MakeStep(MakeParams({ TEXT("name") })),
	});

	const FCortexCommandResult Batch = Router.Execute(TEXT("batch"), BatchParams);
	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	if (!TestTrue(TEXT("Batch returns per-step results"),
		Batch.Data.IsValid() && Batch.Data->TryGetArrayField(TEXT("results"), Results) && Results->Num() == 2))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> First = (*Results)[0]->AsObject();
	TestFalse(TEXT("The step with an undeclared param fails"), First->GetBoolField(TEXT("success")));
	TestEqual(TEXT("The failing step reports INVALID_FIELD"), First->GetStringField(TEXT("error_code")),
		FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("The sibling step with declared params succeeds"), (*Results)[1]->AsObject()->GetBoolField(TEXT("success")));
	TestEqual(TEXT("Only the valid step reached the handler"), Handler->ExecuteCount, 1);
	return true;
}
