#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;
struct FBPVariableDescription;
struct FCortexCommandResult;

/**
 * SaveGame variables carry data in player save files. A Blueprint variable that moves to a native property,
 * is renamed, changes type, loses SaveGame or disappears can silently break those saves, so each such change is a
 * finding that must be acknowledged (`save_game:<variable>`) before a migration command proceeds.
 */
class FCortexBPSaveGameGuard
{
public:
	struct FFinding
	{
		FString Variable;
		/** save_game_flag_missing | save_game_name_changed | save_game_type_changed | save_game_removed */
		FString Kind;
		/** Name of the native counterpart; empty for save_game_removed. */
		FString NativeProperty;
		FString Message;

		/** save_game:<variable> */
		FString AcknowledgmentKey() const;
		TSharedRef<FJsonObject> ToJson() const;
	};

	/**
	 * Appends the findings for Variable against its native counterpart (null when the variable is removed without
	 * one). Non-SaveGame variables never raise a finding.
	 */
	static void Evaluate(const FBPVariableDescription& Variable, const FProperty* NativeOrNull, TArray<FFinding>& InOutFindings);

	/**
	 * rename_member kind:variable input: a SaveGame variable renamed to NewName raises save_game_name_changed (old name is
	 * Variable.VarName). Renaming to the same name or a non-SaveGame variable raises nothing.
	 */
	static void EvaluateRename(const FBPVariableDescription& Variable, FName NewName, TArray<FFinding>& InOutFindings);

	/** Sorted, de-duplicated acknowledgment keys for the findings. */
	static TArray<FString> RequiredAcknowledgment(const TArray<FFinding>& Findings);

	/**
	 * True when Acknowledged (the caller's `save_game:` keys, already separated from other acknowledgment kinds) equals the
	 * required set exactly. Otherwise POTENTIAL_DATA_LOSS with
	 * details.required_acknowledgment (sorted), details.acknowledged and details.findings.
	 */
	static bool Check(const TArray<FFinding>& Findings, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError);

	/** Whether the Blueprint variable type and the native property type are the same type. */
	static bool SameType(const FBPVariableDescription& Variable, const FProperty* Native);
};
