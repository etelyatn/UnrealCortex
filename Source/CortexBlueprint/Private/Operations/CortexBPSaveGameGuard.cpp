#include "Operations/CortexBPSaveGameGuard.h"

#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"

FString FCortexBPSaveGameGuard::FFinding::AcknowledgmentKey() const
{
	return FString::Printf(TEXT("save_game:%s"), *Variable);
}

TSharedRef<FJsonObject> FCortexBPSaveGameGuard::FFinding::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("variable"), Variable);
	Json->SetStringField(TEXT("kind"), Kind);
	Json->SetStringField(TEXT("native_property"), NativeProperty);
	Json->SetStringField(TEXT("acknowledgment"), AcknowledgmentKey());
	Json->SetStringField(TEXT("message"), Message);
	return Json;
}

bool FCortexBPSaveGameGuard::SameType(const FBPVariableDescription& Variable, const FProperty* Native)
{
	if (Native == nullptr)
	{
		return false;
	}

	FEdGraphPinType NativeType;
	if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Native, NativeType))
	{
		return false;
	}

	const FEdGraphPinType& BlueprintType = Variable.VarType;
	return BlueprintType.PinCategory == NativeType.PinCategory
		&& BlueprintType.PinSubCategory == NativeType.PinSubCategory
		&& BlueprintType.PinSubCategoryObject.Get() == NativeType.PinSubCategoryObject.Get()
		&& BlueprintType.ContainerType == NativeType.ContainerType
		&& BlueprintType.PinValueType == NativeType.PinValueType;
}

void FCortexBPSaveGameGuard::Evaluate(const FBPVariableDescription& Variable, const FProperty* NativeOrNull, TArray<FFinding>& InOutFindings)
{
	if ((Variable.PropertyFlags & CPF_SaveGame) == 0)
	{
		return;
	}

	const FString VariableName = Variable.VarName.ToString();
	auto Add = [&](const TCHAR* Kind, const FString& Message)
	{
		FFinding Finding;
		Finding.Variable = VariableName;
		Finding.Kind = Kind;
		Finding.NativeProperty = NativeOrNull ? NativeOrNull->GetName() : FString();
		Finding.Message = Message;
		InOutFindings.Add(MoveTemp(Finding));
	};

	if (NativeOrNull == nullptr)
	{
		Add(TEXT("save_game_removed"),
			FString::Printf(TEXT("SaveGame variable '%s' has no native counterpart; saved values for it are discarded"), *VariableName));
		return;
	}

	if (NativeOrNull->GetFName() != Variable.VarName)
	{
		Add(TEXT("save_game_name_changed"),
			FString::Printf(TEXT("SaveGame variable '%s' becomes '%s'; saves written under the old name no longer load into it"),
				*VariableName, *NativeOrNull->GetName()));
	}
	if (!SameType(Variable, NativeOrNull))
	{
		Add(TEXT("save_game_type_changed"),
			FString::Printf(TEXT("SaveGame variable '%s' changes type on the native property '%s'"), *VariableName, *NativeOrNull->GetName()));
	}
	if (!NativeOrNull->HasAnyPropertyFlags(CPF_SaveGame))
	{
		Add(TEXT("save_game_flag_missing"),
			FString::Printf(TEXT("Native property '%s' is not SaveGame; '%s' would stop being saved"), *NativeOrNull->GetName(), *VariableName));
	}
}

void FCortexBPSaveGameGuard::EvaluateRename(const FBPVariableDescription& Variable, FName NewName, TArray<FFinding>& InOutFindings)
{
	if ((Variable.PropertyFlags & CPF_SaveGame) == 0 || NewName == Variable.VarName)
	{
		return;
	}

	FFinding Finding;
	Finding.Variable = Variable.VarName.ToString();
	Finding.Kind = TEXT("save_game_name_changed");
	Finding.NativeProperty = NewName.ToString();
	Finding.Message = FString::Printf(TEXT("SaveGame variable '%s' is renamed to '%s'; saves written under the old name no longer load into it"),
		*Finding.Variable, *Finding.NativeProperty);
	InOutFindings.Add(MoveTemp(Finding));
}

TArray<FString> FCortexBPSaveGameGuard::RequiredAcknowledgment(const TArray<FFinding>& Findings)
{
	TSet<FString> Unique;
	for (const FFinding& Finding : Findings)
	{
		Unique.Add(Finding.AcknowledgmentKey());
	}

	TArray<FString> Keys = Unique.Array();
	Keys.Sort();
	return Keys;
}

bool FCortexBPSaveGameGuard::Check(const TArray<FFinding>& Findings, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError)
{
	const TArray<FString> Required = RequiredAcknowledgment(Findings);
	TSet<FString> Provided(Acknowledged);
	TArray<FString> ProvidedSorted = Provided.Array();
	ProvidedSorted.Sort();
	if (Required == ProvidedSorted)
	{
		return true;
	}

	auto ToValues = [](const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	};

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("required_acknowledgment"), ToValues(Required));
	Details->SetArrayField(TEXT("acknowledged"), ToValues(ProvidedSorted));
	TArray<TSharedPtr<FJsonValue>> FindingValues;
	for (const FFinding& Finding : Findings)
	{
		FindingValues.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
	}
	Details->SetArrayField(TEXT("findings"), FindingValues);
	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::PotentialDataLoss,
		Required.IsEmpty()
			? TEXT("No SaveGame data is at risk; acknowledged_losses must not carry stale SaveGame keys")
			: TEXT("SaveGame variables would change or disappear; acknowledge exactly the required keys to proceed"),
		Details);
	return false;
}
