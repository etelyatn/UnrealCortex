#include "Operations/CortexBPClassDefaultsOps.h"

#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexPropertyUtils.h"
#include "CortexSerializer.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"

namespace
{
	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}

		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
		return Output;
	}

	static FCortexCommandResult MakePropertyNotFoundError(
		UStruct* Struct,
		const FString& PropertyName,
		const FString& Context,
		const TFunction<TArray<FString>(UStruct*, const FString&, int32)>& SuggestionFunc)
	{
		TArray<FString> Suggestions = SuggestionFunc(Struct, PropertyName, 3);

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> SuggestionsArr;
		for (const FString& Suggestion : Suggestions)
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(Suggestion));
		}
		Details->SetArrayField(TEXT("suggestions"), SuggestionsArr);

		int32 AvailableCount = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			++AvailableCount;
		}
		Details->SetNumberField(TEXT("available_count"), AvailableCount);

		return FCortexCommandRouter::Error(
			CortexErrorCodes::PropertyNotFound,
			FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Context),
			Details);
	}
}

UObject* FCortexBPClassDefaultsOps::GetBlueprintCDO(UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!GeneratedClass)
		{
			OutError = TEXT("Blueprint has no GeneratedClass after compilation");
			return nullptr;
		}
	}

	UObject* CDO = GeneratedClass->GetDefaultObject(false);
	if (!CDO)
	{
		OutError = TEXT("GeneratedClass exists but CDO is null");
		return nullptr;
	}

	return CDO;
}

TArray<FString> FCortexBPClassDefaultsOps::FindSimilarPropertyNames(
	UStruct* Struct,
	const FString& Name,
	int32 MaxSuggestions)
{
	if (!Struct || Name.IsEmpty() || MaxSuggestions <= 0)
	{
		return TArray<FString>();
	}

	TArray<TPair<FString, int32>> Candidates;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FString PropertyName = It->GetName();
		const int32 Len1 = Name.Len();
		const int32 Len2 = PropertyName.Len();

		TArray<int32> Prev;
		TArray<int32> Curr;
		Prev.SetNumUninitialized(Len2 + 1);
		Curr.SetNumUninitialized(Len2 + 1);

		for (int32 J = 0; J <= Len2; ++J)
		{
			Prev[J] = J;
		}

		for (int32 I = 1; I <= Len1; ++I)
		{
			Curr[0] = I;
			for (int32 J = 1; J <= Len2; ++J)
			{
				const int32 Cost =
					(FChar::ToLower(Name[I - 1]) == FChar::ToLower(PropertyName[J - 1])) ? 0 : 1;
				Curr[J] = FMath::Min3(
					Prev[J] + 1,
					Curr[J - 1] + 1,
					Prev[J - 1] + Cost);
			}
			Swap(Prev, Curr);
		}

		Candidates.Add(TPair<FString, int32>(PropertyName, Prev[Len2]));
	}

	Candidates.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		return A.Value < B.Value;
	});

	TArray<FString> Result;
	for (int32 I = 0; I < FMath::Min(MaxSuggestions, Candidates.Num()); ++I)
	{
		if (Candidates[I].Value <= FMath::Max(3, Name.Len() / 2))
		{
			Result.Add(Candidates[I].Key);
		}
	}

	return Result;
}

