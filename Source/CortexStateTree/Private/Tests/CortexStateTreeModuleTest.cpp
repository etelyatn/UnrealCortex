#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexStateTreeCommandHandler.h"
#include "ICortexCommandRegistry.h"

namespace
{
const TSharedPtr<FJsonObject>* FindCommandByName(
	const TArray<TSharedPtr<FJsonValue>>& Commands,
	const FString& CommandName)
{
	for (const TSharedPtr<FJsonValue>& CommandValue : Commands)
	{
		const TSharedPtr<FJsonObject>* CommandObject = nullptr;
		if (!CommandValue.IsValid() || !CommandValue->TryGetObject(CommandObject) || CommandObject == nullptr)
		{
			continue;
		}

		FString Name;
		if ((*CommandObject)->TryGetStringField(TEXT("name"), Name) && Name == CommandName)
		{
			return CommandObject;
		}
	}

	return nullptr;
}

bool HasParam(
	const TArray<TSharedPtr<FJsonValue>>& Params,
	const FString& ParamName,
	const FString& ParamType,
	bool bRequired)
{
	for (const TSharedPtr<FJsonValue>& ParamValue : Params)
	{
		const TSharedPtr<FJsonObject>* ParamObject = nullptr;
		if (!ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObject) || ParamObject == nullptr)
		{
			continue;
		}

		FString Name;
		FString Type;
		bool bActualRequired = false;
		if ((*ParamObject)->TryGetStringField(TEXT("name"), Name)
			&& (*ParamObject)->TryGetStringField(TEXT("type"), Type)
			&& (*ParamObject)->TryGetBoolField(TEXT("required"), bActualRequired)
			&& Name == ParamName
			&& Type == ParamType
			&& bActualRequired == bRequired)
		{
			return true;
		}
	}

	return false;
}

