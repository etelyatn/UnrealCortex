#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexGraphTraceOps
{
public:
	static FCortexCommandResult TraceExec(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult TraceDataflow(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetSubgraph(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListEventHandlers(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindEventHandler(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindFunctionCalls(const TSharedPtr<FJsonObject>& Params);
};
