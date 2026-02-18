#include "Operations/CortexReflectOps.h"
#include "CortexReflectModule.h"

UClass* FCortexReflectOps::FindClassByName(const FString& ClassName, FCortexCommandResult& OutError)
{
	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::ClassNotFound,
		FString::Printf(TEXT("Class not found: %s"), *ClassName)
	);
	return nullptr;
}

bool FCortexReflectOps::IsProjectClass(const UClass* Class)
{
	return false;
}

FCortexCommandResult FCortexReflectOps::ClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::ClassDetail(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::FindOverrides(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::FindUsages(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::Search(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
