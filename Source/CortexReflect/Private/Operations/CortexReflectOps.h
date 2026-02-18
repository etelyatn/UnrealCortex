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
	static TSharedPtr<FJsonObject> SerializeProperty(const FProperty* Property, const UObject* CDO);
	static TSharedPtr<FJsonObject> SerializeFunction(const UFunction* Function, const UClass* QueryClass);
	static TArray<FString> GetPropertyFlags(const FProperty* Property);
	static TArray<FString> GetFunctionFlags(const UFunction* Function);
	static FString GetPropertyTypeName(const FProperty* Property);
};
