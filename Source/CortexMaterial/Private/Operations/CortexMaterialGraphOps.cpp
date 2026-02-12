#include "Operations/CortexMaterialGraphOps.h"
#include "CortexMaterialModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

UMaterialExpression* FCortexMaterialGraphOps::FindExpression(UMaterial* Material, const FString& NodeId)
{
	if (Material == nullptr || !Material->GetEditorOnlyData())
	{
		return nullptr;
	}

	for (UMaterialExpression* Expression : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
	{
		if (Expression && Expression->GetName() == NodeId)
		{
			return Expression;
		}
	}

	return nullptr;
}

FCortexCommandResult FCortexMaterialGraphOps::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::GetNode(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::ListConnections(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialGraphOps::Disconnect(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
