
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
	static const FString BatchRefResolutionFailed = TEXT("BATCH_REF_RESOLUTION_FAILED");
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
	// Editor / PIE errors
	static const FString PIENotActive = TEXT("PIE_NOT_ACTIVE");
	static const FString PIEAlreadyActive = TEXT("PIE_ALREADY_ACTIVE");
	static const FString PIEAlreadyPaused = TEXT("PIE_ALREADY_PAUSED");
	static const FString PIENotPaused = TEXT("PIE_NOT_PAUSED");
	static const FString PIETransitionInProgress = TEXT("PIE_TRANSITION_IN_PROGRESS");
	static const FString PIETerminated = TEXT("PIE_TERMINATED");
	static const FString PIEModeUnsupported = TEXT("PIE_MODE_UNSUPPORTED");
	static const FString ViewportNotFound = TEXT("VIEWPORT_NOT_FOUND");
	static const FString InputActionNotFound = TEXT("INPUT_ACTION_NOT_FOUND");
	static const FString ScreenshotFailed = TEXT("SCREENSHOT_FAILED");
	static const FString ConsoleCommandFailed = TEXT("CONSOLE_COMMAND_FAILED");
	static const FString InvalidTimeScale = TEXT("INVALID_TIME_SCALE");
	static const FString GameModeNotFound = TEXT("GAME_MODE_NOT_FOUND");
}

/** Result of a command execution */
struct CORTEXCORE_API FCortexCommandResult
{
	bool bSuccess = false;
	bool bIsDeferred = false;
	TSharedPtr<FJsonObject> Data;
	FString ErrorCode;
	FString ErrorMessage;
	TSharedPtr<FJsonObject> ErrorDetails;
	TArray<FString> Warnings;
};

using FDeferredResponseCallback = TFunction<void(FCortexCommandResult)>;
