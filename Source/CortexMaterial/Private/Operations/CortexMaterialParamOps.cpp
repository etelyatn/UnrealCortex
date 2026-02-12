#include "Operations/CortexMaterialParamOps.h"

FCortexCommandResult FCortexMaterialParamOps::ListParameters(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::GetParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::SetParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::SetParameters(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::ResetParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
