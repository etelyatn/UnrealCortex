#include "Operations/CortexGraphSymbolResolver.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "UObject/Interface.h"
#include "UObject/UObjectIterator.h"

TSharedRef<FJsonObject> FCortexResolvedSymbol::ToJson() const
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("declaring_class"), DeclaringClassPath);
	Out->SetStringField(TEXT("context_class"), ContextClassPath);
	Out->SetStringField(TEXT("member_name"), MemberName.ToString());
	Out->SetStringField(TEXT("call_kind"), FCortexGraphSymbolResolver::CallKindToString(CallKind));
	Out->SetBoolField(TEXT("is_function"), bIsFunction);
	Out->SetBoolField(TEXT("is_property"), bIsProperty);
	Out->SetBoolField(TEXT("is_pure"), bIsPure);
	Out->SetBoolField(TEXT("is_const"), bIsConst);
	Out->SetBoolField(TEXT("is_blueprint_callable"), bIsBlueprintCallable);
	Out->SetBoolField(TEXT("is_blueprint_pure"), bIsBlueprintPure);
	Out->SetBoolField(TEXT("is_blueprint_visible"), bIsBlueprintVisible);
	Out->SetBoolField(TEXT("is_blueprint_read_only"), bIsBlueprintReadOnly);
	Out->SetBoolField(TEXT("is_protected"), bIsProtected);
	Out->SetBoolField(TEXT("is_private"), bIsPrivate);
	Out->SetBoolField(TEXT("is_static"), bIsStatic);
	return Out;
}

FString FCortexGraphSymbolResolver::CallKindToString(ECortexCallKind Kind)
{
	switch (Kind)
	{
	case ECortexCallKind::Ordinary:
		return TEXT("ordinary");
	case ECortexCallKind::InterfaceMessage:
		return TEXT("interface_message");
	case ECortexCallKind::Parent:
		return TEXT("parent");
	case ECortexCallKind::Unsupported:
	default:
		return TEXT("unsupported");
	}
}

bool FCortexGraphSymbolResolver::TryParseCallKind(const FString& KindString, ECortexCallKind& OutKind)
{
	if (KindString.Equals(TEXT("ordinary"), ESearchCase::IgnoreCase))
	{
		OutKind = ECortexCallKind::Ordinary;
		return true;
	}
	if (KindString.Equals(TEXT("interface_message"), ESearchCase::IgnoreCase))
	{
		OutKind = ECortexCallKind::InterfaceMessage;
		return true;
	}
	if (KindString.Equals(TEXT("parent"), ESearchCase::IgnoreCase))
	{
		OutKind = ECortexCallKind::Parent;
		return true;
	}
	if (KindString.Equals(TEXT("unsupported"), ESearchCase::IgnoreCase))
	{
		OutKind = ECortexCallKind::Unsupported;
		return true;
	}
	return false;
}

