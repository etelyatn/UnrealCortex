#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UMaterial;
class UMaterialExpression;

class FCortexMaterialGraphOps
{
public:
	static FCortexCommandResult ListNodes(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListConnections(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Connect(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Disconnect(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AutoLayout(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNodePins(const TSharedPtr<FJsonObject>& Params);

private:
	static UMaterialExpression* FindExpression(UMaterial* Material, const FString& NodeId);
};
