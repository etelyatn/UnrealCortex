#include "Operations/CortexBPCompareOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	void AddDifference(
		TArray<TSharedPtr<FJsonValue>>& Differences,
		const FString& Section,
		const FString& Item,
		const FString& Message,
		const FString& SourceValue = FString(),
		const FString& TargetValue = FString())
	{
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("section"), Section);
		Diff->SetStringField(TEXT("item"), Item);
		Diff->SetStringField(TEXT("message"), Message);
		if (!SourceValue.IsEmpty() || !TargetValue.IsEmpty())
		{
			Diff->SetStringField(TEXT("source_value"), SourceValue);
			Diff->SetStringField(TEXT("target_value"), TargetValue);
		}
		Differences.Add(MakeShared<FJsonValueObject>(Diff));
	}

	bool WantsSection(const TSet<FString>& Sections, const TCHAR* Name)
	{
		return Sections.Num() == 0 || Sections.Contains(Name);
	}
}

FCortexCommandResult FCortexBPCompareOps::CompareBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString SourcePath;
	FString TargetPath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: source_path"));
	}
	if (!Params->TryGetStringField(TEXT("target_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: target_path"));
	}

	FString LoadError;
	UBlueprint* SourceBP = FCortexBPAssetOps::LoadBlueprint(SourcePath, LoadError);
	if (!SourceBP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	UBlueprint* TargetBP = FCortexBPAssetOps::LoadBlueprint(TargetPath, LoadError);
	if (!TargetBP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	TSet<FString> Sections;
	const TArray<TSharedPtr<FJsonValue>>* SectionsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("sections"), SectionsArray) && SectionsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *SectionsArray)
		{
			Sections.Add(Value->AsString());
		}
	}

	TArray<TSharedPtr<FJsonValue>> Differences;
	int32 TotalChecks = 0;

	if (WantsSection(Sections, TEXT("variables")))
	{
		TMap<FString, const FBPVariableDescription*> SourceVars;
		for (const FBPVariableDescription& Var : SourceBP->NewVariables)
		{
			SourceVars.Add(Var.VarName.ToString(), &Var);
		}

		TMap<FString, const FBPVariableDescription*> TargetVars;
		for (const FBPVariableDescription& Var : TargetBP->NewVariables)
		{
			TargetVars.Add(Var.VarName.ToString(), &Var);
		}

		TSet<FString> VarNames;
		SourceVars.GetKeys(VarNames);
		for (const TPair<FString, const FBPVariableDescription*>& Pair : TargetVars)
		{
			VarNames.Add(Pair.Key);
		}

		for (const FString& VarName : VarNames)
		{
			++TotalChecks;
			const FBPVariableDescription* const* SourceVarPtr = SourceVars.Find(VarName);
			const FBPVariableDescription* const* TargetVarPtr = TargetVars.Find(VarName);
			if (!SourceVarPtr || !TargetVarPtr)
			{
				AddDifference(Differences, TEXT("variables"), VarName,
					TEXT("Variable missing on one side"),
					SourceVarPtr ? TEXT("present") : TEXT("missing"),
					TargetVarPtr ? TEXT("present") : TEXT("missing"));
				continue;
			}

			const FString SourceType = CortexBPTypeUtils::FriendlyTypeName((*SourceVarPtr)->VarType);
			const FString TargetType = CortexBPTypeUtils::FriendlyTypeName((*TargetVarPtr)->VarType);
			if (SourceType != TargetType)
			{
				AddDifference(Differences, TEXT("variables"), VarName,
					FString::Printf(TEXT("Type differs: %s vs %s"), *SourceType, *TargetType),
					SourceType, TargetType);
			}
			if ((*SourceVarPtr)->DefaultValue != (*TargetVarPtr)->DefaultValue)
			{
				AddDifference(Differences, TEXT("variables"), VarName,
					TEXT("Default value differs"),
					(*SourceVarPtr)->DefaultValue,
					(*TargetVarPtr)->DefaultValue);
			}
		}
	}

	if (WantsSection(Sections, TEXT("functions")))
	{
		TSet<FString> FunctionNames;
		for (UEdGraph* Graph : SourceBP->FunctionGraphs)
		{
			if (Graph)
			{
				FunctionNames.Add(Graph->GetName());
			}
		}

		TSet<FString> TargetFunctionNames;
		for (UEdGraph* Graph : TargetBP->FunctionGraphs)
		{
			if (Graph)
			{
				TargetFunctionNames.Add(Graph->GetName());
				FunctionNames.Add(Graph->GetName());
			}
		}

		for (const FString& FunctionName : FunctionNames)
		{
			++TotalChecks;
			const bool bInSource = SourceBP->FunctionGraphs.ContainsByPredicate(
				[&FunctionName](const UEdGraph* Graph) { return Graph && Graph->GetName() == FunctionName; });
			const bool bInTarget = TargetBP->FunctionGraphs.ContainsByPredicate(
				[&FunctionName](const UEdGraph* Graph) { return Graph && Graph->GetName() == FunctionName; });
			if (bInSource != bInTarget)
			{
				AddDifference(Differences, TEXT("functions"), FunctionName,
					TEXT("Function missing on one side"),
					bInSource ? TEXT("present") : TEXT("missing"),
					bInTarget ? TEXT("present") : TEXT("missing"));
			}
		}
	}

	if (WantsSection(Sections, TEXT("components")))
	{
		auto BuildComponentSet = [](UBlueprint* Blueprint)
		{
			TSet<FString> Components;
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{
				return Components;
			}

			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}

				const FString Entry = FString::Printf(TEXT("%s:%s"),
					*Node->GetVariableName().ToString(),
					Node->ComponentClass ? *Node->ComponentClass->GetName() : TEXT("None"));
				Components.Add(Entry);
			}
			return Components;
		};

		TSet<FString> SourceComponents = BuildComponentSet(SourceBP);
		TSet<FString> TargetComponents = BuildComponentSet(TargetBP);
		TSet<FString> AllComponents = SourceComponents;
		for (const FString& Entry : TargetComponents)
		{
			AllComponents.Add(Entry);
		}

		for (const FString& ComponentEntry : AllComponents)
		{
			++TotalChecks;
			const bool bInSource = SourceComponents.Contains(ComponentEntry);
			const bool bInTarget = TargetComponents.Contains(ComponentEntry);
			if (bInSource != bInTarget)
			{
				AddDifference(Differences, TEXT("components"), ComponentEntry,
					TEXT("Component differs"),
					bInSource ? TEXT("present") : TEXT("missing"),
					bInTarget ? TEXT("present") : TEXT("missing"));
			}
		}
	}

	if (WantsSection(Sections, TEXT("cdo")))
	{
		UObject* SourceCDO = SourceBP->GeneratedClass ? SourceBP->GeneratedClass->GetDefaultObject(false) : nullptr;
		UObject* TargetCDO = TargetBP->GeneratedClass ? TargetBP->GeneratedClass->GetDefaultObject(false) : nullptr;
		if (SourceCDO && TargetCDO)
		{
			for (TFieldIterator<FProperty> PropIt(SourceCDO->GetClass()); PropIt; ++PropIt)
			{
				FProperty* SourceProperty = *PropIt;
				if (!SourceProperty || !SourceProperty->HasAnyPropertyFlags(CPF_BlueprintVisible))
				{
					continue;
				}

				FProperty* TargetProperty = TargetCDO->GetClass()->FindPropertyByName(SourceProperty->GetFName());
				if (!TargetProperty || !SourceProperty->SameType(TargetProperty))
				{
					continue;
				}

				const void* SourceValuePtr = SourceProperty->ContainerPtrToValuePtr<void>(SourceCDO);
				const void* TargetValuePtr = TargetProperty->ContainerPtrToValuePtr<void>(TargetCDO);

				++TotalChecks;

				FObjectPropertyBase* SourceObjProp = CastField<FObjectPropertyBase>(SourceProperty);
				FObjectPropertyBase* TargetObjProp = CastField<FObjectPropertyBase>(TargetProperty);
				if (SourceObjProp && TargetObjProp)
				{
					UObject* SourceObj = SourceObjProp->GetObjectPropertyValue(SourceValuePtr);
					UObject* TargetObj = TargetObjProp->GetObjectPropertyValue(TargetValuePtr);
					if ((SourceObj && !IsValid(SourceObj)) || (TargetObj && !IsValid(TargetObj)))
					{
						const FString SourceObjText = SourceObj ? (IsValid(SourceObj) ? SourceObj->GetPathName() : TEXT("<invalid>")) : TEXT("<null>");
						const FString TargetObjText = TargetObj ? (IsValid(TargetObj) ? TargetObj->GetPathName() : TEXT("<invalid>")) : TEXT("<null>");
						AddDifference(Differences, TEXT("cdo"), SourceProperty->GetName(),
							TEXT("CDO property has invalid object reference"),
							SourceObjText,
							TargetObjText);
						continue;
					}

					if (SourceObj != TargetObj)
					{
						const FString SourceObjText = SourceObj ? SourceObj->GetPathName() : TEXT("<null>");
						const FString TargetObjText = TargetObj ? TargetObj->GetPathName() : TEXT("<null>");
						AddDifference(Differences, TEXT("cdo"), SourceProperty->GetName(),
							TEXT("CDO object property differs"),
							SourceObjText,
							TargetObjText);
					}
					continue;
				}

				if (!SourceProperty->Identical(SourceValuePtr, TargetValuePtr))
				{
					FString SourceVal, TargetVal;
					SourceProperty->ExportText_Direct(SourceVal, SourceValuePtr, SourceValuePtr, nullptr, PPF_None);
					TargetProperty->ExportText_Direct(TargetVal, TargetValuePtr, TargetValuePtr, nullptr, PPF_None);
					AddDifference(Differences, TEXT("cdo"), SourceProperty->GetName(),
						TEXT("CDO property differs"), SourceVal, TargetVal);
				}
			}
		}
	}

	const int32 DifferenceCount = Differences.Num();
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("total_checks"), TotalChecks);
	Summary->SetNumberField(TEXT("matches"), FMath::Max(0, TotalChecks - DifferenceCount));
	Summary->SetNumberField(TEXT("differences"), DifferenceCount);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("match"), DifferenceCount == 0);
	Data->SetArrayField(TEXT("differences"), Differences);
	Data->SetObjectField(TEXT("summary"), Summary);
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("target_path"), TargetPath);

	return FCortexCommandRouter::Success(Data);
}
