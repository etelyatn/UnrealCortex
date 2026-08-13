#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexCoreCommandHandler.h"
#include "ICortexDomainHandler.h"
#include "Dom/JsonObject.h"

namespace
{
/** Test double for a domain handler advertising a graph-style command contract. */
class FCortexOperationSchemaGraphHandler : public ICortexDomainHandler
{
public:
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr) override
	{
		(void)Command;
		(void)Params;
		(void)DeferredCallback;
		return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
	}

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
	{
		return {
			FCortexCommandInfo{ TEXT("add_node"), TEXT("Add a node to a graph") }
				.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full asset path"))
				.Required(TEXT("node_class"), TEXT("string"), TEXT("Node class name"))
				.Optional(TEXT("params"), TEXT("object"), TEXT("Node construction params")),
		};
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexOperationSchemaTest,
	"Cortex.Core.OperationSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexOperationSchemaTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexOperationSchemaGraphHandler>());
	Router.RegisterDomain(TEXT("core"), TEXT("Cortex Core"), TEXT("1.0.1"),
		MakeShared<FCortexCoreCommandHandler>(&Router));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("domain"), TEXT("graph"));
	Params->SetStringField(TEXT("command"), TEXT("add_node"));
	FCortexCommandResult Result = Router.Execute(TEXT("core.get_operation_schema"), Params);
	TestTrue(TEXT("live command schema must succeed"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("source is live_editor"), Result.Data->GetStringField(TEXT("source")), FString(TEXT("live_editor")));
		TestEqual(TEXT("domain echoes"), Result.Data->GetStringField(TEXT("domain")), FString(TEXT("graph")));
		TestEqual(TEXT("command echoes"), Result.Data->GetStringField(TEXT("command")), FString(TEXT("add_node")));
		TestEqual(TEXT("router is graph_cmd"), Result.Data->GetStringField(TEXT("router")), FString(TEXT("graph_cmd")));
		TestTrue(TEXT("editor_instance_id is non-empty"), !Result.Data->GetStringField(TEXT("editor_instance_id")).IsEmpty());
		TestTrue(TEXT("plugin_build_id is non-empty"), !Result.Data->GetStringField(TEXT("plugin_build_id")).IsEmpty());
		const TArray<TSharedPtr<FJsonValue>>* ParamList = nullptr;
		TestTrue(TEXT("params array present"), Result.Data->TryGetArrayField(TEXT("params"), ParamList));
		if (ParamList)
		{
			TestTrue(TEXT("params describe add_node"), ParamList->Num() >= 1);
		}
	}

	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("domain"), TEXT("graph"));
	MissingParams->SetStringField(TEXT("command"), TEXT("describe_node"));
	FCortexCommandResult Missing = Router.Execute(TEXT("core.get_operation_schema"), MissingParams);
	TestFalse(TEXT("absent command must fail"), Missing.bSuccess);
	TestEqual(TEXT("error code"), Missing.ErrorCode, CortexErrorCodes::CapabilityCommandNotFound);
	if (Missing.ErrorDetails.IsValid())
	{
		TestTrue(TEXT("has restart_or_reload_required"),
			Missing.ErrorDetails->HasField(TEXT("restart_or_reload_required")));
		TestTrue(TEXT("has suggested_action"), Missing.ErrorDetails->HasField(TEXT("suggested_action")));
	}

	TSharedPtr<FJsonObject> NoDomain = MakeShared<FJsonObject>();
	NoDomain->SetStringField(TEXT("command"), TEXT("add_node"));
	FCortexCommandResult BadParams = Router.Execute(TEXT("core.get_operation_schema"), NoDomain);
	TestFalse(TEXT("missing domain must fail"), BadParams.bSuccess);
	TestEqual(TEXT("missing domain code"), BadParams.ErrorCode, CortexErrorCodes::InvalidField);

	FCortexCommandResult Status = Router.Execute(TEXT("get_status"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), Status.bSuccess);
	return true;
}
