#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexReflectOps
{
public:
	static FCortexCommandResult ClassHierarchy(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ClassDetail(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindOverrides(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindUsages(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Search(const TSharedPtr<FJsonObject>& Params);

private:
	static UClass* FindClassByName(const FString& ClassName, FCortexCommandResult& OutError);
	static bool IsProjectClass(const UClass* Class);
	static FString GetCppClassName(const UClass* Class);
};