bool FCortexGraphSymbolResolver::ResolveClass(
	const FString& ClassIdentifier,
	UClass*& OutClass,
	FCortexCommandResult& OutError,
	TArray<FString>* OutAmbiguityCandidates)
{
	OutClass = nullptr;

	if (ClassIdentifier.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Class identifier is empty"));
		return false;
	}

	// 1. Canonical path resolution (starts with /)
	if (ClassIdentifier.StartsWith(TEXT("/")))
	{
		// Guard against splitting at the first dot:
		// A canonical path is /Script/ModuleName.ClassName or /Game/Path.Asset_C
		const FString PackageName = FPackageName::ObjectPathToPackageName(ClassIdentifier);
		const bool bPackageExists =
			PackageName.StartsWith(TEXT("/"))
			&& (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName));

		if (!bPackageExists)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Class package not found: %s"), *PackageName));
			return false;
		}

		// Try in-memory class first
		if (UClass* FoundClass = FindObject<UClass>(nullptr, *ClassIdentifier))
		{
			OutClass = FoundClass;
			return true;
		}

		// In-memory Blueprint asset
		if (UBlueprint* InMemoryBP = FindObject<UBlueprint>(nullptr, *ClassIdentifier))
		{
			OutClass = InMemoryBP->GeneratedClass;
			if (OutClass)
			{
				return true;
			}
		}

		// Load class from package
		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassIdentifier))
		{
			OutClass = LoadedClass;
			return true;
		}

		// Load Blueprint asset
		if (UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *ClassIdentifier))
		{
			OutClass = LoadedBP->GeneratedClass;
			if (OutClass)
			{
				return true;
			}
		}

		// Try with _C suffix if not present
		if (!ClassIdentifier.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassName = ClassIdentifier + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassName))
			{
				OutClass = GeneratedClass;
				return true;
			}
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Class not found at path: %s"), *ClassIdentifier));
		return false;
	}

	// 2. Short name resolution (ambiguity-aware search across all loaded classes)
	TArray<UClass*> MatchingCandidates;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (!IsValid(Candidate) || Candidate->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		const FString CandidateName = Candidate->GetName();
		FString StrippedCandidate = CandidateName;
		if (CandidateName.EndsWith(TEXT("_C")))
		{
			StrippedCandidate = CandidateName.LeftChop(2);
		}

		const bool bExactMatch =
			CandidateName.Equals(ClassIdentifier, ESearchCase::IgnoreCase)
			|| StrippedCandidate.Equals(ClassIdentifier, ESearchCase::IgnoreCase);

		const bool bPrefixMatch =
			FString::Printf(TEXT("A%s"), *CandidateName).Equals(ClassIdentifier, ESearchCase::IgnoreCase)
			|| FString::Printf(TEXT("U%s"), *CandidateName).Equals(ClassIdentifier, ESearchCase::IgnoreCase)
			|| FString::Printf(TEXT("A%s"), *StrippedCandidate).Equals(ClassIdentifier, ESearchCase::IgnoreCase)
			|| FString::Printf(TEXT("U%s"), *StrippedCandidate).Equals(ClassIdentifier, ESearchCase::IgnoreCase);

		if (bExactMatch || bPrefixMatch)
		{
			MatchingCandidates.AddUnique(Candidate);
		}
	}

	if (MatchingCandidates.Num() > 1)
	{
		if (OutAmbiguityCandidates)
		{
			const int32 Limit = FMath::Min(MatchingCandidates.Num(), 16);
			for (int32 i = 0; i < Limit; ++i)
			{
				OutAmbiguityCandidates->Add(MatchingCandidates[i]->GetPathName());
			}
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CandidateArray;
		const int32 Limit = FMath::Min(MatchingCandidates.Num(), 16);
		for (int32 i = 0; i < Limit; ++i)
		{
			CandidateArray.Add(MakeShared<FJsonValueString>(MatchingCandidates[i]->GetPathName()));
		}
		Details->SetArrayField(TEXT("candidates"), CandidateArray);

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Class identifier '%s' is ambiguous (%d matching classes found)"), *ClassIdentifier, MatchingCandidates.Num()),
			Details);
		return false;
	}

	if (MatchingCandidates.Num() == 1)
	{
		OutClass = MatchingCandidates[0];
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidField,
		FString::Printf(TEXT("Class not found: %s"), *ClassIdentifier));
	return false;
}

