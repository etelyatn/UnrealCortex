#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FCortexCommandRouter;
class FJsonObject;
class IHttpRouter;
struct FHttpRouteHandleInternal;

/**
 * PROOF OF CONCEPT — native HTTP/SSE MCP transport for UnrealCortex.
 *
 * Hosts an in-editor HTTP endpoint (UE HTTPServer / IHttpRouter) that speaks the
 * MCP protocol (JSON-RPC: initialize / tools/list / tools/call) directly, so an
 * MCP client can drive the editor over HTTP with NO Python bridge and NO TCP
 * port-file discovery — mirroring UE 5.8's native ModelContextProtocol plugin.
 * tools/list is generated from the live command registry; tools/call dispatches
 * straight into the existing FCortexCommandRouter.
 *
 * Runs ALONGSIDE the existing FCortexTcpServer (it does not replace it). Responses
 * use MCP "Streamable HTTP" with single application/json bodies (no SSE in this POC).
 *
 * This header stays free of HTTPServer includes (HTTPServer is a private dependency);
 * all HTTPServer types are used only in the .cpp.
 */
class CORTEXCORE_API FCortexHttpServer
{
public:
	FCortexHttpServer() = default;
	~FCortexHttpServer();

	/** Bind /mcp on the given port and start listening. Reuses InRouter for dispatch. */
	bool Start(int32 Port, FCortexCommandRouter& InRouter);

	/** Unbind the route and stop. Does not stop other HTTPServer listeners. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	int32 GetPort() const { return BoundPort; }

private:
	/**
	 * Parse one JSON-RPC request and produce the response string.
	 * InSessionId  = client's Mcp-Session-Id request header ("" if none).
	 * OutSessionId = session id to echo back (newly minted on initialize; else InSessionId).
	 * OutHttpCode  = 200 ok / 202 notification-ack / 400 missing-session / 404 unknown-session.
	 * Empty return => notification (no body).
	 */
	FString HandleJsonRpc(const FString& RequestBody, const FString& InSessionId,
	                      FString& OutSessionId, int32& OutHttpCode);

	/** Build the tools/list payload from the live command registry. */
	TSharedPtr<FJsonObject> BuildToolsList() const;

	/** Dispatch a tools/call into the command router; returns the MCP result object. */
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params, bool& bOutIsError) const;

	FCortexCommandRouter* Router = nullptr;
	TSharedPtr<IHttpRouter> HttpRouter;
	TSharedPtr<const FHttpRouteHandleInternal> RouteHandle;
	FString CapabilityToken;      // required in X-MCP-Capability-Token (auto-generated if CVar unset)
	TSet<FString> ActiveSessions; // minted on initialize; enforced on every later request
	int32 BoundPort = 0;
	bool bRunning = false;
};
