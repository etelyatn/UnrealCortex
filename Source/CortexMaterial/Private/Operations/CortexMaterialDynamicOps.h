#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UPrimitiveComponent;
class UMaterialInstanceDynamic;
class UWorld;

/**
 * Runtime operations for UMaterialInstanceDynamic on PIE actors.
 * All methods require PIE to be Playing or Paused.
 * Resolution is stateless; actor/component/DMI are resolved fresh each call.
 */
class FCortexMaterialDynamicOps
{
public:
	static FCortexCommandResult ListDynamicInstances(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDynamicInstance(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateDynamicInstance(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DestroyDynamicInstance(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetDynamicParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDynamicParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListDynamicParameters(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetDynamicParameters(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ResetDynamicParameter(const TSharedPtr<FJsonObject>& Params);

private:
	static UWorld* GetPIEWorldOrError(FCortexCommandResult& OutError);

	static UPrimitiveComponent* ResolveComponent(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutActorName,
		int32& OutSlotIndex,
		FCortexCommandResult& OutError);

	static UMaterialInstanceDynamic* ResolveDMI(
		UWorld* World,
		const TSharedPtr<FJsonObject>& Params,
		UPrimitiveComponent*& OutComponent,
		FString& OutActorName,
		int32& OutSlotIndex,
		FCortexCommandResult& OutError);

	static TSharedPtr<FJsonObject> SerializeParameters(UMaterialInstanceDynamic* DMI);

	static bool ApplyParameter(
		UMaterialInstanceDynamic* DMI,
		const FString& ParamName,
		const FString& ParamType,
		const TSharedPtr<FJsonValue>& Value,
		FCortexCommandResult& OutError);

	static TArray<TSharedPtr<FJsonValue>> ColorToJsonArray(const FLinearColor& Color);
	static bool IsParameterOverridden(UMaterialInstanceDynamic* DMI, const FName& ParamName, const FString& ParamType);
	static int32 CountParameters(const TSharedPtr<FJsonObject>& ParametersJson);

	/** Determine parameter type by enumerating all parameter infos.
	 *  Returns "scalar", "vector", "texture", or empty string if not found. */
	static FString DetermineParameterType(UMaterialInstanceDynamic* DMI, const FName& ParamName);
};
