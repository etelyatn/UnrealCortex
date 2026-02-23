#include "Operations/CortexBPTimelineOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CortexBPTimelineOpsPrivate
{
	struct FFloatTrackSpec
	{
		FString Name;
		TArray<TPair<float, float>> Keys;
	};

	struct FVectorTrackSpec
	{
		FString Name;
		TArray<TPair<float, FVector>> Keys;
	};

	bool IsBlueprintCompiled(UBlueprint* Blueprint)
	{
		return Blueprint != nullptr
			&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	TSharedPtr<FJsonObject> BuildBlueprintCompileDetails(UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		TArray<TSharedPtr<FJsonValue>> WarningsArray;

		if (Blueprint == nullptr)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("errors"), ErrorsArray);
			Details->SetArrayField(TEXT("warnings"), WarningsArray);
			return Details;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr || !Node->bHasCompilerMessage)
				{
					continue;
				}

				TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
				MsgObj->SetStringField(TEXT("node"), Node->GetName());
				MsgObj->SetStringField(TEXT("message"), Node->ErrorMsg);
				if (Node->ErrorType <= EMessageSeverity::Error)
				{
					ErrorsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
				else
				{
					WarningsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
			}
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("errors"), ErrorsArray);
		Details->SetArrayField(TEXT("warnings"), WarningsArray);
		return Details;
	}

	bool ParseVectorKey(
		const TSharedPtr<FJsonObject>& KeyObj,
		const FString& TimelineName,
		const FString& TrackName,
		FString& OutError,
		float& OutTime,
		FVector& OutValue)
	{
		double Time = 0.0;
		if (!KeyObj->TryGetNumberField(TEXT("time"), Time))
		{
			OutError = FString::Printf(TEXT("Vector track '%s' key in timeline '%s' is missing 'time'"),
				*TrackName, *TimelineName);
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!KeyObj->TryGetNumberField(TEXT("x"), X)
			|| !KeyObj->TryGetNumberField(TEXT("y"), Y)
			|| !KeyObj->TryGetNumberField(TEXT("z"), Z))
		{
			OutError = FString::Printf(TEXT("Vector track '%s' key in timeline '%s' requires x, y, z"),
				*TrackName, *TimelineName);
			return false;
		}

		OutTime = static_cast<float>(Time);
		OutValue = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
		return true;
	}
}