bool FCortexGraphSymbolResolver::ParseMemberSelector(
	const TSharedPtr<FJsonObject>& Params,
	const FString& MemberFieldName,
	FString& OutOwnerClass,
	FString& OutMemberName,
	FCortexCommandResult& OutError)
{
	OutOwnerClass.Reset();
	OutMemberName.Reset();

	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is missing"));
		return false;
	}

	FString OwnerClass;
	const bool bHasOwnerClass = Params->TryGetStringField(TEXT("owner_class"), OwnerClass) && !OwnerClass.IsEmpty();

	FString VariableClass;
	const bool bHasVarClass = Params->TryGetStringField(TEXT("variable_class"), VariableClass) && !VariableClass.IsEmpty();

	FString DelegateClass;
	const bool bHasDelegateClass = Params->TryGetStringField(TEXT("delegate_class"), DelegateClass) && !DelegateClass.IsEmpty();

	// Check conflicts between owner_class and variable_class / delegate_class
	if (bHasOwnerClass && bHasVarClass && OwnerClass != VariableClass)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Conflicting owner_class '%s' and variable_class '%s'"), *OwnerClass, *VariableClass));
		return false;
	}
	if (bHasOwnerClass && bHasDelegateClass && OwnerClass != DelegateClass)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Conflicting owner_class '%s' and delegate_class '%s'"), *OwnerClass, *DelegateClass));
		return false;
	}

	if (!bHasOwnerClass)
	{
		if (bHasVarClass)
		{
			OwnerClass = VariableClass;
		}
		else if (bHasDelegateClass)
		{
			OwnerClass = DelegateClass;
		}
	}

	FString RawMember;
	if (!Params->TryGetStringField(MemberFieldName, RawMember) || RawMember.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Missing required field: %s"), *MemberFieldName));
		return false;
	}

	// If owner_class is explicitly provided, raw member must not contain dots
	if (!OwnerClass.IsEmpty())
	{
		if (RawMember.Contains(TEXT(".")))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Conflicting owner_class '%s' and combined selector '%s'"), *OwnerClass, *RawMember));
			return false;
		}
		OutOwnerClass = OwnerClass;
		OutMemberName = RawMember;
		return true;
	}

	// No owner_class: check if raw member is a combined selector
	if (RawMember.Contains(TEXT(".")))
	{
		if (RawMember.StartsWith(TEXT("/")))
		{
			// Canonical path combined selector, e.g. /Script/Engine.KismetSystemLibrary.PrintString
			// Split from end to preserve package.class
			if (RawMember.Split(TEXT("."), &OutOwnerClass, &OutMemberName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				if (!OutOwnerClass.IsEmpty() && !OutMemberName.IsEmpty())
				{
					return true;
				}
			}
		}
		else
		{
			// Short combined selector, e.g. ClassName.MemberName
			TArray<FString> Parts;
			RawMember.ParseIntoArray(Parts, TEXT("."));
			if (Parts.Num() == 2 && !Parts[0].IsEmpty() && !Parts[1].IsEmpty())
			{
				OutOwnerClass = Parts[0];
				OutMemberName = Parts[1];
				return true;
			}
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Malformed member selector '%s'"), *RawMember));
		return false;
	}

	// Bare member name without owner_class (e.g. self-context variable or function)
	OutOwnerClass.Reset();
	OutMemberName = RawMember;
	return true;
}

bool FCortexGraphSymbolResolver::ResolveFunction(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	FCortexResolvedSymbol& OutSymbol,
	FCortexCommandResult& OutError)
{
	FString OwnerClassStr;
	FString MemberNameStr;
	if (!ParseMemberSelector(Params, TEXT("function_name"), OwnerClassStr, MemberNameStr, OutError))
	{
		return false;
	}

	UClass* ContextClass = nullptr;
	if (OwnerClassStr.IsEmpty())
	{
		if (Blueprint)
		{
			ContextClass = Blueprint->SkeletonGeneratedClass
				? Blueprint->SkeletonGeneratedClass
				: Blueprint->GeneratedClass;
		}
		if (!ContextClass)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Missing owner_class for function '%s'"), *MemberNameStr));
			return false;
		}
	}
	else
	{
		if (!ResolveClass(OwnerClassStr, ContextClass, OutError))
		{
			return false;
		}
	}

	UFunction* Func = ContextClass->FindFunctionByName(FName(*MemberNameStr));
	if (!Func)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Function not found: %s on class %s"), *MemberNameStr, *ContextClass->GetName()));
		return false;
	}

	// Validate Blueprint callability, placeability as event, or overridability
	const bool bIsCallable = UEdGraphSchema_K2::CanUserKismetCallFunction(Func);
	const bool bCanBeEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Func);
	const bool bIsOverridable = Func->HasAnyFunctionFlags(FUNC_BlueprintEvent);
	if (!bIsCallable && !bCanBeEvent && !bIsOverridable)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Function '%s' on class '%s' is not callable, placeable, or overridable in Blueprints"), *MemberNameStr, *ContextClass->GetName()));
		return false;
	}

	// Validate access (private/protected)
	UClass* DeclaringClass = Func->GetOwnerClass();
	if (Func->HasAnyFunctionFlags(FUNC_Private))
	{
		const bool bIsBlueprintSelf = Blueprint && (
			DeclaringClass == Blueprint->GeneratedClass
			|| DeclaringClass == Blueprint->SkeletonGeneratedClass);
		if (!bIsBlueprintSelf)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Function '%s' is private to class '%s' and not accessible"), *MemberNameStr, *DeclaringClass->GetName()));
			return false;
		}
	}

	if (Func->GetBoolMetaData(FBlueprintMetadata::MD_Protected))
	{
		UClass* SelfClass = Blueprint
			? (Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass)
			: nullptr;
		if (!SelfClass || !SelfClass->IsChildOf(DeclaringClass))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Function '%s' is protected on class '%s' and not accessible"), *MemberNameStr, *DeclaringClass->GetName()));
			return false;
		}
	}

	// Determine call kind
	ECortexCallKind CallKind = ECortexCallKind::Ordinary;
	if (DeclaringClass->IsChildOf(UInterface::StaticClass()))
	{
		CallKind = ECortexCallKind::InterfaceMessage;
	}

	FString RequestedCallKindStr;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("call_kind"), RequestedCallKindStr))
	{
		ECortexCallKind RequestedKind;
		if (!TryParseCallKind(RequestedCallKindStr, RequestedKind))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unsupported call kind: %s"), *RequestedCallKindStr));
			return false;
		}

		if (RequestedKind == ECortexCallKind::Parent)
		{
			CallKind = ECortexCallKind::Parent;
		}
		else if (RequestedKind != CallKind)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Requested call_kind '%s' does not match function call kind '%s'"),
					*RequestedCallKindStr, *CallKindToString(CallKind)));
			return false;
		}
	}

	OutSymbol.DeclaringClassPath = DeclaringClass->GetPathName();
	OutSymbol.ContextClassPath = ContextClass->GetPathName();
	OutSymbol.MemberName = FName(*MemberNameStr);
	OutSymbol.CallKind = CallKind;
	OutSymbol.bIsFunction = true;
	OutSymbol.bIsProperty = false;
	OutSymbol.bIsPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
	OutSymbol.bIsConst = Func->HasAnyFunctionFlags(FUNC_Const);
	OutSymbol.bIsBlueprintCallable = Func->HasAnyFunctionFlags(FUNC_BlueprintCallable);
	OutSymbol.bIsBlueprintPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
	OutSymbol.bIsProtected = Func->GetBoolMetaData(FBlueprintMetadata::MD_Protected);
	OutSymbol.bIsPrivate = Func->HasAnyFunctionFlags(FUNC_Private);
	OutSymbol.bIsStatic = Func->HasAnyFunctionFlags(FUNC_Static);
	OutSymbol.DeclaringClass = DeclaringClass;
	OutSymbol.ContextClass = ContextClass;
	OutSymbol.Function = Func;
	OutSymbol.Property = nullptr;

	return true;
}

