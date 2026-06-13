#include "Providers/CortexCodexAppServerProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    FString GetCodexAppServerAccessModeDisplayName(ECortexAccessMode AccessMode)
    {
        switch (AccessMode)
        {
        case ECortexAccessMode::ReadOnly:
            return TEXT("Read-Only");
        case ECortexAccessMode::Guided:
            return TEXT("Guided");
        case ECortexAccessMode::FullAccess:
            return TEXT("Full Access");
        }

        return TEXT("Read-Only");
    }

    FString GetCodexAppServerWorkflowModeDisplayName(ECortexWorkflowMode WorkflowMode)
    {
        return WorkflowMode == ECortexWorkflowMode::Direct ? TEXT("Direct") : TEXT("Thorough");
    }

    FString GetCodexAppServerEnabledDisabledLabel(bool bEnabled)
    {
        return bEnabled ? TEXT("Enabled") : TEXT("Disabled");
    }

    FString SanitizeDirective(const FString& Directive)
    {
        FString Sanitized = Directive;
        Sanitized.ReplaceInline(TEXT("\n"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("\r"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("\t"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("$("), TEXT(""));
        Sanitized.ReplaceInline(TEXT("`"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("%"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("^"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("|"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("&"), TEXT(""));
        Sanitized.ReplaceInline(TEXT(">"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("<"), TEXT(""));
        return Sanitized;
    }

    FString BuildInstructionBlock(const FCortexSessionConfig& Config, ECortexAccessMode AccessMode)
    {
        const FCortexResolvedLaunchOptions& LaunchOptions = Config.LaunchOptions;

        FString Instructions = !Config.SystemPrompt.IsEmpty()
            ? Config.SystemPrompt
            : FString::Printf(
                TEXT("You are running inside the Unreal Editor's Cortex AI frontend. ")
                TEXT("You can inspect and modify the local workspace and use Cortex MCP tools when available. ")
                TEXT("Current access mode: %s."),
                *GetCodexAppServerAccessModeDisplayName(AccessMode));

        Instructions += FString::Printf(
            TEXT("\nCurrent access mode: %s")
            TEXT("\nWorkflow mode: %s")
            TEXT("\nProject context: %s")
            TEXT("\nAuto-context: %s"),
            *GetCodexAppServerAccessModeDisplayName(AccessMode),
            *GetCodexAppServerWorkflowModeDisplayName(LaunchOptions.WorkflowMode),
            *GetCodexAppServerEnabledDisabledLabel(LaunchOptions.bProjectContext),
            *GetCodexAppServerEnabledDisabledLabel(LaunchOptions.bAutoContext));

        if (LaunchOptions.WorkflowMode == ECortexWorkflowMode::Direct)
        {
            Instructions += TEXT("\nAct directly on the user's request. Do not detour into planning-only workflows or documentation-first ceremonies.");
        }

        if (!LaunchOptions.CustomDirective.IsEmpty())
        {
            Instructions += TEXT("\nCustom directive: ") + SanitizeDirective(LaunchOptions.CustomDirective);
        }

        return Instructions;
    }

    FString NormalizePathForJson(FString Path)
    {
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        return Path;
    }

    FString WriteJsonLine(const TSharedRef<FJsonObject>& Object)
    {
        FString Json;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
        FJsonSerializer::Serialize(Object, Writer);
        Writer->Close();
        return Json + TEXT("\n");
    }

    TSharedPtr<FJsonObject> MakeRequest(int32 RequestId, const FString& Method)
    {
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("method"), Method);
        Root->SetNumberField(TEXT("id"), RequestId);
        return Root;
    }

    FString SerializeCodexAppServerJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
    {
        if (!JsonObject.IsValid())
        {
            return FString();
        }

        FString JsonText;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
        FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
        Writer->Close();
        return JsonText;
    }

    void SetJsonPathValue(const TSharedPtr<FJsonObject>& RootObject, const FString& Path, const TSharedPtr<FJsonValue>& Value)
    {
        if (!RootObject.IsValid() || !Value.IsValid())
        {
            return;
        }

        TArray<FString> Segments;
        Path.ParseIntoArray(Segments, TEXT("."), true);
        if (Segments.Num() == 0)
        {
            return;
        }

        TSharedPtr<FJsonObject> Cursor = RootObject;
        for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
        {
            const FString& Segment = Segments[Index];
            const TSharedPtr<FJsonObject>* ExistingObject = nullptr;
            if (!Cursor->TryGetObjectField(Segment, ExistingObject) || ExistingObject == nullptr)
            {
                TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
                Cursor->SetObjectField(Segment, Child);
                Cursor = Child;
                continue;
            }

            Cursor = *ExistingObject;
        }

        Cursor->SetField(Segments.Last(), Value);
    }

    TSharedPtr<FJsonObject> BuildStructuredConfigObject(const FCortexSessionConfig& Config)
    {
        TSharedPtr<FJsonObject> ConfigObject = MakeShared<FJsonObject>();

        if (!Config.McpConfigPath.IsEmpty())
        {
            const TMap<FString, TSharedPtr<FJsonValue>> OverrideValues =
                FCortexMcpConfigTranslator::BuildCodexConfigOverrideValues(Config.McpConfigPath);
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : OverrideValues)
            {
                SetJsonPathValue(ConfigObject, Pair.Key, Pair.Value);
            }
        }

        return ConfigObject;
    }

    void CopyTokenUsageTotals(const TSharedPtr<FJsonObject>& TokenUsageObject, FCortexStreamEvent& OutEvent)
    {
        if (!TokenUsageObject.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* TotalObject = nullptr;
        if (!TokenUsageObject->TryGetObjectField(TEXT("total"), TotalObject) || TotalObject == nullptr)
        {
            return;
        }

        double Value = 0.0;
        if ((*TotalObject)->TryGetNumberField(TEXT("inputTokens"), Value))
        {
            OutEvent.InputTokens = static_cast<int64>(Value);
        }
        if ((*TotalObject)->TryGetNumberField(TEXT("outputTokens"), Value))
        {
            OutEvent.OutputTokens = static_cast<int64>(Value);
        }
        if ((*TotalObject)->TryGetNumberField(TEXT("cachedInputTokens"), Value))
        {
            OutEvent.CacheReadTokens = static_cast<int64>(Value);
        }
        if ((*TotalObject)->TryGetNumberField(TEXT("reasoningOutputTokens"), Value))
        {
            OutEvent.CacheCreationTokens = static_cast<int64>(Value);
        }
    }
}

FString FCortexCodexAppServerProtocol::BuildInitializeRequest(int32 RequestId)
{
    TSharedPtr<FJsonObject> Root = MakeRequest(RequestId, TEXT("initialize"));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
    ClientInfo->SetStringField(TEXT("name"), TEXT("cortex_frontend"));
    ClientInfo->SetStringField(TEXT("title"), TEXT("Cortex Frontend"));
    ClientInfo->SetStringField(TEXT("version"), TEXT("0.1.0"));
    Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

    TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetBoolField(TEXT("experimentalApi"), true);
    Params->SetObjectField(TEXT("capabilities"), Capabilities);

    Root->SetObjectField(TEXT("params"), Params);
    return WriteJsonLine(Root.ToSharedRef());
}

FString FCortexCodexAppServerProtocol::BuildInitializedNotification()
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("method"), TEXT("initialized"));
    Root->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
    return WriteJsonLine(Root.ToSharedRef());
}

FString FCortexCodexAppServerProtocol::BuildThreadStartRequest(
    int32 RequestId,
    const FCortexSessionConfig& Config,
    ECortexAccessMode AccessMode)
{
    TSharedPtr<FJsonObject> Root = MakeRequest(RequestId, TEXT("thread/start"));
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    if (!Config.ResolvedOptions.ModelId.IsEmpty())
    {
        Params->SetStringField(TEXT("model"), Config.ResolvedOptions.ModelId);
    }

    const FString WorkingDirectory = !Config.WorkingDirectory.IsEmpty()
        ? Config.WorkingDirectory
        : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    Params->SetStringField(TEXT("cwd"), NormalizePathForJson(WorkingDirectory));
    Params->SetStringField(TEXT("sandbox"), AccessModeToSandboxMode(AccessMode));
    Params->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
    Params->SetStringField(TEXT("instructions"), BuildInstructionBlock(Config, AccessMode));
    Params->SetObjectField(TEXT("config"), BuildStructuredConfigObject(Config));

    Root->SetObjectField(TEXT("params"), Params);
    return WriteJsonLine(Root.ToSharedRef());
}

FString FCortexCodexAppServerProtocol::BuildTurnStartRequest(
    int32 RequestId,
    const FString& ThreadId,
    const FString& Prompt,
    const FCortexSessionConfig& Config,
    ECortexAccessMode AccessMode)
{
    TSharedPtr<FJsonObject> Root = MakeRequest(RequestId, TEXT("turn/start"));
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("threadId"), ThreadId);

    TArray<TSharedPtr<FJsonValue>> InputItems;
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    InputObject->SetStringField(TEXT("type"), TEXT("text"));
    InputObject->SetStringField(TEXT("text"), Prompt);
    TArray<TSharedPtr<FJsonValue>> EmptyTextElements;
    InputObject->SetArrayField(TEXT("text_elements"), EmptyTextElements);
    InputItems.Add(MakeShared<FJsonValueObject>(InputObject));
    Params->SetArrayField(TEXT("input"), InputItems);

    Params->SetObjectField(TEXT("sandboxPolicy"), AccessModeToSandboxPolicy(AccessMode));

    TSharedPtr<FJsonObject> Effort = MakeShared<FJsonObject>();
    Effort->SetStringField(TEXT("effort"), EffortToAppServerValue(Config.ResolvedOptions.EffortLevel));
    Params->SetObjectField(TEXT("reasoning"), Effort);

    Root->SetObjectField(TEXT("params"), Params);
    return WriteJsonLine(Root.ToSharedRef());
}

FString FCortexCodexAppServerProtocol::BuildTurnInterruptRequest(int32 RequestId, const FString& ThreadId)
{
    TSharedPtr<FJsonObject> Root = MakeRequest(RequestId, TEXT("turn/interrupt"));
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("threadId"), ThreadId);
    Root->SetObjectField(TEXT("params"), Params);
    return WriteJsonLine(Root.ToSharedRef());
}

bool FCortexCodexAppServerProtocol::ParseLine(
    const FString& JsonLine,
    FCortexCodexAppServerProtocolState& State,
    TArray<FCortexStreamEvent>& OutEvents)
{
    OutEvents.Reset();

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* ResultObject = nullptr;
    if (Root->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject != nullptr)
    {
        const TSharedPtr<FJsonObject>* ThreadObject = nullptr;
        if ((*ResultObject)->TryGetObjectField(TEXT("thread"), ThreadObject) && ThreadObject != nullptr)
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::SessionInit;
            (*ThreadObject)->TryGetStringField(TEXT("id"), State.ThreadId);
            Event.SessionId = State.ThreadId;
            (*ResultObject)->TryGetStringField(TEXT("model"), Event.Model);
            Event.RawJson = JsonLine;
            OutEvents.Add(MoveTemp(Event));
            return true;
        }
    }

    FString Method;
    if (!Root->TryGetStringField(TEXT("method"), Method))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Params = nullptr;
    if (!Root->TryGetObjectField(TEXT("params"), Params) || Params == nullptr)
    {
        return false;
    }

    if (Method == TEXT("item/agentMessage/delta"))
    {
        FCortexStreamEvent Event;
        Event.Type = ECortexStreamEventType::ContentBlockDelta;
        (*Params)->TryGetStringField(TEXT("delta"), Event.Text);
        (*Params)->TryGetStringField(TEXT("threadId"), Event.SessionId);
        (*Params)->TryGetStringField(TEXT("turnId"), State.ActiveTurnId);
        State.PendingAssistantText += Event.Text;
        Event.RawJson = JsonLine;
        OutEvents.Add(MoveTemp(Event));
        return true;
    }

    if (Method == TEXT("turn/completed"))
    {
        FCortexStreamEvent Event;
        Event.Type = ECortexStreamEventType::Result;
        Event.SessionId = State.ThreadId;
        Event.ResultText = State.PendingAssistantText;

        const TSharedPtr<FJsonObject>* TurnObject = nullptr;
        if ((*Params)->TryGetObjectField(TEXT("turn"), TurnObject) && TurnObject != nullptr)
        {
            double DurationMs = 0.0;
            if ((*TurnObject)->TryGetNumberField(TEXT("durationMs"), DurationMs))
            {
                Event.DurationMs = static_cast<int32>(DurationMs);
            }
        }

        Event.RawJson = JsonLine;
        State.PendingAssistantText.Reset();
        State.ActiveTurnId.Reset();
        OutEvents.Add(MoveTemp(Event));
        return true;
    }

    if (Method == TEXT("item/started") || Method == TEXT("item/completed"))
    {
        const TSharedPtr<FJsonObject>* ItemObject = nullptr;
        if (!(*Params)->TryGetObjectField(TEXT("item"), ItemObject) || ItemObject == nullptr)
        {
            return false;
        }

        FString ItemType;
        if (!(*ItemObject)->TryGetStringField(TEXT("type"), ItemType))
        {
            return false;
        }

        if (Method == TEXT("item/started") && ItemType == TEXT("commandExecution"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::ToolUse;
            (*ItemObject)->TryGetStringField(TEXT("id"), Event.ToolCallId);
            (*ItemObject)->TryGetStringField(TEXT("command"), Event.ToolName);
            Event.ToolInput = SerializeCodexAppServerJsonObject(*ItemObject);
            Event.RawJson = JsonLine;
            OutEvents.Add(MoveTemp(Event));
            return true;
        }

        if (Method == TEXT("item/completed") && ItemType == TEXT("mcpToolCall"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::ToolResult;
            (*ItemObject)->TryGetStringField(TEXT("id"), Event.ToolCallId);
            (*ItemObject)->TryGetStringField(TEXT("tool"), Event.ToolName);

            const TSharedPtr<FJsonObject>* ResultValue = nullptr;
            if ((*ItemObject)->TryGetObjectField(TEXT("result"), ResultValue) && ResultValue != nullptr)
            {
                Event.ToolResultContent = SerializeCodexAppServerJsonObject(*ResultValue);
            }
            Event.RawJson = JsonLine;
            OutEvents.Add(MoveTemp(Event));
            return true;
        }
    }

    if (Method == TEXT("thread/tokenUsage/updated"))
    {
        FCortexStreamEvent Event;
        Event.Type = ECortexStreamEventType::Unknown;
        const TSharedPtr<FJsonObject>* TokenUsageObject = nullptr;
        if ((*Params)->TryGetObjectField(TEXT("tokenUsage"), TokenUsageObject) && TokenUsageObject != nullptr)
        {
            CopyTokenUsageTotals(*TokenUsageObject, Event);
        }
        Event.RawJson = JsonLine;
        OutEvents.Add(MoveTemp(Event));
        return true;
    }

    if (Method == TEXT("turn/failed") || Method == TEXT("error"))
    {
        FCortexStreamEvent Event;
        Event.Type = ECortexStreamEventType::Result;
        Event.bIsError = true;
        Event.ResultText = State.PendingAssistantText;
        if (Event.ResultText.IsEmpty())
        {
            (*Params)->TryGetStringField(TEXT("message"), Event.ResultText);
        }
        Event.RawJson = JsonLine;
        State.PendingAssistantText.Reset();
        State.ActiveTurnId.Reset();
        OutEvents.Add(MoveTemp(Event));
        return true;
    }

    return true;
}

FString FCortexCodexAppServerProtocol::AccessModeToSandboxMode(ECortexAccessMode AccessMode)
{
    switch (AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        return TEXT("read-only");
    case ECortexAccessMode::Guided:
        return TEXT("workspace-write");
    case ECortexAccessMode::FullAccess:
        return TEXT("danger-full-access");
    }

    return TEXT("read-only");
}

TSharedPtr<FJsonObject> FCortexCodexAppServerProtocol::AccessModeToSandboxPolicy(ECortexAccessMode AccessMode)
{
    TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();

    switch (AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        Policy->SetStringField(TEXT("type"), TEXT("readOnly"));
        break;

    case ECortexAccessMode::Guided:
        Policy->SetStringField(TEXT("type"), TEXT("workspaceWrite"));
        break;

    case ECortexAccessMode::FullAccess:
        Policy->SetStringField(TEXT("type"), TEXT("dangerFullAccess"));
        break;
    }

    return Policy;
}

FString FCortexCodexAppServerProtocol::EffortToAppServerValue(ECortexEffortLevel EffortLevel)
{
    switch (EffortLevel)
    {
    case ECortexEffortLevel::Default:
        return TEXT("default");
    case ECortexEffortLevel::Low:
        return TEXT("low");
    case ECortexEffortLevel::Medium:
        return TEXT("medium");
    case ECortexEffortLevel::High:
        return TEXT("high");
    case ECortexEffortLevel::Maximum:
        return TEXT("xhigh");
    }

    return TEXT("default");
}