FCortexCommandResult FCortexBPTimelineOps::ConfigureTimeline(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString TimelineName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("timeline_name"), TimelineName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, timeline_name")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (Timeline == nullptr)
	{
		FScopedTransaction AddTimelineTxn(FText::FromString(
			FString::Printf(TEXT("Cortex: Add Timeline %s"), *TimelineName)));
		Blueprint->Modify();
		Timeline = FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*TimelineName));
		if (Timeline == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Failed to create timeline '%s'"), *TimelineName)
			);
		}
	}

	FScopedTransaction ConfigureTimelineTxn(FText::FromString(
		FString::Printf(TEXT("Cortex: Configure Timeline %s"), *TimelineName)));
	Timeline->Modify();
	Blueprint->Modify();

	double Length = 0.0;
	if (Params->TryGetNumberField(TEXT("length"), Length))
	{
		Timeline->TimelineLength = static_cast<float>(Length);
	}

	bool bLoop = false;
	if (Params->TryGetBoolField(TEXT("loop"), bLoop))
	{
		Timeline->bLoop = bLoop;
	}

	const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
	TArray<CortexBPTimelineOpsPrivate::FFloatTrackSpec> FloatSpecs;
	TArray<CortexBPTimelineOpsPrivate::FVectorTrackSpec> VectorSpecs;
	if (Params->TryGetArrayField(TEXT("tracks"), TracksArray) && TracksArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& TrackValue : *TracksArray)
		{
			const TSharedPtr<FJsonObject>& TrackObj = TrackValue->AsObject();
			if (!TrackObj.IsValid())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Each tracks entry must be an object")
				);
			}

			FString TrackType;
			FString TrackName;
			if (!TrackObj->TryGetStringField(TEXT("type"), TrackType)
				|| !TrackObj->TryGetStringField(TEXT("name"), TrackName))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Each track requires 'type' and 'name'")
				);
			}

			const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
			if (!TrackObj->TryGetArrayField(TEXT("keys"), KeysArray) || KeysArray == nullptr)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Track '%s' is missing keys array"), *TrackName)
				);
			}

			if (TrackType == TEXT("float"))
			{
				CortexBPTimelineOpsPrivate::FFloatTrackSpec TrackSpec;
				TrackSpec.Name = TrackName;

				for (const TSharedPtr<FJsonValue>& KeyValue : *KeysArray)
				{
					const TSharedPtr<FJsonObject>& KeyObj = KeyValue->AsObject();
					if (!KeyObj.IsValid())
					{
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Track '%s' contains non-object key"), *TrackName)
						);
					}

					double KeyTime = 0.0;
					double KeyScalar = 0.0;
					if (!KeyObj->TryGetNumberField(TEXT("time"), KeyTime)
						|| !KeyObj->TryGetNumberField(TEXT("value"), KeyScalar))
					{
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Float track '%s' keys require time and value"), *TrackName)
						);
					}
					TrackSpec.Keys.Add(TPair<float, float>(
						static_cast<float>(KeyTime),
						static_cast<float>(KeyScalar)));
				}

				FloatSpecs.Add(TrackSpec);
				continue;
			}

			if (TrackType == TEXT("vector"))
			{
				CortexBPTimelineOpsPrivate::FVectorTrackSpec TrackSpec;
				TrackSpec.Name = TrackName;

				for (const TSharedPtr<FJsonValue>& KeyValue : *KeysArray)
				{
					const TSharedPtr<FJsonObject>& KeyObj = KeyValue->AsObject();
					if (!KeyObj.IsValid())
					{
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Track '%s' contains non-object key"), *TrackName)
						);
					}

					float KeyTime = 0.0f;
					FVector KeyVector = FVector::ZeroVector;
					FString ParseError;
					if (!CortexBPTimelineOpsPrivate::ParseVectorKey(KeyObj, TimelineName, TrackName, ParseError, KeyTime, KeyVector))
					{
						return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ParseError);
					}
					TrackSpec.Keys.Add(TPair<float, FVector>(KeyTime, KeyVector));
				}

				VectorSpecs.Add(TrackSpec);
				continue;
			}

			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Unsupported track type '%s' (supported: float, vector)"), *TrackType)
			);
		}

		Timeline->FloatTracks.Reset();
		Timeline->VectorTracks.Reset();
		for (const CortexBPTimelineOpsPrivate::FFloatTrackSpec& FloatSpec : FloatSpecs)
		{
			UCurveFloat* Curve = NewObject<UCurveFloat>(
				Blueprint,
				NAME_None,
				RF_Transactional
			);
			Curve->Modify();
			for (const TPair<float, float>& Key : FloatSpec.Keys)
			{
				const FKeyHandle Handle = Curve->FloatCurve.AddKey(Key.Key, Key.Value);
				Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Cubic);
			}

			FTTFloatTrack NewTrack;
			NewTrack.SetTrackName(FName(*FloatSpec.Name), Timeline);
			NewTrack.CurveFloat = Curve;
			Timeline->FloatTracks.Add(NewTrack);
		}

		for (const CortexBPTimelineOpsPrivate::FVectorTrackSpec& VectorSpec : VectorSpecs)
		{
			UCurveVector* Curve = NewObject<UCurveVector>(
				Blueprint,
				NAME_None,
				RF_Transactional
			);
			Curve->Modify();
			for (const TPair<float, FVector>& Key : VectorSpec.Keys)
			{
				Curve->FloatCurves[0].UpdateOrAddKey(Key.Key, Key.Value.X);
				Curve->FloatCurves[1].UpdateOrAddKey(Key.Key, Key.Value.Y);
				Curve->FloatCurves[2].UpdateOrAddKey(Key.Key, Key.Value.Z);
			}

			FTTVectorTrack NewTrack;
			NewTrack.SetTrackName(FName(*VectorSpec.Name), Timeline);
			NewTrack.CurveVector = Curve;
			Timeline->VectorTracks.Add(NewTrack);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!CortexBPTimelineOpsPrivate::IsBlueprintCompiled(Blueprint))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			FString::Printf(TEXT("Blueprint compilation failed after configuring timeline: %s"), *AssetPath),
			CortexBPTimelineOpsPrivate::BuildBlueprintCompileDetails(Blueprint));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("timeline_name"), TimelineName);
	Data->SetNumberField(TEXT("track_count"), Timeline->FloatTracks.Num() + Timeline->VectorTracks.Num());
	Data->SetNumberField(TEXT("length"), Timeline->TimelineLength);
	Data->SetBoolField(TEXT("loop"), Timeline->bLoop);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Configured timeline '%s' on %s with %d tracks"),
		*TimelineName, *AssetPath, Timeline->FloatTracks.Num() + Timeline->VectorTracks.Num());

	return FCortexCommandRouter::Success(Data);
}