bool FCortexGraphSymbolResolver::ResolveProperty(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	bool bIsWrite,
	FCortexResolvedSymbol& OutSymbol,
	FCortexCommandResult& OutError)
{
	FString OwnerClassStr;
	FString MemberNameStr;
	if (!ParseMemberSelector(Params, TEXT("variable_name"), OwnerClassStr, MemberNameStr, OutError))
	{
		return false;
	}

	UClass* ContextClass = nullptr;

	if (OwnerClassStr.IsEmpty())
	{
		// Self context
		if (!Blueprint)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Missing owner_class for variable '%s' and no Blueprint context provided"), *MemberNameStr));
			return false;
		}

		UClass* SelfClass = Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass;

		// Designer widgets (UMG)
		if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Blueprint))
		{
			if (UWidget* Widget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*MemberNameStr)) : nullptr)
			{
				if (!Widget->bIsVariable)
				{
					OutError = FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Designer widget '%s' has is_variable=false and cannot be referenced from a graph; call umg.set_widget_variable first."), *MemberNameStr));
					return false;
				}

				OutSymbol.DeclaringClassPath = WBP->GeneratedClass ? WBP->GeneratedClass->GetPathName() : WBP->GetPathName();
				OutSymbol.ContextClassPath = OutSymbol.DeclaringClassPath;
				OutSymbol.MemberName = FName(*MemberNameStr);
				OutSymbol.bIsProperty = true;
				OutSymbol.bIsBlueprintVisible = true;
				OutSymbol.ContextClass = SelfClass;
				return true;
			}
		}

		// Blueprint member variables (NewVariables)
		const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*MemberNameStr));
		if (VarIndex != INDEX_NONE)
		{
			const FBPVariableDescription& VarDesc = Blueprint->NewVariables[VarIndex];
			if (bIsWrite && (VarDesc.PropertyFlags & CPF_BlueprintReadOnly))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Property '%s' is BlueprintReadOnly and cannot be written"), *MemberNameStr));
				return false;
			}

			OutSymbol.DeclaringClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : Blueprint->GetPathName();
			OutSymbol.ContextClassPath = OutSymbol.DeclaringClassPath;
			OutSymbol.MemberName = FName(*MemberNameStr);
			OutSymbol.bIsProperty = true;
			OutSymbol.bIsBlueprintVisible = true;
			OutSymbol.bIsBlueprintReadOnly = (VarDesc.PropertyFlags & CPF_BlueprintReadOnly) != 0;
			OutSymbol.ContextClass = SelfClass;
			return true;
		}

		// Reflected property on SelfClass
		if (SelfClass)
		{
			FProperty* Prop = SelfClass->FindPropertyByName(FName(*MemberNameStr));
			if (Prop)
			{
				if (bIsWrite && Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
				{
					OutError = FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Property '%s' is BlueprintReadOnly and cannot be written"), *MemberNameStr));
					return false;
				}

				OutSymbol.DeclaringClassPath = Prop->GetOwnerClass()->GetPathName();
				OutSymbol.ContextClassPath = SelfClass->GetPathName();
				OutSymbol.MemberName = FName(*MemberNameStr);
				OutSymbol.bIsProperty = true;
				OutSymbol.bIsBlueprintVisible = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
				OutSymbol.bIsBlueprintReadOnly = Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
				OutSymbol.DeclaringClass = Prop->GetOwnerClass();
				OutSymbol.ContextClass = SelfClass;
				OutSymbol.Property = Prop;
				return true;
			}
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Self property not found: %s"), *MemberNameStr));
		return false;
	}

	// External class context
	if (!ResolveClass(OwnerClassStr, ContextClass, OutError))
	{
		return false;
	}

	FProperty* Prop = ContextClass->FindPropertyByName(FName(*MemberNameStr));
	if (!Prop)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Property not found: %s on class %s"), *MemberNameStr, *ContextClass->GetName()));
		return false;
	}

	if (bIsWrite && Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Property '%s' on class '%s' is BlueprintReadOnly and cannot be written"), *MemberNameStr, *ContextClass->GetName()));
		return false;
	}

	OutSymbol.DeclaringClassPath = Prop->GetOwnerClass()->GetPathName();
	OutSymbol.ContextClassPath = ContextClass->GetPathName();
	OutSymbol.MemberName = FName(*MemberNameStr);
	OutSymbol.bIsProperty = true;
	OutSymbol.bIsBlueprintVisible = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
	OutSymbol.bIsBlueprintReadOnly = Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
	OutSymbol.DeclaringClass = Prop->GetOwnerClass();
	OutSymbol.ContextClass = ContextClass;
	OutSymbol.Property = Prop;

	return true;
}
