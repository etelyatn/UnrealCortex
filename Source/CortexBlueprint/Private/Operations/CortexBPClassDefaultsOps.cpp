#include "Operations/CortexBPClassDefaultsOps.h"

#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexBlueprintModule.h"
#include "CortexPropertyUtils.h"
#include "CortexSerializer.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
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

	static bool IsBareComponentReference(const FString& Value)
	{
		return !Value.IsEmpty()
			&& !Value.Contains(TEXT("/"))
			&& !Value.Contains(TEXT("."))
			&& !Value.Contains(TEXT(":"));
	}

	static void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty() && !Values.Contains(Value))
		{
			Values.Add(Value);
		}
	}

	static FString StripGenVariableSuffix(const FString& Name)
	{
		static const FString GenVariableSuffix(TEXT("_GEN_VARIABLE"));
		return Name.EndsWith(GenVariableSuffix)
			? Name.LeftChop(GenVariableSuffix.Len())
			: Name;
	}

	static int32 ComputeLevenshteinDistance(const FString& A, const FString& B)
	{
		const int32 LenA = A.Len();
		const int32 LenB = B.Len();
		TArray<int32> Prev;
		TArray<int32> Curr;
		Prev.SetNumUninitialized(LenB + 1);
		Curr.SetNumUninitialized(LenB + 1);

		for (int32 J = 0; J <= LenB; ++J)
		{
			Prev[J] = J;
		}

		for (int32 I = 1; I <= LenA; ++I)
		{
			Curr[0] = I;
			for (int32 J = 1; J <= LenB; ++J)
			{
				const int32 Cost = (FChar::ToLower(A[I - 1]) == FChar::ToLower(B[J - 1])) ? 0 : 1;
				Curr[J] = FMath::Min3(
					Prev[J] + 1,
					Curr[J - 1] + 1,
					Prev[J - 1] + Cost);
			}
			Swap(Prev, Curr);
		}

		return Prev[LenB];
	}

	static FString FindClosestCandidate(const FString& Input, const TArray<FString>& Candidates, int32 MaxDistance)
	{
		FString Closest;
		int32 BestDistance = TNumericLimits<int32>::Max();
		for (const FString& Candidate : Candidates)
		{
			const int32 Distance = ComputeLevenshteinDistance(Input, Candidate);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Closest = Candidate;
			}
		}

		return BestDistance <= MaxDistance ? Closest : FString();
	}

	static void CollectResolverContext(
		UBlueprint* Blueprint,
		TArray<FString>& OutSCSNodesInBlueprint,
		TArray<FString>& OutNativeDefaultSubobjects,
		TArray<FString>& OutCandidatePool)
	{
		if (!Blueprint)
		{
			return;
		}

		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}

				const FString NodeName = Node->GetVariableName().ToString();
				AddUniqueString(OutSCSNodesInBlueprint, NodeName);
				AddUniqueString(OutCandidatePool, NodeName);
			}
		}

		for (UClass* ClassCursor = Blueprint->ParentClass.Get(); ClassCursor; ClassCursor = ClassCursor->GetSuperClass())
		{
			UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(ClassCursor);
			if (!ParentBPGC || !ParentBPGC->SimpleConstructionScript)
			{
				continue;
			}

			for (USCS_Node* ParentNode : ParentBPGC->SimpleConstructionScript->GetAllNodes())
			{
				if (ParentNode)
				{
					AddUniqueString(OutCandidatePool, ParentNode->GetVariableName().ToString());
				}
			}
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		AActor* ActorCDO = BPGC ? Cast<AActor>(BPGC->GetDefaultObject(false)) : nullptr;
		if (ActorCDO)
		{
			TInlineComponentArray<UActorComponent*> Components;
			ActorCDO->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}

				const FString RawName = Component->GetName();
				const FString StrippedName = StripGenVariableSuffix(RawName);
				if (RawName == StrippedName)
				{
					AddUniqueString(OutNativeDefaultSubobjects, RawName);
				}

				AddUniqueString(OutCandidatePool, RawName);
				AddUniqueString(OutCandidatePool, StrippedName);
			}
		}

		OutSCSNodesInBlueprint.Sort();
		OutNativeDefaultSubobjects.Sort();
		OutCandidatePool.Sort();
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static TSharedPtr<FJsonObject> BuildBareNameTypeMismatchDetails(
		UBlueprint* Blueprint,
		const FString& PropertyName,
		const FString& InputValue,
		const FString& ExpectedType,
		const FString& FailureReason)
	{
		TArray<FString> SCSNodesInBlueprint;
		TArray<FString> NativeDefaultSubobjects;
		TArray<FString> CandidatePool;
		CollectResolverContext(Blueprint, SCSNodesInBlueprint, NativeDefaultSubobjects, CandidatePool);

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("expected_type"), ExpectedType);
		Details->SetStringField(TEXT("received_value"), JsonValueToString(MakeShared<FJsonValueString>(InputValue)));
		Details->SetArrayField(TEXT("accepted_formats"), ToJsonStringArray({
			TEXT("Bare component name (e.g. OpenSeq)"),
			TEXT("Full object path (e.g. /Game/.../BP.BP_C:Name_GEN_VARIABLE)")
		}));
		Details->SetArrayField(TEXT("scs_nodes_in_blueprint"), ToJsonStringArray(SCSNodesInBlueprint));
		Details->SetArrayField(TEXT("native_default_subobjects"), ToJsonStringArray(NativeDefaultSubobjects));
		if (!FailureReason.IsEmpty())
		{
			Details->SetStringField(TEXT("resolver_failure"), FailureReason);
		}

		const FString ClosestMatch = FindClosestCandidate(InputValue, CandidatePool, 2);
		if (!ClosestMatch.IsEmpty())
		{
			Details->SetStringField(TEXT("closest_match"), ClosestMatch);
			TSharedPtr<FJsonObject> RetryWith = MakeShared<FJsonObject>();
			RetryWith->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
			TSharedPtr<FJsonObject> RetryProps = MakeShared<FJsonObject>();
			RetryProps->SetStringField(PropertyName, ClosestMatch);
			RetryWith->SetObjectField(TEXT("properties"), RetryProps);
			Details->SetObjectField(TEXT("retry_with"), RetryWith);
		}

		return Details;
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

	// Also search one level into struct properties for dot-notation paths
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(*It);
		if (!StructProp || !StructProp->Struct)
		{
			continue;
		}

		for (TFieldIterator<FProperty> MemberIt(StructProp->Struct); MemberIt; ++MemberIt)
		{
			const FString DotPath = FString::Printf(TEXT("%s.%s"), *(*It)->GetName(), *MemberIt->GetName());
			const FString MemberName = MemberIt->GetName();

			// Compare against the leaf name (user might type "bCanEverTick" meaning "PrimaryActorTick.bCanEverTick")
			const int32 Len1 = Name.Len();
			const int32 Len2 = MemberName.Len();

			TArray<int32> NestedPrev;
			TArray<int32> NestedCurr;
			NestedPrev.SetNumUninitialized(Len2 + 1);
			NestedCurr.SetNumUninitialized(Len2 + 1);

			for (int32 J = 0; J <= Len2; ++J)
			{
				NestedPrev[J] = J;
			}

			for (int32 I = 1; I <= Len1; ++I)
			{
				NestedCurr[0] = I;
				for (int32 J = 1; J <= Len2; ++J)
				{
					const int32 Cost =
						(FChar::ToLower(Name[I - 1]) == FChar::ToLower(MemberName[J - 1])) ? 0 : 1;
					NestedCurr[J] = FMath::Min3(
						NestedPrev[J] + 1,
						NestedCurr[J - 1] + 1,
						NestedPrev[J - 1] + Cost);
				}
				Swap(NestedPrev, NestedCurr);
			}

			// Use dot-notation path as the suggestion
			Candidates.Add(TPair<FString, int32>(DotPath, NestedPrev[Len2]));
		}
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
	if (!Params->TryGetStringField(TEXT("asset_path"), BlueprintPath))
	{
		Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	}
	if (BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
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
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				const TSharedPtr<FJsonObject> TextJson = FCortexSerializer::TextToJson(
					TextProperty->GetPropertyValue(ValuePtr));
				if (TextJson->HasField(TEXT("string_table")))
				{
					PropertyInfo->SetObjectField(TEXT("string_table"), TextJson->GetObjectField(TEXT("string_table")));
				}
			}

			const FString Category = Property->GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				PropertyInfo->SetStringField(TEXT("category"), Category);
			}

			if (UStruct* OwnerStruct = Property->GetOwnerStruct())
			{
				PropertyInfo->SetStringField(TEXT("defined_in"), OwnerStruct->GetName());
			}

			// If property is a struct, add "members" with one level of member info
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
			{
				TSharedPtr<FJsonObject> MembersObj = MakeShared<FJsonObject>();
				const UScriptStruct* ScriptStruct = StructProp->Struct;
				if (ScriptStruct)
				{
					for (TFieldIterator<FProperty> MemberIt(ScriptStruct); MemberIt; ++MemberIt)
					{
						FProperty* MemberProp = *MemberIt;
						if (!MemberProp)
						{
							continue;
						}

						TSharedPtr<FJsonObject> MemberInfo = MakeShared<FJsonObject>();
						MemberInfo->SetStringField(TEXT("type"), MemberProp->GetCPPType());
						MemberInfo->SetStringField(TEXT("path"),
							FString::Printf(TEXT("%s.%s"), *Property->GetName(), *MemberProp->GetName()));

						// Serialize member value
						void* MemberValuePtr = MemberProp->ContainerPtrToValuePtr<void>(ValuePtr);
						const TSharedPtr<FJsonValue> MemberValue = FCortexSerializer::PropertyToJson(MemberProp, MemberValuePtr);
						MemberInfo->SetField(TEXT("value"),
							MemberValue.IsValid() ? MemberValue : MakeShared<FJsonValueNull>());
						if (const FTextProperty* MemberTextProperty = CastField<FTextProperty>(MemberProp))
						{
							const TSharedPtr<FJsonObject> TextJson = FCortexSerializer::TextToJson(
								MemberTextProperty->GetPropertyValue(MemberValuePtr));
							if (TextJson->HasField(TEXT("string_table")))
							{
								MemberInfo->SetObjectField(TEXT("string_table"), TextJson->GetObjectField(TEXT("string_table")));
							}
						}

						MembersObj->SetObjectField(MemberProp->GetName(), MemberInfo);
					}
				}
				PropertyInfo->SetObjectField(TEXT("members"), MembersObj);
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
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				const TSharedPtr<FJsonObject> TextJson = FCortexSerializer::TextToJson(
					TextProperty->GetPropertyValue(ValuePtr));
				if (TextJson->HasField(TEXT("string_table")))
				{
					PropertyInfo->SetObjectField(TEXT("string_table"), TextJson->GetObjectField(TEXT("string_table")));
				}
			}

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
	if (!Params->TryGetStringField(TEXT("asset_path"), BlueprintPath))
	{
		Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	}
	if (BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
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

		const auto RollbackAppliedChanges = [&AppliedChanges, CDO]()
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
					CDO,
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
			TSharedPtr<FJsonValue> ValueForSet = JsonValue;

			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			const bool bIsComponentProperty = ObjectProperty
				&& ObjectProperty->PropertyClass
				&& ObjectProperty->PropertyClass->IsChildOf(UActorComponent::StaticClass());
			if (bIsComponentProperty && JsonValue.IsValid() && JsonValue->Type == EJson::String)
			{
				const FString RawInput = JsonValue->AsString();
				if (IsBareComponentReference(RawInput))
				{
					const FCortexBPSCSDiagnostics::FResolveResult ResolveResult =
						FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(Blueprint, RawInput);
					if (ResolveResult.bIsAmbiguous)
					{
						RollbackAppliedChanges();
						TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
						Details->SetArrayField(TEXT("candidates"), ToJsonStringArray(ResolveResult.AmbiguousCandidates));
						return FCortexCommandRouter::Error(
							CortexErrorCodes::AmbiguousComponentReference,
							FString::Printf(TEXT("Bare component reference '%s' is ambiguous"), *RawInput),
							Details);
					}

					if (!ResolveResult.Component)
					{
						RollbackAppliedChanges();
						TSharedPtr<FJsonObject> Details = BuildBareNameTypeMismatchDetails(
							Blueprint,
							PropertyName,
							RawInput,
							Property->GetCPPType(),
							ResolveResult.FailureReason);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::TypeMismatch,
							FString::Printf(
								TEXT("Failed to set property '%s': bare-name component '%s' did not resolve"),
								*PropertyName,
								*RawInput),
							Details);
					}

					if (!ResolveResult.Component->IsA(ObjectProperty->PropertyClass))
					{
						RollbackAppliedChanges();
						TSharedPtr<FJsonObject> Details = BuildBareNameTypeMismatchDetails(
							Blueprint,
							PropertyName,
							RawInput,
							Property->GetCPPType(),
							TEXT("Resolved component type is incompatible with target property"));
						Details->SetStringField(TEXT("resolved_component_class"), ResolveResult.Component->GetClass()->GetName());
						return FCortexCommandRouter::Error(
							CortexErrorCodes::TypeMismatch,
							FString::Printf(
								TEXT("Failed to set property '%s': resolved component '%s' has incompatible type"),
								*PropertyName,
								*RawInput),
							Details);
					}

					// Keep serializer behavior for object properties by converting the bare name to object path.
					ValueForSet = MakeShared<FJsonValueString>(ResolveResult.Component->GetPathName());
				}
			}

			TArray<FString> SetWarnings;
			if (!FCortexSerializer::JsonToProperty(ValueForSet, Property, ValuePtr, CDO, SetWarnings))
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
			FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
			CDO->PostEditChangeProperty(ChangedEvent);

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
