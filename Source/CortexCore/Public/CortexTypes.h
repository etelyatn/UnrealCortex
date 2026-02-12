
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Error codes matching the PRD specification */
namespace CortexErrorCodes
{
	static const FString TableNotFound = TEXT("TABLE_NOT_FOUND");
	static const FString RowNotFound = TEXT("ROW_NOT_FOUND");
	static const FString AssetNotFound = TEXT("ASSET_NOT_FOUND");
	static const FString RowAlreadyExists = TEXT("ROW_ALREADY_EXISTS");
	static const FString InvalidField = TEXT("INVALID_FIELD");
	static const FString InvalidValue = TEXT("INVALID_VALUE");
	static const FString InvalidStructType = TEXT("INVALID_STRUCT_TYPE");
	static const FString InvalidTag = TEXT("INVALID_TAG");
	static const FString SerializationError = TEXT("SERIALIZATION_ERROR");
	static const FString EditorNotReady = TEXT("EDITOR_NOT_READY");
	static const FString UnknownCommand = TEXT("UNKNOWN_COMMAND");
	static const FString CompositeWriteBlocked = TEXT("COMPOSITE_WRITE_BLOCKED");
	static const FString BatchLimitExceeded = TEXT("BATCH_LIMIT_EXCEEDED");
	static const FString BatchRecursionBlocked = TEXT("BATCH_RECURSION_BLOCKED");
	static const FString GraphNotFound = TEXT("GRAPH_NOT_FOUND");
	static const FString NodeNotFound = TEXT("NODE_NOT_FOUND");
	static const FString PinNotFound = TEXT("PIN_NOT_FOUND");
	static const FString PinTypeMismatch = TEXT("PIN_TYPE_MISMATCH");
	static const FString ConnectionExists = TEXT("CONNECTION_EXISTS");
	static const FString InvalidOperation = TEXT("INVALID_OPERATION");
	static const FString BlueprintNotFound = TEXT("BLUEPRINT_NOT_FOUND");
	static const FString BlueprintAlreadyExists = TEXT("BLUEPRINT_ALREADY_EXISTS");
	static const FString InvalidBlueprintType = TEXT("INVALID_BLUEPRINT_TYPE");
	static const FString InvalidParentClass = TEXT("INVALID_PARENT_CLASS");
	static const FString CompileFailed = TEXT("COMPILE_FAILED");
	static const FString VariableExists = TEXT("VARIABLE_EXISTS");
	static const FString VariableNotFound = TEXT("VARIABLE_NOT_FOUND");
	static const FString FunctionExists = TEXT("FUNCTION_EXISTS");
	static const FString HasReferences = TEXT("HAS_REFERENCES");
	static const FString WidgetNotFound = TEXT("WIDGET_NOT_FOUND");
	static const FString WidgetNameExists = TEXT("WIDGET_NAME_EXISTS");
	static const FString InvalidWidgetClass = TEXT("INVALID_WIDGET_CLASS");
	static const FString InvalidParent = TEXT("INVALID_PARENT");
	static const FString InvalidSlotIndex = TEXT("INVALID_SLOT_INDEX");
	static const FString AnimationNotFound = TEXT("ANIMATION_NOT_FOUND");
	static const FString AnimationExists = TEXT("ANIMATION_EXISTS");
	static const FString TrackNotFound = TEXT("TRACK_NOT_FOUND");
	static const FString InvalidPropertyPath = TEXT("INVALID_PROPERTY_PATH");
	static const FString InvalidPropertyValue = TEXT("INVALID_PROPERTY_VALUE");
	static const FString NotTextWidget = TEXT("NOT_TEXT_WIDGET");
	// Material errors
	static const FString MaterialNotFound = TEXT("MATERIAL_NOT_FOUND");
	static const FString InstanceNotFound = TEXT("INSTANCE_NOT_FOUND");
	static const FString CollectionNotFound = TEXT("COLLECTION_NOT_FOUND");
	static const FString ParameterNotFound = TEXT("PARAMETER_NOT_FOUND");
	static const FString InvalidConnection = TEXT("INVALID_CONNECTION");
	static const FString InvalidParameter = TEXT("INVALID_PARAMETER");
	static const FString LimitExceeded = TEXT("LIMIT_EXCEEDED");
	static const FString AssetAlreadyExists = TEXT("ASSET_ALREADY_EXISTS");
}

/** Result of a command execution */
struct CORTEXCORE_API FCortexCommandResult
{
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Data;
	FString ErrorCode;
	FString ErrorMessage;
	TSharedPtr<FJsonObject> ErrorDetails;
	TArray<FString> Warnings;
};
