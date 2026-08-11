#include "CortexHttpServer.h"

#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"   // LogCortex
#include "CortexTypes.h"
#include "ICortexDomainHandler.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpServerConstants.h"

#include "Misc/Guid.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* kMcpProtocolVersion = TEXT("2024-11-05");

	FString SerializeJson(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	// NOTE: named MakeJsonRpcError (not MakeError) to avoid colliding with UE's global
	// MakeError() in ValueOrError.h, whose perfect-forwarding overload would otherwise win.
	FString MakeJsonRpcError(const TSharedPtr<FJsonValue>& IdValue, int32 Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Resp->SetField(TEXT("id"), IdValue.IsValid() ? IdValue : MakeShared<FJsonValueNull>());
		TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetNumberField(TEXT("code"), Code);
		Err->SetStringField(TEXT("message"), Message);
		Resp->SetObjectField(TEXT("error"), Err);
		return SerializeJson(Resp);
	}
}

FCortexHttpServer::~FCortexHttpServer()
{
	Stop();
}

bool FCortexHttpServer::Start(int32 Port, FCortexCommandRouter& InRouter)
{
	Router = &InRouter;
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	FHttpServerModule& Module = FHttpServerModule::Get();
	HttpRouter = Module.GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogCortex, Warning, TEXT("[http-mcp] failed to bind HTTP router on port %d"), Port);
		return false;
	}

	RouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda(
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				// GET = the optional server->client SSE stream. IHttpRouter is request/response
				// (can't hold a long-lived stream), and the MCP spec allows 405 here, so we
				// decline it; POST request/response carries initialize/tools.list/tools.call.
				if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
				{
					TUniquePtr<FHttpServerResponse> NotAllowed =
						FHttpServerResponse::Create(FString(), TEXT("text/plain"));
					NotAllowed->Code = EHttpServerResponseCodes::BadMethod; // 405
					OnComplete(MoveTemp(NotAllowed));
					return true;
				}

				// Decode UTF-8 body (Request.Body is not null-terminated).
				FString Body;
				if (Request.Body.Num() > 0)
				{
					TArray<uint8> Bytes = Request.Body;
					Bytes.Add(0);
					Body = FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData())));
				}

				const FString RespStr = HandleJsonRpc(Body);

				TUniquePtr<FHttpServerResponse> Response =
					FHttpServerResponse::Create(RespStr, TEXT("application/json"));
				// Empty body => JSON-RPC notification: acknowledge with 202.
				Response->Code = RespStr.IsEmpty()
					? EHttpServerResponseCodes::Accepted
					: EHttpServerResponseCodes::Ok;

				// MCP Streamable HTTP session + protocol headers (issued leniently — not enforced).
				Response->Headers.Add(TEXT("Mcp-Session-Id"), { SessionId });
				Response->Headers.Add(TEXT("MCP-Protocol-Version"), { TEXT("2024-11-05") });

				OnComplete(MoveTemp(Response));
				return true;
			}));

	if (!RouteHandle.IsValid())
	{
		UE_LOG(LogCortex, Warning, TEXT("[http-mcp] failed to bind /mcp route on port %d"), Port);
		HttpRouter.Reset();
		return false;
	}

	Module.StartAllListeners();
	BoundPort = Port;
	bRunning = true;
	UE_LOG(LogCortex, Log, TEXT("[http-mcp] MCP-over-HTTP endpoint live at http://127.0.0.1:%d/mcp (POC)"), Port);
	return true;
}

void FCortexHttpServer::Stop()
{
	if (HttpRouter.IsValid() && RouteHandle.IsValid())
	{
		HttpRouter->UnbindRoute(RouteHandle);
	}
	RouteHandle.Reset();
	HttpRouter.Reset();
	Router = nullptr;
	bRunning = false;
	BoundPort = 0;
}

FString FCortexHttpServer::HandleJsonRpc(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return MakeJsonRpcError(nullptr, -32700, TEXT("Parse error"));
	}

	FString Method;
	Root->TryGetStringField(TEXT("method"), Method);

	const TSharedPtr<FJsonValue>* IdFound = Root->Values.Find(TEXT("id"));
	const TSharedPtr<FJsonValue> IdValue = IdFound ? *IdFound : nullptr;
	const bool bHasId = IdValue.IsValid();

	// JSON-RPC notifications (no id, or "notifications/*") get an empty ack.
	if (!bHasId || Method.StartsWith(TEXT("notifications/")))
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Result;
	bool bIsError = false;

	if (Method == TEXT("initialize"))
	{
		Result = MakeShared<FJsonObject>();
		// Echo the client's requested protocol version when present (better negotiation).
		FString ClientProto;
		const TSharedPtr<FJsonObject>* InitParams = nullptr;
		if (Root->TryGetObjectField(TEXT("params"), InitParams) && InitParams)
		{
			(*InitParams)->TryGetStringField(TEXT("protocolVersion"), ClientProto);
		}
		Result->SetStringField(TEXT("protocolVersion"), ClientProto.IsEmpty() ? kMcpProtocolVersion : ClientProto);
		TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
		Caps->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("capabilities"), Caps);
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), TEXT("cortex-inproc"));
		Info->SetStringField(TEXT("version"), TEXT("0.1.13-http-poc"));
		Result->SetObjectField(TEXT("serverInfo"), Info);
	}
	else if (Method == TEXT("ping"))
	{
		Result = MakeShared<FJsonObject>();
	}
	else if (Method == TEXT("tools/list"))
	{
		Result = BuildToolsList();
	}
	else if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> Params = Root->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr
			? *ParamsPtr : MakeShared<FJsonObject>();
		Result = HandleToolsCall(Params, bIsError);
	}
	else
	{
		return MakeJsonRpcError(IdValue, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
	}

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Resp->SetField(TEXT("id"), IdValue);
	Resp->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonObject>());
	return SerializeJson(Resp);
}

