#include "Operations/CortexSTInspectOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

FCortexCommandResult FCortexSTInspectOps::DumpTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	const bool bIncludeTransitions = CortexST::GetOptionalBool(Params, TEXT("include_transitions"), true);
	const bool bIncludeNodes = CortexST::GetOptionalBool(Params, TEXT("include_nodes"), false);

	UStateTreeState* RootState =
		Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() > 0
			? Context.EditorData->SubTrees[0]
			: nullptr;

	TArray<FCortexSTStateRef> States;
	CortexST::CollectStates(RootState, States);

	TArray<TSharedPtr<FJsonValue>> SerializedStates;
	SerializedStates.Reserve(States.Num());
	for (const FCortexSTStateRef& StateRef : States)
	{
		SerializedStates.Add(MakeShared<FJsonValueObject>(
			CortexST::SerializeState(StateRef, bIncludeTransitions, bIncludeNodes)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Data->SetArrayField(TEXT("states"), SerializedStates);
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Context.StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTInspectOps::GetState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	FCortexSTStateRef StateRef;
	if (!CortexST::ResolveState(Context, Params, StateRef, Error))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = CortexST::SerializeState(StateRef, true, false);
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Context.StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	return FCortexCommandRouter::Success(Data);
}
