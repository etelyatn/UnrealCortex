#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexCoreCommandHandler.h"
#include "ICortexDomainHandler.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"

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
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
	TestTrue(TEXT("UnrealCortex descriptor is available"), Plugin.IsValid());
	if (!Plugin.IsValid())
	{
		return false;
	}
	const FString ExpectedPluginVersion = Plugin->GetDescriptor().VersionName;
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
		const FString PluginBuildId = Result.Data->GetStringField(TEXT("plugin_build_id"));
		TestTrue(TEXT("plugin_build_id uses the descriptor release version"),
			PluginBuildId.StartsWith(ExpectedPluginVersion + TEXT("-")));
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

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetStringField(TEXT("domain"), TEXT("core"));
	BatchParams->SetStringField(TEXT("command"), TEXT("batch_query"));
	FCortexCommandResult BatchSchema = Router.Execute(TEXT("core.get_operation_schema"), BatchParams);
	TestTrue(TEXT("batch_query schema must succeed"), BatchSchema.bSuccess);
	if (BatchSchema.bSuccess && BatchSchema.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BatchParamList = nullptr;
		TestTrue(TEXT("batch params array present"), BatchSchema.Data->TryGetArrayField(TEXT("params"), BatchParamList));
		if (BatchParamList)
		{
			for (const TSharedPtr<FJsonValue>& ParamValue : *BatchParamList)
			{
				const TSharedPtr<FJsonObject>* ParamObject = nullptr;
				if (ParamValue.IsValid() && ParamValue->TryGetObject(ParamObject) && ParamObject != nullptr)
				{
					const FString Name = (*ParamObject)->GetStringField(TEXT("name"));
					if (Name == TEXT("commands") || Name == TEXT("steps"))
					{
						TestFalse(*FString::Printf(TEXT("%s alias must not be independently required"), *Name),
							(*ParamObject)->GetBoolField(TEXT("required")));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> NoDomain = MakeShared<FJsonObject>();
	NoDomain->SetStringField(TEXT("command"), TEXT("add_node"));
	FCortexCommandResult BadParams = Router.Execute(TEXT("core.get_operation_schema"), NoDomain);
	TestFalse(TEXT("missing domain must fail"), BadParams.bSuccess);
	TestEqual(TEXT("missing domain code"), BadParams.ErrorCode, CortexErrorCodes::InvalidField);

	FCortexCommandResult Status = Router.Execute(TEXT("get_status"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), Status.bSuccess);
	if (Status.bSuccess && Status.Data.IsValid())
	{
		TestEqual(TEXT("get_status reports the release version"),
			Status.Data->GetStringField(TEXT("plugin_version")), ExpectedPluginVersion);
	}

	const FCortexCommandResult Capabilities =
		Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeds"), Capabilities.bSuccess);
	if (Capabilities.bSuccess && Capabilities.Data.IsValid())
	{
		TestEqual(TEXT("get_capabilities reports the release version"),
			Capabilities.Data->GetStringField(TEXT("plugin_version")), ExpectedPluginVersion);
	}
	return true;
}