TSharedPtr<FJsonObject> FCortexHttpServer::BuildToolsList() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Tools;

	if (Router)
	{
		for (const FCortexRegisteredDomain& Domain : Router->GetRegisteredDomains())
		{
			const TArray<FCortexCommandInfo> Commands = Domain.Handler.IsValid()
				? Domain.Handler->GetSupportedCommands() : TArray<FCortexCommandInfo>();

			TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), Domain.Namespace + TEXT("_cmd"));
			Tool->SetStringField(TEXT("description"), FString::Printf(
				TEXT("Route UnrealCortex %s commands via %s_cmd(command, params). %d commands available."),
				*Domain.Namespace, *Domain.Namespace, Commands.Num()));

			// inputSchema: { command: string(enum), params: object }
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));

			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

			TSharedPtr<FJsonObject> CmdProp = MakeShared<FJsonObject>();
			CmdProp->SetStringField(TEXT("type"), TEXT("string"));
			CmdProp->SetStringField(TEXT("description"), TEXT("Subcommand to run"));
			TArray<TSharedPtr<FJsonValue>> EnumVals;
			for (const FCortexCommandInfo& Cmd : Commands)
			{
				EnumVals.Add(MakeShared<FJsonValueString>(Cmd.Name));
			}
			if (EnumVals.Num() > 0)
			{
				CmdProp->SetArrayField(TEXT("enum"), EnumVals);
			}
			Props->SetObjectField(TEXT("command"), CmdProp);

			TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
			ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
			ParamsProp->SetStringField(TEXT("description"), TEXT("Command parameters"));
			Props->SetObjectField(TEXT("params"), ParamsProp);

			Schema->SetObjectField(TEXT("properties"), Props);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("command")));
			Schema->SetArrayField(TEXT("required"), Required);

			Tool->SetObjectField(TEXT("inputSchema"), Schema);
			Tools.Add(MakeShared<FJsonValueObject>(Tool));
		}
	}

	Out->SetArrayField(TEXT("tools"), Tools);
	return Out;
}

TSharedPtr<FJsonObject> FCortexHttpServer::HandleToolsCall(const TSharedPtr<FJsonObject>& Params, bool& bOutIsError) const
{
	FString ToolName;
	Params->TryGetStringField(TEXT("name"), ToolName);

	const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
	TSharedPtr<FJsonObject> Arguments = Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr
		? *ArgsPtr : MakeShared<FJsonObject>();

	FString SubCommand;
	Arguments->TryGetStringField(TEXT("command"), SubCommand);

	const TSharedPtr<FJsonObject>* CmdParamsPtr = nullptr;
	TSharedPtr<FJsonObject> CmdParams = Arguments->TryGetObjectField(TEXT("params"), CmdParamsPtr) && CmdParamsPtr
		? *CmdParamsPtr : MakeShared<FJsonObject>();

	FString Namespace = ToolName;
	Namespace.RemoveFromEnd(TEXT("_cmd"));
	const FString FullCommand = Namespace + TEXT(".") + SubCommand;

	FString Text;
	bool bSuccess = false;
	if (Router)
	{
		const FCortexCommandResult R = Router->Execute(FullCommand, CmdParams, nullptr);
		bSuccess = R.bSuccess;
		if (R.bSuccess)
		{
			Text = SerializeJson(R.Data.IsValid() ? R.Data : MakeShared<FJsonObject>());
		}
		else
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("error_code"), R.ErrorCode);
			ErrObj->SetStringField(TEXT("error_message"), R.ErrorMessage);
			Text = SerializeJson(ErrObj);
		}
	}
	else
	{
		Text = TEXT("{\"error_message\":\"router unavailable\"}");
	}

	bOutIsError = !bSuccess;

	// MCP tools/call result: { content: [ {type:"text", text:...} ], isError: bool }
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("type"), TEXT("text"));
	Item->SetStringField(TEXT("text"), Text);
	Content.Add(MakeShared<FJsonValueObject>(Item));
	Out->SetArrayField(TEXT("content"), Content);
	Out->SetBoolField(TEXT("isError"), !bSuccess);
	return Out;
}