bool HasParamNamed(
	const TArray<TSharedPtr<FJsonValue>>& Params,
	const FString& ParamName)
{
	for (const TSharedPtr<FJsonValue>& ParamValue : Params)
	{
		const TSharedPtr<FJsonObject>* ParamObject = nullptr;
		if (!ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObject) || ParamObject == nullptr)
		{
			continue;
		}

		FString Name;
		if ((*ParamObject)->TryGetStringField(TEXT("name"), Name) && Name == ParamName)
		{
			return true;
		}
	}

	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeModuleRegistrationTest,
	"Cortex.StateTree.Module.RegistersDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeModuleRegistrationTest::RunTest(const FString& Parameters)
{
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("CortexStateTree"));

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandResult Capabilities =
		CoreModule.GetCommandRouter().Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());

	TestTrue(TEXT("get_capabilities succeeds"), Capabilities.bSuccess);

	const TSharedPtr<FJsonObject>* Domains = nullptr;
	TestTrue(TEXT("capabilities contains domains"),
		Capabilities.Data.IsValid()
		&& Capabilities.Data->TryGetObjectField(TEXT("domains"), Domains)
		&& Domains != nullptr);

	const TSharedPtr<FJsonObject>* StateTreeDomain = nullptr;
	TestTrue(TEXT("capabilities contains statetree domain"),
		Domains != nullptr
		&& (*Domains)->TryGetObjectField(TEXT("statetree"), StateTreeDomain)
		&& StateTreeDomain != nullptr);

	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	TestTrue(TEXT("statetree publishes command metadata"),
		StateTreeDomain != nullptr
		&& (*StateTreeDomain)->TryGetArrayField(TEXT("commands"), Commands)
		&& Commands != nullptr);

	if (Commands == nullptr)
	{
		return false;
	}

	TSet<FString> CommandNames;
	for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
	{
		const TSharedPtr<FJsonObject>* CommandObject = nullptr;
		if (CommandValue.IsValid() && CommandValue->TryGetObject(CommandObject) && CommandObject != nullptr)
		{
			FString Name;
			if ((*CommandObject)->TryGetStringField(TEXT("name"), Name))
			{
				CommandNames.Add(Name);
			}
		}
	}

	const TSet<FString> ExpectedNames = {
		TEXT("list_assets"),
		TEXT("create_asset"),
		TEXT("duplicate_asset"),
		TEXT("delete_asset"),
		TEXT("dump_tree"),
		TEXT("get_state"),
		TEXT("check_structure"),
		TEXT("validate_asset"),
		TEXT("compile"),
		TEXT("add_state"),
		TEXT("remove_state"),
		TEXT("rename_state"),
		TEXT("move_state"),
		TEXT("set_state_properties"),
		TEXT("add_transition"),
		TEXT("remove_transition"),
		TEXT("set_transition_properties"),
	};

	TestEqual(TEXT("statetree command set matches v1"), CommandNames.Num(), ExpectedNames.Num());
	for (const FString& ExpectedName : ExpectedNames)
	{
		TestTrue(FString::Printf(TEXT("command exists: %s"), *ExpectedName), CommandNames.Contains(ExpectedName));
	}

	const TSharedPtr<FJsonObject>* RenameState = FindCommandByName(*Commands, TEXT("rename_state"));
	const TSharedPtr<FJsonObject>* AddState = FindCommandByName(*Commands, TEXT("add_state"));
	const TSharedPtr<FJsonObject>* AddTransition = FindCommandByName(*Commands, TEXT("add_transition"));
	const TSharedPtr<FJsonObject>* RemoveTransition = FindCommandByName(*Commands, TEXT("remove_transition"));
	const TSharedPtr<FJsonObject>* SetTransitionProperties = FindCommandByName(*Commands, TEXT("set_transition_properties"));

	TestTrue(TEXT("rename_state metadata present"), RenameState != nullptr);
	TestTrue(TEXT("add_state metadata present"), AddState != nullptr);
	TestTrue(TEXT("add_transition metadata present"), AddTransition != nullptr);
	TestTrue(TEXT("remove_transition metadata present"), RemoveTransition != nullptr);
	TestTrue(TEXT("set_transition_properties metadata present"), SetTransitionProperties != nullptr);

	if (RenameState == nullptr
		|| AddState == nullptr
		|| AddTransition == nullptr
		|| RemoveTransition == nullptr
		|| SetTransitionProperties == nullptr)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RenameStateParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* AddStateParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* AddTransitionParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RemoveTransitionParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SetTransitionPropertiesParams = nullptr;

	TestTrue(TEXT("rename_state params present"),
		(*RenameState)->TryGetArrayField(TEXT("params"), RenameStateParams) && RenameStateParams != nullptr);
	TestTrue(TEXT("add_state params present"),
		(*AddState)->TryGetArrayField(TEXT("params"), AddStateParams) && AddStateParams != nullptr);
	TestTrue(TEXT("add_transition params present"),
		(*AddTransition)->TryGetArrayField(TEXT("params"), AddTransitionParams) && AddTransitionParams != nullptr);
	TestTrue(TEXT("remove_transition params present"),
		(*RemoveTransition)->TryGetArrayField(TEXT("params"), RemoveTransitionParams) && RemoveTransitionParams != nullptr);
	TestTrue(TEXT("set_transition_properties params present"),
		(*SetTransitionProperties)->TryGetArrayField(TEXT("params"), SetTransitionPropertiesParams)
		&& SetTransitionPropertiesParams != nullptr);

	if (RenameStateParams == nullptr
		|| AddStateParams == nullptr
		|| AddTransitionParams == nullptr
		|| RemoveTransitionParams == nullptr
		|| SetTransitionPropertiesParams == nullptr)
	{
		return false;
	}

	TestTrue(TEXT("rename_state uses required name"),
		HasParam(*RenameStateParams, TEXT("name"), TEXT("string"), true));
	TestFalse(TEXT("rename_state does not expose new_name at all"),
		HasParamNamed(*RenameStateParams, TEXT("new_name")));
	TestTrue(TEXT("rename_state exposes compile"),
		HasParam(*RenameStateParams, TEXT("compile"), TEXT("boolean"), false));
	TestTrue(TEXT("rename_state exposes save"),
		HasParam(*RenameStateParams, TEXT("save"), TEXT("boolean"), false));

	TestTrue(TEXT("add_state exposes type"),
		HasParam(*AddStateParams, TEXT("type"), TEXT("string"), false));
	TestTrue(TEXT("add_state exposes tag"),
		HasParam(*AddStateParams, TEXT("tag"), TEXT("string"), false));
	TestTrue(TEXT("add_state exposes enabled"),
		HasParam(*AddStateParams, TEXT("enabled"), TEXT("boolean"), false));
	TestTrue(TEXT("add_state exposes selection_behavior"),
		HasParam(*AddStateParams, TEXT("selection_behavior"), TEXT("string"), false));
	TestTrue(TEXT("add_state exposes compile"),
		HasParam(*AddStateParams, TEXT("compile"), TEXT("boolean"), false));
	TestTrue(TEXT("add_state exposes save"),
		HasParam(*AddStateParams, TEXT("save"), TEXT("boolean"), false));

	TestFalse(TEXT("add_transition does not require source_state_id"),
		HasParam(*AddTransitionParams, TEXT("source_state_id"), TEXT("string"), true));
	TestFalse(TEXT("add_transition does not require target_state_id"),
		HasParam(*AddTransitionParams, TEXT("target_state_id"), TEXT("string"), true));
	TestTrue(TEXT("add_transition exposes optional source_state_id"),
		HasParam(*AddTransitionParams, TEXT("source_state_id"), TEXT("string"), false));
	TestTrue(TEXT("add_transition exposes optional target_state_id"),
		HasParam(*AddTransitionParams, TEXT("target_state_id"), TEXT("string"), false));
	TestTrue(TEXT("add_transition exposes source_state_path"),
		HasParam(*AddTransitionParams, TEXT("source_state_path"), TEXT("string"), false));
	TestTrue(TEXT("add_transition exposes target_state_path"),
		HasParam(*AddTransitionParams, TEXT("target_state_path"), TEXT("string"), false));
	TestTrue(TEXT("add_transition exposes priority"),
		HasParam(*AddTransitionParams, TEXT("priority"), TEXT("string"), false));
	TestTrue(TEXT("add_transition exposes compile"),
		HasParam(*AddTransitionParams, TEXT("compile"), TEXT("boolean"), false));
	TestTrue(TEXT("add_transition exposes save"),
		HasParam(*AddTransitionParams, TEXT("save"), TEXT("boolean"), false));

	TestFalse(TEXT("remove_transition does not expose source_state_id at all"),
		HasParamNamed(*RemoveTransitionParams, TEXT("source_state_id")));
	TestTrue(TEXT("remove_transition exposes state_id"),
		HasParam(*RemoveTransitionParams, TEXT("state_id"), TEXT("string"), false));
	TestTrue(TEXT("remove_transition exposes state_path"),
		HasParam(*RemoveTransitionParams, TEXT("state_path"), TEXT("string"), false));
	TestTrue(TEXT("remove_transition exposes compile"),
		HasParam(*RemoveTransitionParams, TEXT("compile"), TEXT("boolean"), false));
	TestTrue(TEXT("remove_transition exposes save"),
		HasParam(*RemoveTransitionParams, TEXT("save"), TEXT("boolean"), false));

	TestFalse(TEXT("set_transition_properties does not expose source_state_id at all"),
		HasParamNamed(*SetTransitionPropertiesParams, TEXT("source_state_id")));
	TestTrue(TEXT("set_transition_properties exposes state_id"),
		HasParam(*SetTransitionPropertiesParams, TEXT("state_id"), TEXT("string"), false));
	TestTrue(TEXT("set_transition_properties exposes state_path"),
		HasParam(*SetTransitionPropertiesParams, TEXT("state_path"), TEXT("string"), false));
	TestTrue(TEXT("set_transition_properties exposes compile"),
		HasParam(*SetTransitionPropertiesParams, TEXT("compile"), TEXT("boolean"), false));
	TestTrue(TEXT("set_transition_properties exposes save"),
		HasParam(*SetTransitionPropertiesParams, TEXT("save"), TEXT("boolean"), false));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeUnknownCommandTest,
	"Cortex.StateTree.Module.UnknownCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeUnknownCommandTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FCortexCommandResult Result = Handler.Execute(TEXT("missing_command"), MakeShared<FJsonObject>());

	TestFalse(TEXT("unknown command fails"), Result.bSuccess);
	TestEqual(TEXT("unknown command error code"), Result.ErrorCode, CortexErrorCodes::UnknownCommand);
	return true;
}
