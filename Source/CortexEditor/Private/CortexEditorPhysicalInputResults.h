#pragma once

#include "CortexCommandRouter.h"
#include "CortexTypes.h"

/**
 * Result constructors shared by the physical input session translation units
 * (CortexEditorPhysicalInputSession.cpp and CortexEditorPhysicalInputOwnedPIE.cpp).
 */
namespace CortexEditorPhysicalInputResults
{
inline FCortexCommandResult MakeSuccessResult()
{
	FCortexCommandResult Result;
	Result.bSuccess = true;
	return Result;
}

inline FCortexCommandResult MakeErrorResult(const FString& ErrorCode, const FString& Message)
{
	return FCortexCommandRouter::Error(ErrorCode, Message);
}
}
