#include "Operations/CortexBPClassDefaultsOps.h"

#include "CortexEngineCompat.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexAssetFingerprint.h"
#include "CortexAssetMutationGuard.h"
#include "CortexBatchMutation.h"
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
#include "GameplayTagContainer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif

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

	static TSharedPtr<FJsonObject> CopyClassDefaultsBatchJsonObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (!Source.IsValid())
		{
			return Copy;
		}

		for (const auto& Pair : Source->Values)
		{
			Copy->SetField(Pair.Key, Pair.Value);
		}

		return Copy;
	}

	static FCortexAssetFingerprint MakeClassDefaultsBlueprintFingerprint(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return MakeObjectAssetFingerprint(nullptr);
		}

		return MakeObjectAssetFingerprint(Blueprint, GetTypeHash(static_cast<uint32>(Blueprint->Status)));
	}

	static TSharedPtr<FJsonObject> BuildClassDefaultsBatchResponseData(const FCortexBatchMutationResult& BatchResult)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("status"), BatchResult.Status);
		Data->SetArrayField(TEXT("written_targets"), ToJsonStringArray(BatchResult.WrittenTargets));
		Data->SetArrayField(TEXT("unwritten_targets"), ToJsonStringArray(BatchResult.UnwrittenTargets));

		TArray<TSharedPtr<FJsonValue>> PerItem;
		PerItem.Reserve(BatchResult.PerItem.Num());
		for (const FCortexBatchMutationItemResult& ItemResult : BatchResult.PerItem)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("target"), ItemResult.Target);
			Entry->SetBoolField(TEXT("success"), ItemResult.Result.bSuccess);
			if (ItemResult.Result.Data.IsValid())
			{
				Entry->SetObjectField(TEXT("data"), ItemResult.Result.Data);
			}
			if (!ItemResult.Result.ErrorCode.IsEmpty())
			{
				Entry->SetStringField(TEXT("error_code"), ItemResult.Result.ErrorCode);
			}
			if (!ItemResult.Result.ErrorMessage.IsEmpty())
			{
				Entry->SetStringField(TEXT("error_message"), ItemResult.Result.ErrorMessage);
			}
			if (ItemResult.Result.ErrorDetails.IsValid())
			{
				Entry->SetObjectField(TEXT("error_details"), ItemResult.Result.ErrorDetails);
			}
			if (ItemResult.Result.Warnings.Num() > 0)
			{
				Entry->SetArrayField(TEXT("warnings"), ToJsonStringArray(ItemResult.Result.Warnings));
			}
			PerItem.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Data->SetArrayField(TEXT("per_item"), PerItem);
		return Data;
	}

	static FCortexCommandResult MakeClassDefaultsBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
	{
		TSharedPtr<FJsonObject> Data = BuildClassDefaultsBatchResponseData(BatchResult);
		if (BatchResult.Status == TEXT("committed"))
		{
			return FCortexCommandRouter::Success(Data);
		}

		return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
	}

	static bool ShouldExposePropertyForDiscovery(const FProperty* Property)
	{
		return Property != nullptr
			&& !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_DisableEditOnTemplate)
			&& Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
	}

	static bool IsSupportedDiscoveryProperty(const FProperty* Property)
	{
		return Property != nullptr
			&& !Property->IsA(FDelegateProperty::StaticClass())
			&& !Property->IsA(FMulticastDelegateProperty::StaticClass())
			&& !Property->IsA(FMulticastInlineDelegateProperty::StaticClass())
			&& !Property->IsA(FMulticastSparseDelegateProperty::StaticClass())
			&& !Property->IsA(FWeakObjectProperty::StaticClass())
			&& !Property->IsA(FInt8Property::StaticClass());
	}

	static TArray<TSharedPtr<FJsonValue>> BuildAcceptedFormats(const FProperty* Property)
	{
		TArray<TSharedPtr<FJsonValue>> Formats;

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
			{
				Formats.Add(MakeShared<FJsonValueString>(TEXT("Bare component name")));
				Formats.Add(MakeShared<FJsonValueString>(TEXT("Full object path")));
				return Formats;
			}

			Formats.Add(MakeShared<FJsonValueString>(TEXT("Full object path")));
			return Formats;
		}

		Formats.Add(MakeShared<FJsonValueString>(TEXT("Direct property value")));
		return Formats;
	}

	// Every class-default discovery read shares one bounded metadata window. The discovery_* names
	// never collide with the generic limit/offset selectors, so native discovery paging is never
	// reinterpreted by the cached read layer.
	static constexpr int32 DiscoveryDefaultLimit = 100;
	static constexpr int32 DiscoveryMaxLimit = 200;

	struct FDiscoveryReadControls
	{
		bool bIncludeValues = false;
		bool bIncludeValuesSupplied = false;
		bool bIncludeMembers = false;
		bool bChangedOnly = false;
		int32 Offset = 0;
		int32 Limit = DiscoveryDefaultLimit;
	};

	// Reads an optional boolean control. A field present with a non-boolean JSON type refuses before
	// any property is admitted, so a malformed selector never triggers value work.
	static bool ReadOptionalBoolControl(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		bool& OutValue,
		bool& bOutSupplied,
		FCortexCommandResult& OutError)
	{
		bOutSupplied = false;
		const TSharedPtr<FJsonValue> Found = Params ? Params->TryGetField(Field) : nullptr;
		if (!Found.IsValid())
		{
			return true;
		}
		if (Found->Type != EJson::Boolean)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' must be a boolean"), Field));
			return false;
		}
		OutValue = Found->AsBool();
		bOutSupplied = true;
		return true;
	}

	// Reads an optional integer control within an inclusive range. A present field that is not an
	// integral JSON number in range refuses with InvalidField.
	static bool ReadOptionalIntegerControl(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		bool& bOutSupplied,
		FCortexCommandResult& OutError)
	{
		bOutSupplied = false;
		const TSharedPtr<FJsonValue> Found = Params ? Params->TryGetField(Field) : nullptr;
		if (!Found.IsValid())
		{
			return true;
		}
		if (Found->Type != EJson::Number)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' must be an integer"), Field));
			return false;
		}
		const double RawValue = Found->AsNumber();
		if (!FMath::IsFinite(RawValue)
			|| RawValue != FMath::TruncToDouble(RawValue)
			|| RawValue < static_cast<double>(MinValue)
			|| RawValue > static_cast<double>(MaxValue))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' must be an integer between %d and %d"), Field, MinValue, MaxValue));
			return false;
		}
		OutValue = static_cast<int32>(RawValue);
		bOutSupplied = true;
		return true;
	}

	// Parses the shared include_values/include_members/changed_only/discovery_offset/discovery_limit
	// controls before any property is admitted. Malformed flags and out-of-range limits refuse.
	static bool ParseDiscoveryReadControls(
		const TSharedPtr<FJsonObject>& Params,
		FDiscoveryReadControls& OutControls,
		FCortexCommandResult& OutError)
	{
		OutControls = FDiscoveryReadControls();

		bool bSupplied = false;
		if (!ReadOptionalBoolControl(Params, TEXT("include_values"), OutControls.bIncludeValues, bSupplied, OutError))
		{
			return false;
		}
		OutControls.bIncludeValuesSupplied = bSupplied;
		if (!ReadOptionalBoolControl(Params, TEXT("include_members"), OutControls.bIncludeMembers, bSupplied, OutError))
		{
			return false;
		}
		if (!ReadOptionalBoolControl(Params, TEXT("changed_only"), OutControls.bChangedOnly, bSupplied, OutError))
		{
			return false;
		}
		if (!ReadOptionalIntegerControl(Params, TEXT("discovery_offset"), 0, TNumericLimits<int32>::Max(),
			OutControls.Offset, bSupplied, OutError))
		{
			return false;
		}
		if (!ReadOptionalIntegerControl(Params, TEXT("discovery_limit"), 1, DiscoveryMaxLimit,
			OutControls.Limit, bSupplied, OutError))
		{
			return false;
		}
		return true;
	}

	// Reads the optional 'properties' selector. A present 'properties' must be an array of strings; a
	// non-array value or a non-string element refuses, never silently degrading to an unfiltered read.
	static bool ReadOptionalPropertySelector(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutSelectors,
		FCortexCommandResult& OutError)
	{
		if (!Params || !Params->HasField(TEXT("properties")))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
		if (!Params->TryGetArrayField(TEXT("properties"), Found) || Found == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("'properties' must be an array of strings"));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Found)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'properties' must be an array of strings"));
				return false;
			}
			OutSelectors.Add(Value->AsString());
		}
		return true;
	}

	// Settable discovery admits every exposed, serializable root; inherited discovery additionally
	// excludes properties owned by the Blueprint's own generated class.
	static bool IsAdmittedDiscoveryRoot(
		UBlueprintGeneratedClass* GeneratedClass,
		FProperty* RootProperty,
		bool bInheritedOnly)
	{
		if (!RootProperty
			|| !ShouldExposePropertyForDiscovery(RootProperty)
			|| !IsSupportedDiscoveryProperty(RootProperty))
		{
			return false;
		}
		if (bInheritedOnly && RootProperty->GetOwnerStruct() == GeneratedClass)
		{
			return false;
		}
		return true;
	}

	// Resolves the top-level root FProperty of a selected selector path so discovery scope can be
	// enforced on the root before any leaf is accepted.
	static FProperty* ResolveDiscoveryRootProperty(UBlueprintGeneratedClass* GeneratedClass, const FString& Selector)
	{
		if (!GeneratedClass || Selector.IsEmpty())
		{
			return nullptr;
		}
		const int32 Dot = Selector.Find(TEXT("."), ESearchCase::CaseSensitive);
		const FString RootName = Dot == INDEX_NONE ? Selector : Selector.Left(Dot);
		return FindFProperty<FProperty>(GeneratedClass, FName(*RootName));
	}

	// Resolves the parent-layout field of an admitted root. An inherited baseline only exists when the
	// parent class owns the very same field instance (inherited, never a same-named subclass field)
	// with a compatible type at the identical offset of the parent instance layout, so a subclass field
	// is never read through parent memory. Anything else is a subclass-owned root with no baseline.
	static FProperty* ResolveParentBaselineProperty(const FProperty* RootProperty, UClass* ParentClass)
	{
		if (!RootProperty || !ParentClass)
		{
			return nullptr;
		}

		FProperty* ParentProperty = ParentClass->FindPropertyByName(RootProperty->GetFName());
		if (!ParentProperty
			|| ParentProperty != RootProperty
			|| !ParentProperty->SameType(RootProperty)
			|| ParentProperty->GetOwnerStruct() != RootProperty->GetOwnerStruct()
			|| ParentProperty->GetOffset_ForInternal() != RootProperty->GetOffset_ForInternal()
			|| !ParentClass->IsChildOf(RootProperty->GetOwnerStruct()))
		{
			return nullptr;
		}

		return ParentProperty;
	}

	// Resolves the parent value an admitted root (or a selected leaf path) is compared against. The
	// baseline is only used when the parent layout owns the same field instance at the same offset, so
	// an absent baseline means the root is subclass-owned and must be reported as an effective value.
	static void* ResolveClassDefaultsBaselineValue(
		UObject* BaselineCDO,
		const FString& Selector,
		const FProperty* ValueProperty,
		const FProperty* RootProperty)
	{
		if (!BaselineCDO || !ValueProperty || !RootProperty)
		{
			return nullptr;
		}

		if (!ResolveParentBaselineProperty(RootProperty, BaselineCDO->GetClass()))
		{
			return nullptr;
		}

		FProperty* BaselineProperty = nullptr;
		void* BaselineValuePtr = nullptr;
		if (!FCortexPropertyUtils::ResolvePropertyPath(BaselineCDO, Selector, BaselineProperty, BaselineValuePtr))
		{
			return nullptr;
		}

		if (BaselineProperty != ValueProperty
			|| !BaselineProperty->SameType(ValueProperty)
			|| BaselineProperty->GetOffset_ForInternal() != ValueProperty->GetOffset_ForInternal())
		{
			return nullptr;
		}

		return BaselineValuePtr;
	}

	// Emits the discovery envelope for a metadata page: the honest admitted population size plus the
	// window actually returned. Completeness is only claimed when the page reaches the population end.
	static void AddDiscoveryPagination(
		const TSharedPtr<FJsonObject>& Data,
		int32 TotalProperties,
		int32 Offset,
		int32 Limit,
		int32 Returned)
	{
		const bool bComplete = (Offset + Returned) >= TotalProperties;
		Data->SetNumberField(TEXT("total_properties"), TotalProperties);

		TSharedPtr<FJsonObject> Pagination = MakeShared<FJsonObject>();
		Pagination->SetNumberField(TEXT("offset"), Offset);
		Pagination->SetNumberField(TEXT("limit"), Limit);
		Pagination->SetNumberField(TEXT("returned"), Returned);
		if (bComplete)
		{
			Pagination->SetField(TEXT("next_offset"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Pagination->SetNumberField(TEXT("next_offset"), Offset + Returned);
		}
		Pagination->SetBoolField(TEXT("is_complete"), bComplete);
		Data->SetObjectField(TEXT("discovery_pagination"), Pagination);
	}

	// Builds the single struct-member representation shared by every class-default read: one level of
	// member path/type metadata, with member values extracted only when values were requested.
	static TSharedPtr<FJsonObject> BuildStructMembersInfo(FProperty* Property, void* ValuePtr, bool bIncludeValues)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Property);
		if (!StructProp)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> MembersObj = MakeShared<FJsonObject>();
		const UScriptStruct* ScriptStruct = StructProp->Struct;
		if (!ScriptStruct)
		{
			return MembersObj;
		}

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

			if (bIncludeValues)
			{
				void* MemberValuePtr = MemberProp->ContainerPtrToValuePtr<void>(ValuePtr);
				const TSharedPtr<FJsonValue> MemberValue =
					FCortexSerializer::PropertyToJson(MemberProp, MemberValuePtr);
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
			}

			MembersObj->SetObjectField(MemberProp->GetName(), MemberInfo);
		}

		return MembersObj;
	}

	// Metadata discovery keeps type/category/defined_in/accepted_formats, extracts no value unless
	// requested, and expands struct members only when requested (member values only with values).
	static TSharedPtr<FJsonObject> BuildDiscoveryPropertyInfo(
		FProperty* Property,
		void* ValuePtr,
		bool bIncludeValues,
		bool bIncludeMembers)
	{
		TSharedPtr<FJsonObject> PropertyInfo = MakeShared<FJsonObject>();
		PropertyInfo->SetStringField(TEXT("type"), Property->GetCPPType());

		if (bIncludeValues)
		{
			TSharedPtr<FJsonValue> SerializedValue;
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				SerializedValue = ObjectValue
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ObjectValue->GetPathName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			}
			else
			{
				SerializedValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);
			}
			PropertyInfo->SetField(
				TEXT("value"),
				SerializedValue.IsValid() ? SerializedValue : MakeShared<FJsonValueNull>());

			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				const TSharedPtr<FJsonObject> TextJson = FCortexSerializer::TextToJson(
					TextProperty->GetPropertyValue(ValuePtr));
				if (TextJson->HasField(TEXT("string_table")))
				{
					PropertyInfo->SetObjectField(TEXT("string_table"), TextJson->GetObjectField(TEXT("string_table")));
				}
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

		PropertyInfo->SetArrayField(TEXT("accepted_formats"), BuildAcceptedFormats(Property));

		if (bIncludeMembers)
		{
			if (const TSharedPtr<FJsonObject> Members = BuildStructMembersInfo(Property, ValuePtr, bIncludeValues))
			{
				PropertyInfo->SetObjectField(TEXT("members"), Members);
			}
		}

		return PropertyInfo;
	}

	// ========================================================================
	// Task 14: changed-leaf class-default mutation echoes.
	//
	// The echo mode only decides how an applied mutation is reported back; it never changes what is
	// applied, saved or compiled. Precision-sensitive roots (any reflected tree containing a 64-bit
	// integer leaf) additionally get an exact native snapshot so both the change comparison and the
	// rollback never travel through the lossy legacy JSON double encoding.
	// ========================================================================
	enum class EClassDefaultEchoMode : uint8
	{
		Diff,
		Full,
		None
	};

	// Largest magnitude a 64-bit integer can have and still round-trip through the legacy JSON double
	// encoding. Mirrors CortexSTInspectOps's decimal-string convention, implemented locally so this
	// module has no StateTree dependency.
	static constexpr int64 ClassDefaultSafeIntegerBound = 9007199254740991LL;

	static bool IsWideIntegerLeaf(const FProperty* Property)
	{
		return CastField<FInt64Property>(Property) != nullptr
			|| CastField<FUInt64Property>(Property) != nullptr;
	}

	// The legacy serializer encodes these struct types specially rather than as a plain member object
	// (FSoftObjectPath/FGameplayTag -> string, FGameplayTagContainer -> array, FInstancedStruct ->
	// runtime payload object). Their diff is atomic: a changed value is echoed as a complete
	// before/after pair, never as relative leaf paths.
	static bool IsSpecialEncodedStruct(const UScriptStruct* Struct)
	{
		return Struct == FGameplayTag::StaticStruct()
			|| Struct == FGameplayTagContainer::StaticStruct()
			|| Struct == TBaseStructure<FSoftObjectPath>::Get()
			|| Struct == FInstancedStruct::StaticStruct();
	}

	// True when the reflected property tree can hold an integer leaf whose value is not exactly
	// representable by the legacy JSON number encoding. An FInstancedStruct carries a runtime-selected
	// payload type that may be an int64-bearing struct and is deliberately not provable exact, so it is
	// treated as always precision-relevant: the root takes an exact snapshot, native comparison/copy and
	// a runtime-payload echo instead of a rounded JSON comparison.
	static bool PropertyTreeHasWideInteger(const FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}
		if (IsWideIntegerLeaf(Property))
		{
			return true;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return PropertyTreeHasWideInteger(EnumProperty->GetUnderlyingProperty());
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FInstancedStruct::StaticStruct())
			{
				return true;
			}
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				if (PropertyTreeHasWideInteger(*It))
				{
					return true;
				}
			}
			return false;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return PropertyTreeHasWideInteger(ArrayProperty->Inner);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return PropertyTreeHasWideInteger(SetProperty->ElementProp);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			return PropertyTreeHasWideInteger(MapProperty->KeyProp)
				|| PropertyTreeHasWideInteger(MapProperty->ValueProp);
		}
		return false;
	}

	static FString JoinEchoPath(const FString& Prefix, const FString& Child)
	{
		return Prefix.IsEmpty() ? Child : Prefix + TEXT(".") + Child;
	}

	// Serializes a value exactly, keeping the legacy JSON shape except for 64-bit integer leaves
	// outside the exactly representable range, which become exact decimal strings. Every leaf path
	// that resorted to a decimal string is recorded so the caller can advertise its encoding.
	static TSharedPtr<FJsonValue> SerializeEchoValueExact(
		const FProperty* Property,
		const void* Value,
		const FString& Path,
		const TSharedPtr<FJsonObject>& OutEncodings)
	{
		if (!Property || !Value)
		{
			return MakeShared<FJsonValueNull>();
		}

		if (const FInt64Property* Int64Property = CastField<FInt64Property>(Property))
		{
			const int64 Value64 = Int64Property->GetPropertyValue(Value);
			if (Value64 < -ClassDefaultSafeIntegerBound || Value64 > ClassDefaultSafeIntegerBound)
			{
				OutEncodings->SetStringField(Path, TEXT("decimal_string"));
				return MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), static_cast<long long>(Value64)));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(Value64));
		}

		if (const FUInt64Property* UnsignedProperty = CastField<FUInt64Property>(Property))
		{
			const uint64 Value64 = UnsignedProperty->GetPropertyValue(Value);
			if (Value64 > static_cast<uint64>(ClassDefaultSafeIntegerBound))
			{
				OutEncodings->SetStringField(Path, TEXT("decimal_string"));
				return MakeShared<FJsonValueString>(FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Value64)));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(Value64));
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			// A runtime payload struct is unwrapped exactly like the legacy serializer, but its members
			// are encoded exactly (so a runtime int64 leaf becomes a decimal string).
			if (StructProperty->Struct == FInstancedStruct::StaticStruct())
			{
				const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(Value);
				const UScriptStruct* PayloadStruct = Instance ? Instance->GetScriptStruct() : nullptr;
				const uint8* PayloadMemory = Instance ? Instance->GetMemory() : nullptr;
				if (!PayloadStruct || !PayloadMemory)
				{
					return FCortexSerializer::PropertyToJson(Property, Value);
				}
				TSharedPtr<FJsonObject> PayloadObject = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> It(PayloadStruct); It; ++It)
				{
					FProperty* Member = *It;
					PayloadObject->SetField(Member->GetName(), SerializeEchoValueExact(
						Member,
						Member->ContainerPtrToValuePtr<void>(PayloadMemory),
						JoinEchoPath(Path, Member->GetName()),
						OutEncodings));
				}
				PayloadObject->SetStringField(TEXT("_struct_type"), PayloadStruct->GetName());
				return MakeShared<FJsonValueObject>(PayloadObject);
			}

			// Special encodings keep their legacy representation (string/array/...).
			if (IsSpecialEncodedStruct(StructProperty->Struct))
			{
				return FCortexSerializer::PropertyToJson(Property, Value);
			}

			// Plain declared struct: recurse exactly, regardless of integer width, so the atomic
			// ("full") echo and the nested relative-path diff agree on granularity.
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				FProperty* Member = *It;
				Object->SetField(Member->GetName(), SerializeEchoValueExact(
					Member,
					Member->ContainerPtrToValuePtr<void>(Value),
					JoinEchoPath(Path, Member->GetName()),
					OutEncodings));
			}
			return MakeShared<FJsonValueObject>(Object);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper ArrayHelper(ArrayProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Elements;
			Elements.Reserve(ArrayHelper.Num());
			for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
			{
				Elements.Add(SerializeEchoValueExact(
					ArrayProperty->Inner,
					ArrayHelper.GetRawPtr(Index),
					JoinEchoPath(Path, FString::Printf(TEXT("%d"), Index)),
					OutEncodings));
			}
			return MakeShared<FJsonValueArray>(Elements);
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper SetHelper(SetProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Elements;
			for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
			{
				if (!SetHelper.IsValidIndex(Index))
				{
					continue;
				}
				Elements.Add(SerializeEchoValueExact(
					SetProperty->ElementProp,
					SetHelper.GetElementPtr(Index),
					JoinEchoPath(Path, FString::Printf(TEXT("%d"), Elements.Num())),
					OutEncodings));
			}
			return MakeShared<FJsonValueArray>(Elements);
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper MapHelper(MapProperty, Value);
			TSharedPtr<FJsonObject> MapObject = MakeShared<FJsonObject>();
			for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
			{
				if (!MapHelper.IsValidIndex(Index))
				{
					continue;
				}
				FString KeyString;
				MapProperty->KeyProp->ExportTextItem_Direct(KeyString, MapHelper.GetKeyPtr(Index), nullptr, nullptr, PPF_None);
				MapObject->SetField(KeyString, SerializeEchoValueExact(
					MapProperty->ValueProp,
					MapHelper.GetValuePtr(Index),
					JoinEchoPath(Path, KeyString),
					OutEncodings));
			}
			return MakeShared<FJsonValueObject>(MapObject);
		}

		return FCortexSerializer::PropertyToJson(Property, Value);
	}

	// Deep structural comparison of two JSON values. Case-sensitive for strings so a case-only string
	// change is never mistaken for representational equality.
	static bool JsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		if (!Left.IsValid() || !Right.IsValid())
		{
			return Left.IsValid() == Right.IsValid();
		}
		if (Left->Type != Right->Type)
		{
			return false;
		}

		switch (Left->Type)
		{
		case EJson::Null:
			return true;
		case EJson::String:
			// FString::operator== is case-insensitive in Unreal; a case-only change is a real mutation
			// and must never be reported as equal.
			return Left->AsString().Equals(Right->AsString(), ESearchCase::CaseSensitive);
		case EJson::Number:
			return Left->AsNumber() == Right->AsNumber();
		case EJson::Boolean:
			return Left->AsBool() == Right->AsBool();
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& LeftArray = Left->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& RightArray = Right->AsArray();
			if (LeftArray.Num() != RightArray.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < LeftArray.Num(); ++Index)
			{
				if (!JsonValuesEqual(LeftArray[Index], RightArray[Index]))
				{
					return false;
				}
			}
			return true;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>& LeftObject = Left->AsObject();
			const TSharedPtr<FJsonObject>& RightObject = Right->AsObject();
			if (!LeftObject.IsValid() || !RightObject.IsValid())
			{
				return LeftObject.IsValid() == RightObject.IsValid();
			}
			if (LeftObject->Values.Num() != RightObject->Values.Num())
			{
				return false;
			}
			for (const auto& Pair : LeftObject->Values)
			{
				const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
				if (!JsonValuesEqual(Pair.Value, RightObject->TryGetField(Key)))
				{
					return false;
				}
			}
			return true;
		}
		default:
			return false;
		}
	}

	// Emits one entry per changed leaf of a declared struct by comparing the legacy JSON objects
	// member by member. Nested structs recurse only when the legacy representation is itself an
	// object, so special struct encodings stay atomic.
	static void DiffStructLeavesJson(
		const UScriptStruct* Struct,
		const TSharedPtr<FJsonObject>& Previous,
		const TSharedPtr<FJsonObject>& Current,
		const FString& Prefix,
		const TSharedPtr<FJsonObject>& OutChanges)
	{
		if (!Struct)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Member = *It;
			const FString MemberName = Member->GetName();
			const FString Path = JoinEchoPath(Prefix, MemberName);
			const TSharedPtr<FJsonValue> PreviousValue = Previous.IsValid() ? Previous->TryGetField(MemberName) : nullptr;
			const TSharedPtr<FJsonValue> CurrentValue = Current.IsValid() ? Current->TryGetField(MemberName) : nullptr;

			if (const FStructProperty* NestedStruct = CastField<FStructProperty>(Member))
			{
				const TSharedPtr<FJsonObject>* PreviousObject = nullptr;
				const TSharedPtr<FJsonObject>* CurrentObject = nullptr;
				if (!IsSpecialEncodedStruct(NestedStruct->Struct)
					&& PreviousValue.IsValid() && CurrentValue.IsValid()
					&& PreviousValue->TryGetObject(PreviousObject) && PreviousObject != nullptr
					&& CurrentValue->TryGetObject(CurrentObject) && CurrentObject != nullptr)
				{
					DiffStructLeavesJson(NestedStruct->Struct, *PreviousObject, *CurrentObject, Path, OutChanges);
					continue;
				}
			}

			if (JsonValuesEqual(PreviousValue, CurrentValue))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("previous_value"),
				PreviousValue.IsValid() ? PreviousValue : MakeShared<FJsonValueNull>());
			Entry->SetField(TEXT("new_value"),
				CurrentValue.IsValid() ? CurrentValue : MakeShared<FJsonValueNull>());
			OutChanges->SetObjectField(Path, Entry);
		}
	}

	// Emits one entry per changed leaf of a declared struct using the exact native comparison and the
	// exact-encoded before/after values, so a precision-sensitive leaf is never reported as equal.
	static void DiffStructLeavesNative(
		const FStructProperty* StructProperty,
		const void* PreviousContainer,
		const void* CurrentContainer,
		const FString& Prefix,
		const TSharedPtr<FJsonObject>& OutChanges,
		const TSharedPtr<FJsonObject>& OutEncodings)
	{
		if (!StructProperty)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* Member = *It;
			const void* PreviousValue = Member->ContainerPtrToValuePtr<void>(PreviousContainer);
			const void* CurrentValue = Member->ContainerPtrToValuePtr<void>(CurrentContainer);
			const FString Path = JoinEchoPath(Prefix, Member->GetName());

			if (const FStructProperty* NestedStruct = CastField<FStructProperty>(Member))
			{
				// Recurse into every normally object-encoded nested struct, regardless of integer width,
				// so the diff granularity does not depend on an unrelated int64 sibling. Special
				// encodings stay atomic.
				if (!IsSpecialEncodedStruct(NestedStruct->Struct))
				{
					DiffStructLeavesNative(NestedStruct, PreviousValue, CurrentValue, Path, OutChanges, OutEncodings);
					continue;
				}
			}

			if (Member->Identical(PreviousValue, CurrentValue, PPF_None))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("previous_value"),
				SerializeEchoValueExact(Member, PreviousValue, Path, OutEncodings));
			Entry->SetField(TEXT("new_value"),
				SerializeEchoValueExact(Member, CurrentValue, Path, OutEncodings));
			OutChanges->SetObjectField(Path, Entry);
		}
	}

	// Owns one exact native copy of a precision-sensitive root taken before assignment. Object
	// references inside the copied tree (and the owning CDO) are retained through FGCObject so they
	// stay valid for the whole synchronous mutation loop. Alignment and destruction match the source
	// property exactly; no class-sized or parent-CDO-layout buffer is ever used.
	class FCortexClassDefaultValueSnapshot : public FGCObject
	{
	public:
		FCortexClassDefaultValueSnapshot(FProperty* InProperty, const void* InValuePtr, UObject* InOwner)
			: Property(InProperty)
			, Owner(InOwner)
		{
			if (!Property)
			{
				return;
			}

			Storage = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			Property->InitializeValue(Storage);
			if (InValuePtr)
			{
				Property->CopyCompleteValue(Storage, InValuePtr);
			}

			// Collect the reachable object references once from the frozen copy. The snapshot is never
			// mutated after capture, so the reference set is stable for the whole synchronous loop, and
			// the references are held as TObjectPtr handles so they survive incremental GC.
			CollectObjectReferences(Property, Storage, ReferencedObjects);
		}

		FCortexClassDefaultValueSnapshot(const FCortexClassDefaultValueSnapshot&) = delete;
		FCortexClassDefaultValueSnapshot& operator=(const FCortexClassDefaultValueSnapshot&) = delete;

		virtual ~FCortexClassDefaultValueSnapshot() override
		{
			if (Property && Storage)
			{
				Property->DestroyValue(Storage);
			}
			if (Storage)
			{
				FMemory::Free(Storage);
				Storage = nullptr;
			}
		}

		const void* GetValuePtr() const
		{
			return Storage;
		}

		// Restores the exact captured native value onto the live property value pointer.
		void RestoreTo(void* TargetValuePtr) const
		{
			if (Property && Storage && TargetValuePtr)
			{
				Property->CopyCompleteValue(TargetValuePtr, Storage);
			}
		}

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			// TObjectPtr handles are passed (never raw UObject*) so the collector's handle-aware
			// overload is selected; the raw-pointer overload is unsafe under incremental GC.
			if (Owner)
			{
				Collector.AddReferencedObject(Owner);
			}
			for (TObjectPtr<UObject>& Object : ReferencedObjects)
			{
				Collector.AddReferencedObject(Object);
			}
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FCortexClassDefaultValueSnapshot");
		}

	private:
		// Walks the same reflected struct/container tree as the serializer and gathers every object
		// reference as a TObjectPtr handle. Populated once from the frozen snapshot storage.
		static void CollectObjectReferences(
			FProperty* InProperty,
			const void* Value,
			TArray<TObjectPtr<UObject>>& OutObjects)
		{
			if (!InProperty || !Value)
			{
				return;
			}

			if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(InProperty))
			{
				TObjectPtr<UObject> Object = ObjectProperty->GetObjectPtrPropertyValue(Value);
				if (Object)
				{
					OutObjects.Add(Object);
				}
				return;
			}

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
			{
				// Walk the runtime payload of an instanced struct, not its internal declared members.
				if (StructProperty->Struct == FInstancedStruct::StaticStruct())
				{
					const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(Value);
					const UScriptStruct* PayloadStruct = Instance ? Instance->GetScriptStruct() : nullptr;
					const uint8* PayloadMemory = Instance ? Instance->GetMemory() : nullptr;
					if (PayloadStruct && PayloadMemory)
					{
						for (TFieldIterator<FProperty> It(PayloadStruct); It; ++It)
						{
							CollectObjectReferences(*It, It->ContainerPtrToValuePtr<void>(PayloadMemory), OutObjects);
						}
					}
					return;
				}

				for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
				{
					CollectObjectReferences(*It, (*It)->ContainerPtrToValuePtr<void>(Value), OutObjects);
				}
				return;
			}

			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
			{
				FScriptArrayHelper ArrayHelper(ArrayProperty, Value);
				for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
				{
					CollectObjectReferences(ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index), OutObjects);
				}
				return;
			}

			if (const FSetProperty* SetProperty = CastField<FSetProperty>(InProperty))
			{
				FScriptSetHelper SetHelper(SetProperty, Value);
				for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
				{
					if (SetHelper.IsValidIndex(Index))
					{
						CollectObjectReferences(SetProperty->ElementProp, SetHelper.GetElementPtr(Index), OutObjects);
					}
				}
				return;
			}

			if (const FMapProperty* MapProperty = CastField<FMapProperty>(InProperty))
			{
				FScriptMapHelper MapHelper(MapProperty, Value);
				for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
				{
					if (MapHelper.IsValidIndex(Index))
					{
						CollectObjectReferences(MapProperty->KeyProp, MapHelper.GetKeyPtr(Index), OutObjects);
						CollectObjectReferences(MapProperty->ValueProp, MapHelper.GetValuePtr(Index), OutObjects);
					}
				}
			}
		}

		FProperty* Property = nullptr;
		void* Storage = nullptr;
		TObjectPtr<UObject> Owner;
		TArray<TObjectPtr<UObject>> ReferencedObjects;
	};

	// Advertises how the emitted echoes were encoded: a precision-sensitive scalar advertises a
	// singular value_encoding, struct/atomic collections advertise per-path value_encodings.
	static void ApplyEchoEncodings(
		const FProperty* Property,
		const TSharedPtr<FJsonObject>& Encodings,
		const TSharedPtr<FJsonObject>& OutRecord)
	{
		if (!Encodings.IsValid() || Encodings->Values.Num() == 0)
		{
			return;
		}

		if (IsWideIntegerLeaf(Property))
		{
			OutRecord->SetStringField(TEXT("value_encoding"), TEXT("decimal_string"));
		}
		else
		{
			OutRecord->SetObjectField(TEXT("value_encodings"), Encodings);
		}
	}

	// Fills the per-property result record with the mode-appropriate echo. `changed` survives in every
	// mode; only the value representation differs. When `Snapshot` is present the change decision and
	// the values come from the exact native storage instead of the lossy legacy JSON.
	static void BuildClassDefaultEchoDiff(
		EClassDefaultEchoMode Mode,
		const FProperty* Property,
		const void* CurrentValuePtr,
		const FCortexClassDefaultValueSnapshot* Snapshot,
		const TSharedPtr<FJsonValue>& PreviousValue,
		const TSharedPtr<FJsonValue>& CurrentValue,
		const TSharedPtr<FJsonObject>& OutRecord)
	{
		const bool bHasSnapshot = Snapshot != nullptr && Snapshot->GetValuePtr() != nullptr;

		bool bChanged = false;
		if (bHasSnapshot)
		{
			bChanged = !Property->Identical(Snapshot->GetValuePtr(), CurrentValuePtr, PPF_None);
		}
		else
		{
			bChanged = !JsonValuesEqual(PreviousValue, CurrentValue);
		}
		OutRecord->SetBoolField(TEXT("changed"), bChanged);

		if (Mode == EClassDefaultEchoMode::None)
		{
			return;
		}

		const bool bExactEcho = bHasSnapshot && PropertyTreeHasWideInteger(Property);

		if (Mode == EClassDefaultEchoMode::Full)
		{
			if (bExactEcho)
			{
				TSharedPtr<FJsonObject> Encodings = MakeShared<FJsonObject>();
				OutRecord->SetField(TEXT("previous_value"),
					SerializeEchoValueExact(Property, Snapshot->GetValuePtr(), FString(), Encodings));
				OutRecord->SetField(TEXT("new_value"),
					SerializeEchoValueExact(Property, CurrentValuePtr, FString(), Encodings));
				ApplyEchoEncodings(Property, Encodings, OutRecord);
				return;
			}

			OutRecord->SetField(TEXT("previous_value"),
				PreviousValue.IsValid() ? PreviousValue : MakeShared<FJsonValueNull>());
			OutRecord->SetField(TEXT("new_value"),
				CurrentValue.IsValid() ? CurrentValue : MakeShared<FJsonValueNull>());
			return;
		}

		// Diff mode: only a real change earns a value echo.
		if (!bChanged)
		{
			return;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			// Special-encoded struct roots (FSoftObjectPath/FGameplayTag/FGameplayTagContainer/
			// FInstancedStruct) have no plain member object to diff. They are emitted as one atomic
			// changed value by the scalar/atomic path below, so the change is never dropped.
			if (!IsSpecialEncodedStruct(StructProperty->Struct))
			{
				TSharedPtr<FJsonObject> Changes = MakeShared<FJsonObject>();
				bool bChangesEmitted = false;
				if (bHasSnapshot)
				{
					TSharedPtr<FJsonObject> Encodings = MakeShared<FJsonObject>();
					DiffStructLeavesNative(
						StructProperty, Snapshot->GetValuePtr(), CurrentValuePtr, FString(), Changes, Encodings);
					if (Changes->Values.Num() > 0)
					{
						OutRecord->SetObjectField(TEXT("changes"), Changes);
						bChangesEmitted = true;
					}
					if (Encodings->Values.Num() > 0)
					{
						OutRecord->SetObjectField(TEXT("value_encodings"), Encodings);
					}
				}
				else
				{
					const TSharedPtr<FJsonObject>* PreviousObject = nullptr;
					const TSharedPtr<FJsonObject>* CurrentObject = nullptr;
					if (PreviousValue.IsValid() && CurrentValue.IsValid()
						&& PreviousValue->TryGetObject(PreviousObject) && PreviousObject != nullptr
						&& CurrentValue->TryGetObject(CurrentObject) && CurrentObject != nullptr)
					{
						DiffStructLeavesJson(StructProperty->Struct, *PreviousObject, *CurrentObject, FString(), Changes);
					}
					if (Changes->Values.Num() > 0)
					{
						OutRecord->SetObjectField(TEXT("changes"), Changes);
						bChangesEmitted = true;
					}
				}

				if (bChangesEmitted)
				{
					return;
				}
				// A real change whose leaves could not be represented as relative paths must never be
				// published as an empty complete diff; fall through and emit the complete pair instead.
			}
		}

		// Scalar or atomic-collection root.
		if (bExactEcho)
		{
			TSharedPtr<FJsonObject> Encodings = MakeShared<FJsonObject>();
			OutRecord->SetField(TEXT("previous_value"),
				SerializeEchoValueExact(Property, Snapshot->GetValuePtr(), FString(), Encodings));
			OutRecord->SetField(TEXT("new_value"),
				SerializeEchoValueExact(Property, CurrentValuePtr, FString(), Encodings));
			ApplyEchoEncodings(Property, Encodings, OutRecord);
			return;
		}

		OutRecord->SetField(TEXT("previous_value"),
			PreviousValue.IsValid() ? PreviousValue : MakeShared<FJsonValueNull>());
		OutRecord->SetField(TEXT("new_value"),
			CurrentValue.IsValid() ? CurrentValue : MakeShared<FJsonValueNull>());
	}

	// Validates the optional 'echo' selector before any mutation. Accepts diff (default), full, none.
	static bool ParseClassDefaultEchoMode(
		const TSharedPtr<FJsonObject>& Params,
		EClassDefaultEchoMode& OutMode,
		FCortexCommandResult& OutError)
	{
		OutMode = EClassDefaultEchoMode::Diff;

		const TSharedPtr<FJsonValue> Found = Params ? Params->TryGetField(TEXT("echo")) : nullptr;
		if (!Found.IsValid())
		{
			return true;
		}
		if (Found->Type != EJson::String)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("'echo' must be a string (diff|full|none)"));
			return false;
		}

		const FString Mode = Found->AsString();
		if (Mode == TEXT("diff"))
		{
			OutMode = EClassDefaultEchoMode::Diff;
		}
		else if (Mode == TEXT("full"))
		{
			OutMode = EClassDefaultEchoMode::Full;
		}
		else if (Mode == TEXT("none"))
		{
			OutMode = EClassDefaultEchoMode::None;
		}
		else
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("'echo' must be one of diff|full|none"));
			return false;
		}
		return true;
	}

	static FCortexBatchPreflightResult PreflightBatchSetClassDefaults(const FCortexBatchMutationItem& Item)
	{
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsPathBlocked(Item.Target, BlockReason))
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		}
		FString ValidationError;
		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(Item.Target, ValidationError))
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::InvalidField, ValidationError);
		}

		FString LoadError;
		UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(Item.Target, LoadError);
		if (!Blueprint)
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
		}

		FString LoadedBlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(Blueprint, LoadedBlockReason))
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *LoadedBlockReason));
		}

		return FCortexBatchPreflightResult::Success(MakeClassDefaultsBlueprintFingerprint(Blueprint).ToJson());
	}

	static FCortexCommandResult CommitBatchSetClassDefaults(const FCortexBatchMutationItem& Item)
	{
		TSharedPtr<FJsonObject> ItemParams = CopyClassDefaultsBatchJsonObject(Item.Params);
		ItemParams->SetStringField(TEXT("asset_path"), Item.Target);
		ItemParams->SetStringField(TEXT("blueprint_path"), Item.Target);
		return FCortexBPClassDefaultsOps::SetClassDefaults(ItemParams);
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

FCortexCommandResult FCortexBPClassDefaultsOps::ListDefaults(const TSharedPtr<FJsonObject>& Params, bool bInheritedOnly)
{
	FString BlueprintPath;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("asset_path"), BlueprintPath)
			&& !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		|| BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	// Read controls and the property selector are validated before any asset load or Blueprint compile,
	// so a malformed selector refuses instead of loading, compiling or reporting a lookup failure.
	FDiscoveryReadControls Controls;
	FCortexCommandResult ControlError;
	if (!ParseDiscoveryReadControls(Params, Controls, ControlError))
	{
		return ControlError;
	}

	TArray<FString> Selectors;
	if (!ReadOptionalPropertySelector(Params, Selectors, ControlError))
	{
		return ControlError;
	}

	// The inherited-delta view belongs to get_class_defaults; a list read refuses the flag instead of
	// accepting it and silently returning a metadata page.
	if (Controls.bChangedOnly)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("'changed_only' is only supported by get_class_defaults"));
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

	const auto MakeNotFound = [GeneratedClass](const FString& Selector)
	{
		return MakePropertyNotFoundError(
			GeneratedClass,
			Selector,
			GeneratedClass->GetName(),
			[](UStruct* Struct, const FString& Name, int32 MaxSuggestions)
			{
				return FCortexBPClassDefaultsOps::FindSimilarPropertyNames(Struct, Name, MaxSuggestions);
			});
	};

	TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
	int32 Count = 0;
	int32 TotalProperties = 0;
	bool bPaged = false;

	if (Selectors.Num() > 0)
	{
		// A selector is resolved after root admission and is never silently paged.
		for (const FString& Selector : Selectors)
		{
			if (!IsAdmittedDiscoveryRoot(GeneratedClass, ResolveDiscoveryRootProperty(GeneratedClass, Selector), bInheritedOnly))
			{
				return MakeNotFound(Selector);
			}

			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!FCortexPropertyUtils::ResolvePropertyPath(CDO, Selector, Property, ValuePtr))
			{
				return MakeNotFound(Selector);
			}

			PropertiesObj->SetObjectField(Selector,
				BuildDiscoveryPropertyInfo(Property, ValuePtr, Controls.bIncludeValues, Controls.bIncludeMembers));
			++Count;
		}
	}
	else
	{
		TArray<TPair<FProperty*, void*>> Admitted;
		for (TFieldIterator<FProperty> It(GeneratedClass); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsAdmittedDiscoveryRoot(GeneratedClass, Property, bInheritedOnly))
			{
				continue;
			}
			Admitted.Add(TPair<FProperty*, void*>(Property, Property->ContainerPtrToValuePtr<void>(CDO)));
		}

		TotalProperties = Admitted.Num();
		if (Controls.bIncludeValues)
		{
			// An effective-value read returns the whole admitted population, complete.
			for (const TPair<FProperty*, void*>& Entry : Admitted)
			{
				PropertiesObj->SetObjectField(Entry.Key->GetName(),
					BuildDiscoveryPropertyInfo(Entry.Key, Entry.Value, true, Controls.bIncludeMembers));
			}
			Count = TotalProperties;
		}
		else
		{
			bPaged = true;
			const int32 Begin = FMath::Min(Controls.Offset, TotalProperties);
			const int32 End = FMath::Min(Begin + Controls.Limit, TotalProperties);
			for (int32 Index = Begin; Index < End; ++Index)
			{
				PropertiesObj->SetObjectField(Admitted[Index].Key->GetName(),
					BuildDiscoveryPropertyInfo(Admitted[Index].Key, Admitted[Index].Value, false, Controls.bIncludeMembers));
			}
			Count = End - Begin;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Data->SetStringField(TEXT("class"), GeneratedClass->GetName());
	Data->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT(""));
	Data->SetBoolField(TEXT("values_included"), Controls.bIncludeValues);
	Data->SetObjectField(TEXT("properties"), PropertiesObj);
	Data->SetNumberField(TEXT("count"), Count);
	if (bPaged)
	{
		AddDiscoveryPagination(Data, TotalProperties, Controls.Offset, Controls.Limit, Count);
	}
	else if (Selectors.Num() == 0)
	{
		Data->SetNumberField(TEXT("total_properties"), TotalProperties);
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassDefaultsOps::ListSettableDefaults(const TSharedPtr<FJsonObject>& Params)
{
	return ListDefaults(Params, /*bInheritedOnly=*/false);
}

FCortexCommandResult FCortexBPClassDefaultsOps::ListInheritedProperties(const TSharedPtr<FJsonObject>& Params)
{
	return ListDefaults(Params, /*bInheritedOnly=*/true);
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

	// Read controls and the property selector are validated before any asset load or Blueprint compile,
	// so a malformed selector refuses instead of loading, compiling or reporting a lookup failure.
	FDiscoveryReadControls Controls;
	FCortexCommandResult ControlError;
	if (!ParseDiscoveryReadControls(Params, Controls, ControlError))
	{
		return ControlError;
	}

	TArray<FString> Selectors;
	if (!ReadOptionalPropertySelector(Params, Selectors, ControlError))
	{
		return ControlError;
	}

	// A delta read must carry values; an explicit opt-out refuses before any asset load, compile or
	// lookup instead of returning a delta that declares no values at all.
	if (Controls.bChangedOnly && Controls.bIncludeValuesSupplied && !Controls.bIncludeValues)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("'changed_only' requires 'include_values' to be true"));
	}

	// Member expansion reports the effective struct members, which would contradict the delta view that
	// reports only the members that differ; the combination refuses instead of mixing both meanings.
	if (Controls.bChangedOnly && Controls.bIncludeMembers)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("'include_members' is not supported with 'changed_only'"));
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

	const bool bDiscoveryMode = Selectors.Num() == 0;
	// A nonempty selector is an effective-value read unless the caller opted out explicitly; an
	// unfiltered read defaults to metadata discovery. A delta read always carries values: an explicit
	// opt-out was refused above, and an unstated include_values cannot turn a delta into metadata.
	bool bIncludeValues = Controls.bIncludeValuesSupplied ? Controls.bIncludeValues : !bDiscoveryMode;
	if (Controls.bChangedOnly)
	{
		bIncludeValues = true;
	}

	TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
	int32 Count = 0;
	int32 TotalProperties = 0;
	bool bPaged = false;
	TArray<TSharedPtr<FJsonValue>> OmittedEqualProperties;
	TArray<FCortexSerializationIssue> DeltaIssues;
	// True while every examined root reached a complete verdict: a differing value, or a proven equality.
	bool bEchoComplete = true;

	if (Controls.bChangedOnly)
	{
		// A delta is a view over the admitted population: every admitted root is either reported with the
		// value that differs from its inherited baseline, or listed as an explicit equal omission. A root
		// the parent class does not own compatibly has no baseline, so it is reported as its own effective
		// value: a subclass default is never compared against, or read through, parent memory.
		FCortexSerializationPolicy DeltaPolicy;
		DeltaPolicy.Label = ECortexSerializationPolicyLabel::ReflectedRead;

		UObject* BaselineCDO = Blueprint->ParentClass ? Blueprint->ParentClass->GetDefaultObject() : nullptr;

		const auto AddDeltaEntry = [&](const FString& Key, FProperty* ValueProperty, void* ValuePtr, FProperty* RootProperty)
		{
			void* BaselineValuePtr = ResolveClassDefaultsBaselineValue(BaselineCDO, Key, ValueProperty, RootProperty);

			const FCortexPropertySerializationResult Serialized = BaselineValuePtr
				? FCortexSerializer::PropertyDifferenceToJson(ValueProperty, ValuePtr, BaselineValuePtr, DeltaPolicy, Key)
				: FCortexSerializer::PropertyToJsonDeep(ValueProperty, ValuePtr, DeltaPolicy, Key);
			DeltaIssues.Append(Serialized.Issues);

			// A serialization is authoritative only when it carried no issue and no degraded or partial
			// state. An absent value from a complete serialization means the root genuinely equals its
			// baseline; an absent value from an incomplete one (for example a depth-limited struct
			// comparison) means the comparison could not be completed and must never read as equality.
			const bool bComplete = Serialized.Issues.Num() == 0 && !Serialized.bPartial;
			if (bComplete && !Serialized.JsonValue.IsValid())
			{
				// An equal-to-baseline value is an explicit omission, never a silently missing field.
				OmittedEqualProperties.Add(MakeShared<FJsonValueString>(Key));
				return;
			}

			// A value is only reported when the serialization actually produced one: a null that came with
			// an issue or a partial state is a truncation placeholder (depth-limited or unsupported), not
			// the property's value, so it is never presented as the delta value.
			const bool bUsableValue = Serialized.JsonValue.IsValid()
				&& (!Serialized.JsonValue->IsNull() || bComplete);

			// Member expansion is refused together with changed_only, so a delta entry never mixes
			// effective struct members with a delta value. Values are requested from the entry builder
			// only when the comparison produced a usable one, so an incomplete comparison cannot smuggle
			// an effective value in place of the delta value it failed to produce.
			TSharedPtr<FJsonObject> Entry =
				BuildDiscoveryPropertyInfo(ValueProperty, ValuePtr, bUsableValue, /*bIncludeMembers=*/false);
			if (bUsableValue)
			{
				Entry->SetField(TEXT("value"), Serialized.JsonValue);
			}
			if (!bComplete)
			{
				// The entry keeps its own issues and is flagged incomplete, and the echo declares itself
				// incomplete, so an incomplete comparison is never presented as equality and never claims a
				// value it could not produce.
				bEchoComplete = false;
				Entry->SetBoolField(TEXT("incomplete"), true);
				Entry->SetArrayField(TEXT("issues"), FCortexSerializer::SerializationIssuesToJson(Serialized.Issues));
			}

			PropertiesObj->SetObjectField(Key, Entry);
			++Count;
		};

		if (bDiscoveryMode)
		{
			for (TFieldIterator<FProperty> It(GeneratedClass); It; ++It)
			{
				FProperty* Property = *It;
				if (!ShouldExposePropertyForDiscovery(Property))
				{
					continue;
				}

				AddDeltaEntry(Property->GetName(), Property, Property->ContainerPtrToValuePtr<void>(CDO), Property);
			}
		}
		else
		{
			for (const FString& PropertyName : Selectors)
			{
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

				AddDeltaEntry(PropertyName, Property, ValuePtr,
					ResolveDiscoveryRootProperty(GeneratedClass, PropertyName));
			}
		}

		// Every examined root is either reported or explicitly omitted, so the population stays honest.
		TotalProperties = Count + OmittedEqualProperties.Num();
	}
	else if (bDiscoveryMode)
	{
		TArray<TPair<FProperty*, void*>> Admitted;
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

			Admitted.Add(TPair<FProperty*, void*>(Property, Property->ContainerPtrToValuePtr<void>(CDO)));
		}

		TotalProperties = Admitted.Num();
		if (bIncludeValues)
		{
			// An effective-value discovery read returns the whole admitted population, complete.
			for (const TPair<FProperty*, void*>& Entry : Admitted)
			{
				PropertiesObj->SetObjectField(Entry.Key->GetName(),
					BuildDiscoveryPropertyInfo(Entry.Key, Entry.Value, true, Controls.bIncludeMembers));
			}
			Count = TotalProperties;
		}
		else
		{
			// Metadata discovery returns one bounded page of the admitted population.
			bPaged = true;
			const int32 Begin = FMath::Min(Controls.Offset, TotalProperties);
			const int32 End = FMath::Min(Begin + Controls.Limit, TotalProperties);
			for (int32 Index = Begin; Index < End; ++Index)
			{
				PropertiesObj->SetObjectField(Admitted[Index].Key->GetName(),
					BuildDiscoveryPropertyInfo(Admitted[Index].Key, Admitted[Index].Value, false, Controls.bIncludeMembers));
			}
			Count = End - Begin;
		}
	}
	else
	{
		// Selected property paths preserve their effective values; they are never paged.
		for (const FString& PropertyName : Selectors)
		{
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

			PropertiesObj->SetObjectField(PropertyName,
				BuildDiscoveryPropertyInfo(Property, ValuePtr, bIncludeValues, Controls.bIncludeMembers));
			++Count;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Data->SetStringField(TEXT("class"), GeneratedClass->GetName());
	const FString ParentClassName = Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString();
	Data->SetStringField(TEXT("parent_class"), ParentClassName);
	Data->SetBoolField(TEXT("values_included"), bIncludeValues);
	if (Controls.bChangedOnly)
	{
		// The delta view declares its semantics: values are inherited deltas, never a complete effective
		// read, and every equal-to-baseline root is listed as an explicit omission.
		Data->SetStringField(TEXT("value_mode"), TEXT("inherited_delta"));
		Data->SetStringField(TEXT("baseline_class"), ParentClassName);
		Data->SetBoolField(TEXT("effective_values_complete"), false);
		Data->SetBoolField(TEXT("echo_complete"), bEchoComplete);
		Data->SetArrayField(TEXT("omitted_equal_properties"), OmittedEqualProperties);
		if (DeltaIssues.Num() > 0)
		{
			Data->SetArrayField(TEXT("issues"), FCortexSerializer::SerializationIssuesToJson(DeltaIssues));
		}
	}
	Data->SetObjectField(TEXT("properties"), PropertiesObj);
	Data->SetNumberField(TEXT("count"), Count);
	if (bPaged)
	{
		AddDiscoveryPagination(Data, TotalProperties, Controls.Offset, Controls.Limit, Count);
	}
	else if (bDiscoveryMode)
	{
		Data->SetNumberField(TEXT("total_properties"), TotalProperties);
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Read class defaults from %s (%d properties)"), *BlueprintPath, Count);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPClassDefaultsOps::SetClassDefaults(const TSharedPtr<FJsonObject>& Params)
{
	if (Params.IsValid() && Params->HasField(TEXT("items")))
	{
		FCortexBatchMutationRequest Request;
		FCortexCommandResult ParseError;
		if (!FCortexBatchMutation::ParseRequest(Params, TEXT("asset_path"), Request, ParseError))
		{
			return ParseError;
		}

		return MakeClassDefaultsBatchCommandResult(FCortexBatchMutation::Run(
			Request,
			PreflightBatchSetClassDefaults,
			CommitBatchSetClassDefaults));
	}

	if (Params.IsValid() && Params->HasField(TEXT("expected_fingerprint")))
	{
		TSharedPtr<FJsonObject> RequestParams = Params;
		FString BlueprintPathAlias;
		if (!Params->HasField(TEXT("asset_path")) && Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPathAlias))
		{
			RequestParams = MakeShared<FJsonObject>(*Params);
			RequestParams->SetStringField(TEXT("asset_path"), BlueprintPathAlias);
		}

		FCortexBatchMutationRequest Request;
		FCortexCommandResult ParseError;
		if (!FCortexBatchMutation::ParseRequest(RequestParams, TEXT("asset_path"), Request, ParseError))
		{
			return ParseError;
		}

		const FCortexBatchMutationResult BatchResult = FCortexBatchMutation::Run(
			Request,
			PreflightBatchSetClassDefaults,
			CommitBatchSetClassDefaults);
		if (BatchResult.PerItem.Num() > 0)
		{
			return BatchResult.PerItem[0].Result;
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("No set_class_defaults target was parsed"));
	}

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

	// Echo mode is validated before any mutation so a malformed selector never leaves a partial
	// write behind. It only controls the response representation; the mutation is identical in every
	// mode.
	EClassDefaultEchoMode EchoMode = EClassDefaultEchoMode::Diff;
	FCortexCommandResult EchoError;
	if (!ParseClassDefaultEchoMode(Params, EchoMode, EchoError))
	{
		return EchoError;
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(BlueprintPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
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
			// Exact native copy of a precision-sensitive root, taken before assignment. When present it
			// is the authoritative rollback source; the legacy JSON journal stays for every other root.
			TUniquePtr<FCortexClassDefaultValueSnapshot> Snapshot;
		};
		TArray<FAppliedPropertyChange> AppliedChanges;

		const auto RollbackAppliedChanges = [&AppliedChanges, CDO]()
		{
			for (int32 Index = AppliedChanges.Num() - 1; Index >= 0; --Index)
			{
				FAppliedPropertyChange& Change = AppliedChanges[Index];
				if (!Change.Property || !Change.ValuePtr)
				{
					continue;
				}

				if (Change.Snapshot.IsValid())
				{
					Change.Snapshot->RestoreTo(Change.ValuePtr);
					continue;
				}

				if (!Change.PreviousValue.IsValid())
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

		for (const auto& Entry : (*PropertiesObject)->Values)
		{
			const FString PropertyName = CortexEngineCompat::JsonKeyToString(Entry.Key);
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

			// Capture the exact native value before assignment whenever the root can hold an integer
			// the legacy JSON encoding cannot represent exactly. This backs both the exact echo and the
			// exact rollback in every echo mode.
			TUniquePtr<FCortexClassDefaultValueSnapshot> Snapshot;
			if (PropertyTreeHasWideInteger(Property))
			{
				Snapshot = MakeUnique<FCortexClassDefaultValueSnapshot>(Property, ValuePtr, CDO);
			}

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
			FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
			CDO->PostEditChangeProperty(ChangedEvent);

			const TSharedPtr<FJsonValue> NewValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);

			TSharedPtr<FJsonObject> PropertyResult = MakeShared<FJsonObject>();
			PropertyResult->SetStringField(TEXT("type"), Property->GetCPPType());
			PropertyResult->SetBoolField(TEXT("success"), true);
			BuildClassDefaultEchoDiff(
				EchoMode, Property, ValuePtr, Snapshot.Get(), PreviousValue, NewValue, PropertyResult);

			ResultsObject->SetObjectField(PropertyName, PropertyResult);

			// The snapshot is moved into the journal after the echo is built. It is released with the
			// journal, before the explicit compile/reinstance below, and is the authoritative rollback
			// source for a precision-sensitive root.
			AppliedChanges.Add(FAppliedPropertyChange{
				Property, ValuePtr, PreviousValue, MoveTemp(Snapshot)});
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
