#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesSnapshotTest,
	"Cortex.Core.Capabilities.Snapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString SnapshotPath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("UnrealCortex/MCP/tests/fixtures/mcp_tool_signature_snapshot.json"));
	TestTrue(TEXT("Snapshot fixture exists"), FPaths::FileExists(SnapshotPath));

	if (!FPaths::FileExists(SnapshotPath))
	{
		return false;
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *SnapshotPath))
	{
		AddError(TEXT("Failed to read snapshot fixture"));
		return false;
	}

	TSharedPtr<FJsonObject> SnapshotJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	TestTrue(TEXT("Snapshot fixture is valid JSON"), FJsonSerializer::Deserialize(Reader, SnapshotJson) && SnapshotJson.IsValid());

	if (!SnapshotJson.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Snapshot includes core domain"), SnapshotJson->HasField(TEXT("core")));
	TestTrue(TEXT("Snapshot includes data domain"), SnapshotJson->HasField(TEXT("data")));
	TestTrue(TEXT("Snapshot includes editor domain"), SnapshotJson->HasField(TEXT("editor")));

	FModuleManager::Get().LoadModule(TEXT("CortexData"));
	FModuleManager::Get().LoadModule(TEXT("CortexEditor"));
	FModuleManager::Get().LoadModule(TEXT("CortexBlueprint"));
	FModuleManager::Get().LoadModule(TEXT("CortexMaterial"));
	FModuleManager::Get().LoadModule(TEXT("CortexLevel"));
	FModuleManager::Get().LoadModule(TEXT("CortexUMG"));
	FModuleManager::Get().LoadModule(TEXT("CortexQA"));

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	const FCortexCommandResult CapResult =
		CoreModule.GetCommandRouter().Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
	TestTrue(
		TEXT("Capabilities contain domains object"),
		CapResult.Data.IsValid() && CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr);

	if (DomainsObj == nullptr)
	{
		return false;
	}

	auto FindCommandParams = [](const TSharedPtr<FJsonObject>& DomainObj, const FString& CommandName) -> const TArray<TSharedPtr<FJsonValue>>*
	{
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		if (!DomainObj.IsValid() || !DomainObj->TryGetArrayField(TEXT("commands"), Commands) || Commands == nullptr)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
		{
			const TSharedPtr<FJsonObject>* CommandObj = nullptr;
			if (CommandValue->TryGetObject(CommandObj) && CommandObj != nullptr)
			{
				FString Name;
				if ((*CommandObj)->TryGetStringField(TEXT("name"), Name) && Name == CommandName)
				{
					const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
					if ((*CommandObj)->TryGetArrayField(TEXT("params"), Params))
					{
						return Params;
					}
					return nullptr;
				}
			}
		}

		return nullptr;
	};

	auto SnapshotParams = [this, SnapshotJson](const FString& DomainName, const FString& ToolName, const TArray<FString>& Expected) -> bool
	{
		const TSharedPtr<FJsonObject>* DomainObj = nullptr;
		if (!SnapshotJson->TryGetObjectField(DomainName, DomainObj) || DomainObj == nullptr)
		{
			AddError(FString::Printf(TEXT("Missing snapshot domain: %s"), *DomainName));
			return false;
		}

		const TSharedPtr<FJsonObject>* ToolObj = nullptr;
		if (!(*DomainObj)->TryGetObjectField(ToolName, ToolObj) || ToolObj == nullptr)
		{
			AddError(FString::Printf(TEXT("Missing snapshot tool: %s.%s"), *DomainName, *ToolName));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
		if (!(*ToolObj)->TryGetArrayField(TEXT("params"), Params) || Params == nullptr)
		{
			AddError(FString::Printf(TEXT("Missing snapshot params for: %s.%s"), *DomainName, *ToolName));
			return false;
		}

		if (!TestEqual(FString::Printf(TEXT("%s.%s param count"), *DomainName, *ToolName), Params->Num(), Expected.Num()))
		{
			return false;
		}

		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			FString ParamName;
			if (!(*Params)[Index]->TryGetString(ParamName))
			{
				AddError(FString::Printf(TEXT("Invalid snapshot param entry for %s.%s at index %d"), *DomainName, *ToolName, Index));
				return false;
			}
			if (!TestEqual(FString::Printf(TEXT("%s.%s param[%d]"), *DomainName, *ToolName, Index), ParamName, Expected[Index]))
			{
				return false;
			}
		}

		return true;
	};

	auto AssertCapabilityMatchesSnapshot =
		[this, SnapshotJson, DomainsObj, &FindCommandParams, &SnapshotParams](
			const FString& DomainName,
			const FString& CapabilityCommand,
			const FString& SnapshotTool,
			const TArray<FString>& ParamAliases) -> bool
	{
		const TSharedPtr<FJsonObject>* CapabilityDomain = nullptr;
		if (!(*DomainsObj)->TryGetObjectField(DomainName, CapabilityDomain) || CapabilityDomain == nullptr)
		{
			AddError(FString::Printf(TEXT("Missing capability domain: %s"), *DomainName));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Params = FindCommandParams(*CapabilityDomain, CapabilityCommand);
		if (Params == nullptr)
		{
			AddError(FString::Printf(TEXT("Missing capability params for %s.%s"), *DomainName, *CapabilityCommand));
			return false;
		}

		if (!SnapshotParams(DomainName, SnapshotTool, ParamAliases))
		{
			return false;
		}

		if (!TestEqual(FString::Printf(TEXT("%s.%s capability param count"), *DomainName, *CapabilityCommand), Params->Num(), ParamAliases.Num()))
		{
			return false;
		}

		for (int32 Index = 0; Index < ParamAliases.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* ParamObj = nullptr;
			if (!(*Params)[Index]->TryGetObject(ParamObj) || ParamObj == nullptr)
			{
				AddError(FString::Printf(TEXT("Invalid capability param entry for %s.%s at index %d"), *DomainName, *CapabilityCommand, Index));
				return false;
			}

			FString ParamName;
			if (!(*ParamObj)->TryGetStringField(TEXT("name"), ParamName))
			{
				AddError(FString::Printf(TEXT("Missing capability param name for %s.%s at index %d"), *DomainName, *CapabilityCommand, Index));
				return false;
			}

			if (!TestEqual(FString::Printf(TEXT("%s.%s capability param[%d]"), *DomainName, *CapabilityCommand, Index), ParamName, ParamAliases[Index]))
			{
				return false;
			}
		}

		return true;
	};

	AssertCapabilityMatchesSnapshot(TEXT("data"), TEXT("query_datatable"), TEXT("query_datatable"),
		{ TEXT("table_path"), TEXT("row_name_pattern"), TEXT("row_names"), TEXT("fields"), TEXT("limit"), TEXT("offset") });
	AssertCapabilityMatchesSnapshot(TEXT("editor"), TEXT("get_recent_logs"), TEXT("get_recent_logs"),
		{ TEXT("severity"), TEXT("since_seconds"), TEXT("since_cursor"), TEXT("category") });
	AssertCapabilityMatchesSnapshot(TEXT("blueprint"), TEXT("add_function"), TEXT("add_blueprint_function"),
		{ TEXT("asset_path"), TEXT("name"), TEXT("is_pure"), TEXT("access"), TEXT("inputs"), TEXT("outputs") });
	AssertCapabilityMatchesSnapshot(TEXT("material"), TEXT("set_parameter"), TEXT("set_parameter"),
		{ TEXT("asset_path"), TEXT("parameter_name"), TEXT("parameter_type"), TEXT("value") });
	AssertCapabilityMatchesSnapshot(TEXT("level"), TEXT("spawn_actor"), TEXT("spawn_actor"),
		{ TEXT("class_name"), TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("label"), TEXT("folder"), TEXT("mesh"), TEXT("material") });
	AssertCapabilityMatchesSnapshot(TEXT("umg"), TEXT("add_widget"), TEXT("add_widget"),
		{ TEXT("asset_path"), TEXT("widget_class"), TEXT("name"), TEXT("parent_name"), TEXT("slot_index") });
	AssertCapabilityMatchesSnapshot(TEXT("qa"), TEXT("move_to"), TEXT("move_player_to"),
		{ TEXT("target"), TEXT("timeout"), TEXT("acceptance_radius") });

	return true;
}