FCortexCommandResult FCortexBPClassDefaultsOps::GetClassDefaults(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'blueprint_path' field"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString CDOError;
	UObject* CDO = GetBlueprintCDO(Blueprint, CDOError);
	if (!CDO)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, CDOError);
	}

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			TEXT("Blueprint generated class is missing"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
	Params->TryGetArrayField(TEXT("properties"), PropertiesArray);
	const bool bDiscoveryMode = (PropertiesArray == nullptr || PropertiesArray->Num() == 0);

	TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
	int32 Count = 0;

	if (bDiscoveryMode)
	{
		for (TFieldIterator<FProperty> It(GeneratedClass); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_DisableEditOnTemplate))
			{
				continue;
			}

			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
			TSharedPtr<FJsonObject> PropertyInfo = MakeShared<FJsonObject>();
			PropertyInfo->SetStringField(TEXT("type"), Property->GetCPPType());
			const TSharedPtr<FJsonValue> SerializedValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);
			PropertyInfo->SetField(
				TEXT("value"),
				SerializedValue.IsValid()
					? SerializedValue
					: MakeShared<FJsonValueNull>());

			const FString Category = Property->GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				PropertyInfo->SetStringField(TEXT("category"), Category);
			}

			if (UStruct* OwnerStruct = Property->GetOwnerStruct())
			{
				PropertyInfo->SetStringField(TEXT("defined_in"), OwnerStruct->GetName());
			}

			PropertiesObj->SetObjectField(Property->GetName(), PropertyInfo);
			++Count;
		}
	}
	else
	{
		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
		{
			if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::String)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'properties' must be an array of strings"));
			}

			const FString PropertyName = PropertyValue->AsString();
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!FCortexPropertyUtils::ResolvePropertyPath(CDO, PropertyName, Property, ValuePtr))
			{
				return MakePropertyNotFoundError(
					GeneratedClass,
					PropertyName,
					GeneratedClass->GetName(),
					[](UStruct* Struct, const FString& Name, int32 MaxSuggestions)
					{
						return FCortexBPClassDefaultsOps::FindSimilarPropertyNames(Struct, Name, MaxSuggestions);
					});
			}

			TSharedPtr<FJsonObject> PropertyInfo = MakeShared<FJsonObject>();
			PropertyInfo->SetStringField(TEXT("type"), Property->GetCPPType());
			const TSharedPtr<FJsonValue> SerializedValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);
			PropertyInfo->SetField(
				TEXT("value"),
				SerializedValue.IsValid()
					? SerializedValue
					: MakeShared<FJsonValueNull>());

			const FString Category = Property->GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				PropertyInfo->SetStringField(TEXT("category"), Category);
			}

			if (UStruct* OwnerStruct = Property->GetOwnerStruct())
			{
				PropertyInfo->SetStringField(TEXT("defined_in"), OwnerStruct->GetName());
			}

			PropertiesObj->SetObjectField(PropertyName, PropertyInfo);
			++Count;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Data->SetStringField(TEXT("class"), GeneratedClass->GetName());
	Data->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT(""));
	Data->SetObjectField(TEXT("properties"), PropertiesObj);
	Data->SetNumberField(TEXT("count"), Count);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Read class defaults from %s (%d properties)"), *BlueprintPath, Count);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassDefaultsOps::SetClassDefaults(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'blueprint_path' field"));
	}

	const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObject)
		|| !PropertiesObject
		|| !(*PropertiesObject).IsValid()
		|| (*PropertiesObject)->Values.Num() == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'properties' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString CDOError;
	UObject* CDO = GetBlueprintCDO(Blueprint, CDOError);
	if (!CDO)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, CDOError);
	}

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			TEXT("Blueprint generated class is missing"));
	}

	TSharedPtr<FJsonObject> ResultsObject = MakeShared<FJsonObject>();
	TArray<FString> ResponseWarnings;

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Class Defaults on %s"), *Blueprint->GetName())));

		CDO->Modify();
		struct FAppliedPropertyChange
		{
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			TSharedPtr<FJsonValue> PreviousValue;
		};
		TArray<FAppliedPropertyChange> AppliedChanges;

		const auto RollbackAppliedChanges = [&AppliedChanges]()
		{
			for (int32 Index = AppliedChanges.Num() - 1; Index >= 0; --Index)
			{
				const FAppliedPropertyChange& Change = AppliedChanges[Index];
				if (!Change.Property || !Change.ValuePtr || !Change.PreviousValue.IsValid())
				{
					continue;
				}

				TArray<FString> RestoreWarnings;
				FCortexSerializer::JsonToProperty(
					Change.PreviousValue,
					Change.Property,
					Change.ValuePtr,
					RestoreWarnings);
			}
		};

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PropertiesObject)->Values)
		{
			const FString& PropertyName = Entry.Key;
			const TSharedPtr<FJsonValue>& JsonValue = Entry.Value;

			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!FCortexPropertyUtils::ResolvePropertyPath(CDO, PropertyName, Property, ValuePtr))
			{
				RollbackAppliedChanges();
				return MakePropertyNotFoundError(
					GeneratedClass,
					PropertyName,
					GeneratedClass->GetName(),
					[](UStruct* Struct, const FString& Name, int32 MaxSuggestions)
					{
						return FCortexBPClassDefaultsOps::FindSimilarPropertyNames(Struct, Name, MaxSuggestions);
					});
			}

			if (Property->HasAnyPropertyFlags(CPF_DisableEditOnTemplate))
			{
				RollbackAppliedChanges();
				return FCortexCommandRouter::Error(
					CortexErrorCodes::PropertyNotEditable,
					FString::Printf(
						TEXT("Property '%s' is marked EditInstanceOnly and cannot be set on CDO"),
						*PropertyName));
			}

			if (Property->HasAnyPropertyFlags(CPF_Transient))
			{
				ResponseWarnings.Add(FString::Printf(
					TEXT("Property '%s' is Transient and will not persist after save/load"),
					*PropertyName));
			}

			if (Property->HasAnyPropertyFlags(CPF_Config))
			{
				ResponseWarnings.Add(FString::Printf(
					TEXT("Property '%s' is Config and may be overridden by .ini on next load"),
					*PropertyName));
			}

			const TSharedPtr<FJsonValue> PreviousValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);

			TArray<FString> SetWarnings;
			if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, SetWarnings))
			{
				RollbackAppliedChanges();
				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("expected_type"), Property->GetCPPType());
				Details->SetStringField(TEXT("received_value"), JsonValueToString(JsonValue));

				return FCortexCommandRouter::Error(
					CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("Failed to set property '%s': type mismatch"), *PropertyName),
					Details);
			}

			ResponseWarnings.Append(SetWarnings);
			AppliedChanges.Add({Property, ValuePtr, PreviousValue});

			const TSharedPtr<FJsonValue> NewValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);

			TSharedPtr<FJsonObject> PropertyResult = MakeShared<FJsonObject>();
			PropertyResult->SetStringField(TEXT("type"), Property->GetCPPType());
			PropertyResult->SetField(
				TEXT("previous_value"),
				PreviousValue.IsValid() ? PreviousValue : MakeShared<FJsonValueNull>());
			PropertyResult->SetField(
				TEXT("new_value"),
				NewValue.IsValid() ? NewValue : MakeShared<FJsonValueNull>());
			PropertyResult->SetBoolField(TEXT("success"), true);

			ResultsObject->SetObjectField(PropertyName, PropertyResult);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	TArray<TSharedPtr<FJsonValue>> CompileErrors;

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

		if (!bDidCompile)
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Error)
					{
						TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
						ErrorObject->SetStringField(TEXT("node"), Node->GetName());
						ErrorObject->SetStringField(TEXT("message"), Node->ErrorMsg);
						CompileErrors.Add(MakeShared<FJsonValueObject>(ErrorObject));
					}
				}
			}
		}
	}

	bool bDidSave = false;
	if (bSave)
	{
		UPackage* Package = Blueprint->GetOutermost();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		bDidSave = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Data->SetObjectField(TEXT("results"), ResultsObject);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);
	Data->SetArrayField(TEXT("compile_errors"), CompileErrors);

	TArray<TSharedPtr<FJsonValue>> WarningsArray;
	for (const FString& Warning : ResponseWarnings)
	{
		WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
	}
	Data->SetArrayField(TEXT("warnings"), WarningsArray);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set class defaults on %s (%d properties)"),
		*BlueprintPath, (*PropertiesObject)->Values.Num());

	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	Result.Warnings = MoveTemp(ResponseWarnings);
	return Result;
}
