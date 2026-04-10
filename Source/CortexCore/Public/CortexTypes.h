
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
	static const FString SubgraphNotFound = TEXT("SUBGRAPH_NOT_FOUND");
	static const FString SubgraphDepthExceeded = TEXT("SUBGRAPH_DEPTH_EXCEEDED");
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
	static const FString FontNotFound = TEXT("FONT_NOT_FOUND");
	// Material errors
	static const FString MaterialNotFound = TEXT("MATERIAL_NOT_FOUND");
	static const FString InstanceNotFound = TEXT("INSTANCE_NOT_FOUND");
	static const FString CollectionNotFound = TEXT("COLLECTION_NOT_FOUND");
	static const FString ParameterNotFound = TEXT("PARAMETER_NOT_FOUND");
	static const FString InvalidConnection = TEXT("INVALID_CONNECTION");
	static const FString InvalidParameter = TEXT("INVALID_PARAMETER");
	static const FString LimitExceeded = TEXT("LIMIT_EXCEEDED");
	static const FString AssetAlreadyExists = TEXT("ASSET_ALREADY_EXISTS");
	static const FString SaveFailed = TEXT("SAVE_FAILED");
	static const FString EditorNotAvailable = TEXT("EDITOR_NOT_AVAILABLE");
	static const FString NoDiskFile = TEXT("NO_DISK_FILE");
	static const FString InvalidGlob = TEXT("INVALID_GLOB");
	static const FString NoMatches = TEXT("NO_MATCHES");
	// Dynamic material instance errors
	static const FString NotDynamicInstance = TEXT("NOT_DYNAMIC_INSTANCE");
	static const FString AlreadyDynamicInstance = TEXT("ALREADY_DYNAMIC_INSTANCE");
	static const FString AmbiguousComponent = TEXT("AMBIGUOUS_COMPONENT");
	// Blueprint SCS migration safety
	static const FString PotentialDataLoss = TEXT("POTENTIAL_DATA_LOSS");
	static const FString AmbiguousComponentReference = TEXT("AMBIGUOUS_COMPONENT_REFERENCE");
	// Level errors
	static const FString ActorNotFound = TEXT("ACTOR_NOT_FOUND");
	static const FString AmbiguousActor = TEXT("AMBIGUOUS_ACTOR");
	static const FString ClassNotFound = TEXT("CLASS_NOT_FOUND");
	static const FString ComponentNotFound = TEXT("COMPONENT_NOT_FOUND");
	static const FString ComponentRemoveDenied = TEXT("COMPONENT_REMOVE_DENIED");
	static const FString PropertyNotFound = TEXT("PROPERTY_NOT_FOUND");
	static const FString PropertyNotEditable = TEXT("PROPERTY_NOT_EDITABLE");
	static const FString TypeMismatch = TEXT("TYPE_MISMATCH");
	static const FString SublevelNotFound = TEXT("SUBLEVEL_NOT_FOUND");
	static const FString LevelInUse = TEXT("LEVEL_IN_USE");
	static const FString UnsavedChanges = TEXT("UNSAVED_CHANGES");
	static const FString EditorBusy = TEXT("EDITOR_BUSY");
	static const FString SourceControlError = TEXT("SOURCE_CONTROL_ERROR");
	static const FString DataLayerNotFound = TEXT("DATA_LAYER_NOT_FOUND");
	static const FString SpawnFailed = TEXT("SPAWN_FAILED");
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
	// QA errors
	static const FString NavigationFailed = TEXT("NAVIGATION_FAILED");
	static const FString InteractionFailed = TEXT("INTERACTION_FAILED");
	static const FString ConditionTimeout = TEXT("CONDITION_TIMEOUT");
	static const FString AssertionFailed = TEXT("ASSERTION_FAILED");
	static const FString InvalidCondition = TEXT("INVALID_CONDITION");
	static const FString UnsupportedType = TEXT("UNSUPPORTED_TYPE");
	static const FString MovementMethodUnavailable = TEXT("MOVEMENT_METHOD_UNAVAILABLE");
	// QA recording/replay errors
	static const FString SessionBusy = TEXT("SESSION_BUSY");
	static const FString MapMismatch = TEXT("MAP_MISMATCH");
	static const FString SessionNotFound = TEXT("SESSION_NOT_FOUND");
	static const FString ReplayCancelled = TEXT("REPLAY_CANCELLED");
	// Reflect errors
	static const FString SymbolNotFound = TEXT("SYMBOL_NOT_FOUND");
	// Gen errors
	static const FString ProviderNotFound = TEXT("PROVIDER_NOT_FOUND");
	static const FString CapabilityNotSupported = TEXT("CAPABILITY_NOT_SUPPORTED");
	static const FString JobNotFound = TEXT("JOB_NOT_FOUND");
	static const FString JobNotRetryable = TEXT("JOB_NOT_RETRYABLE");
	static const FString JobLimitReached = TEXT("JOB_LIMIT_REACHED");
	static const FString ProviderError = TEXT("PROVIDER_ERROR");
	static const FString DownloadFailed = TEXT("DOWNLOAD_FAILED");
	static const FString ImportFailed = TEXT("IMPORT_FAILED");
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

	void AddContext(const FString& Key, const FString& Value)
	{
		if (!ErrorDetails.IsValid())
		{
			ErrorDetails = MakeShared<FJsonObject>();
		}
		ErrorDetails->SetStringField(Key, Value);
	}

	void AddContext(const FString& Key, const TArray<FString>& Values)
	{
		if (!ErrorDetails.IsValid())
		{
			ErrorDetails = MakeShared<FJsonObject>();
		}

		const int32 MaxEntries = 20;
		const int32 Count = FMath::Min(Values.Num(), MaxEntries);
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Values[Index]));
		}

		ErrorDetails->SetArrayField(Key, JsonValues);

		if (Values.Num() > MaxEntries)
		{
			ErrorDetails->SetBoolField(Key + TEXT("_truncated"), true);
			ErrorDetails->SetNumberField(Key + TEXT("_total"), Values.Num());
		}
	}

	void AddContext(const FString& Key, TSharedPtr<FJsonObject> Value)
	{
		if (!ErrorDetails.IsValid())
		{
			ErrorDetails = MakeShared<FJsonObject>();
		}

		ErrorDetails->SetObjectField(Key, Value);
	}

	void AddContext(const FString& Key, double Value)
	{
		if (!ErrorDetails.IsValid())
		{
			ErrorDetails = MakeShared<FJsonObject>();
		}

		ErrorDetails->SetNumberField(Key, Value);
	}

	void AddContext(const FString& Key, int32 Value)
	{
		AddContext(Key, static_cast<double>(Value));
	}
};

using FDeferredResponseCallback = TFunction<void(FCortexCommandResult)>;
