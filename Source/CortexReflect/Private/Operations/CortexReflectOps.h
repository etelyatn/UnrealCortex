#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

enum class ECortexModuleOrigin : uint8
{
	Unknown,
	Project,
	Engine
};

class FCortexReflectOps
{
public:
	static FCortexCommandResult ClassHierarchy(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ClassDetail(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindOverrides(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FindUsages(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Search(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDependencies(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetReferencers(const TSharedPtr<FJsonObject>& Params);
	static bool WriteReflectCache(
		const TSharedPtr<FJsonObject>& HierarchyData,
		const TSharedPtr<FJsonObject>& Params = nullptr
	);
	static ECortexModuleOrigin ClassifyNativeModuleFromMetadata(
		const FString& ModuleName,
		const FString& ModuleRelativePath,
		const TMap<FString, ECortexModuleOrigin>& PluginModuleOrigins
	);

private:
	static UClass* FindClassByName(const FString& ClassName, FCortexCommandResult& OutError);
	static bool IsProjectClass(const UClass* Class);
	static FString GetCppClassName(const UClass* Class);
	static void BuildHierarchyTree(
		UClass* Root,
		TSharedPtr<FJsonObject>& OutNode,
		int32 CurrentDepth,
		int32 MaxDepth,
		bool bIncludeBlueprint,
		bool bIncludeEngine,
		int32 MaxResults,
		int32& OutTotalCount,
		int32& OutCppCount,
		int32& OutBPCount,
		int32& OutProjectCppCount,
		int32& OutProjectBPCount
	);
	static TSharedPtr<FJsonObject> SerializeProperty(const FProperty* Property, const UObject* CDO);
	static TSharedPtr<FJsonObject> SerializeFunction(const UFunction* Function, const UClass* QueryClass);
	static TArray<FString> GetPropertyFlags(const FProperty* Property);
	static TArray<FString> GetFunctionFlags(const UFunction* Function);
	static FString GetPropertyTypeName(const FProperty* Property);
};
