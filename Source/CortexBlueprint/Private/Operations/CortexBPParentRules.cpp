#include "Operations/CortexBPParentRules.h"

#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexBPAssetOps.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
	bool MatchesShortName(const UClass* Class, const FString& Input)
	{
		const FString Name = Class->GetName();
		const FString Prefix = Class->GetPrefixCPP();
		if (Input == Name || Input == Prefix + Name)
		{
			return true;
		}

		if (Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) && Name.EndsWith(TEXT("_C")))
		{
			const FString Stem = Name.LeftChop(2);
			return Input == Stem || Input == Prefix + Stem;
		}
		return false;
	}
}

bool FCortexBPClassResolver::Resolve(const FString& Input, UClass*& OutClass, FCortexCommandResult& OutError)
{
	OutClass = nullptr;
	if (Input.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Class reference is empty"));
		return false;
	}

	if (Input.Contains(TEXT("/")) || Input.Contains(TEXT(".")))
	{
		if (UClass* Found = FindObject<UClass>(nullptr, *Input))
		{
			OutClass = Found;
			return true;
		}

		FString BlueprintError;
		const UBlueprint* Blueprint = Input.StartsWith(TEXT("/Script/"))
			? nullptr
			: FCortexBPAssetOps::LoadBlueprint(Input, BlueprintError);
		if (Blueprint != nullptr)
		{
			if (Blueprint->GeneratedClass)
			{
				OutClass = Blueprint->GeneratedClass;
				return true;
			}
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ClassNotFound,
				FString::Printf(TEXT("Blueprint has no generated class (compile it first): %s"), *Input));
			return false;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ClassNotFound,
			FString::Printf(TEXT("Class not found: %s"), *Input));
		return false;
	}

	TArray<UClass*> Candidates;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			continue;
		}
		if (MatchesShortName(Class, Input))
		{
			Candidates.Add(Class);
		}
	}

	if (Candidates.Num() == 1)
	{
		OutClass = Candidates[0];
		return true;
	}

	if (Candidates.Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ClassNotFound,
			FString::Printf(TEXT("Class not found: %s"), *Input));
		return false;
	}

	Candidates.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	for (const UClass* Candidate : Candidates)
	{
		Paths.Add(MakeShared<FJsonValueString>(Candidate->GetPathName()));
	}
	Details->SetArrayField(TEXT("candidates"), Paths);
	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::AmbiguousClass,
		FString::Printf(TEXT("Class name '%s' matches %d classes; pass a full class path or Blueprint asset path"), *Input, Candidates.Num()),
		Details);
	return false;
}

EBPFamily FCortexBPFamily::Classify(const UClass* Class)
{
	if (Class != nullptr)
	{
		if (Class->IsChildOf(AActor::StaticClass()))
		{
			return EBPFamily::Actor;
		}
		if (Class->IsChildOf(UActorComponent::StaticClass()))
		{
			return EBPFamily::Component;
		}
		if (Class->IsChildOf(UUserWidget::StaticClass()))
		{
			return EBPFamily::Widget;
		}
		if (Class->IsChildOf(UAnimInstance::StaticClass()))
		{
			return EBPFamily::AnimInstance;
		}
	}
	return EBPFamily::Object;
}

const TCHAR* FCortexBPFamily::Name(EBPFamily Family)
{
	switch (Family)
	{
	case EBPFamily::Actor: return TEXT("Actor");
	case EBPFamily::Component: return TEXT("Component");
	case EBPFamily::Widget: return TEXT("Widget");
	case EBPFamily::AnimInstance: return TEXT("AnimInstance");
	case EBPFamily::Object: break;
	}
	return TEXT("Object");
}

bool FCortexBPFamily::CheckReparent(const UBlueprint* Blueprint, const UClass* NewParent, TSharedPtr<FJsonObject>& OutDetails)
{
	OutDetails = MakeShared<FJsonObject>();
	if (Blueprint == nullptr || NewParent == nullptr)
	{
		OutDetails->SetStringField(TEXT("reason"), TEXT("invalid_class"));
		return false;
	}

	const EBPFamily From = Classify(Blueprint->ParentClass);
	const EBPFamily To = Classify(NewParent);
	OutDetails->SetStringField(TEXT("from_family"), Name(From));
	OutDetails->SetStringField(TEXT("to_family"), Name(To));

	auto Refuse = [&OutDetails](const TCHAR* Reason)
	{
		OutDetails->SetStringField(TEXT("reason"), Reason);
		return false;
	};

	if (NewParent->HasAnyClassFlags(CLASS_Interface))
	{
		return Refuse(TEXT("interface"));
	}

	const UClass* Own = Blueprint->GeneratedClass;
	if ((Own != nullptr && NewParent->IsChildOf(Own))
		|| (Blueprint->SkeletonGeneratedClass != nullptr && NewParent->IsChildOf(Blueprint->SkeletonGeneratedClass)))
	{
		return Refuse(TEXT("self_or_descendant"));
	}

	if (From != To)
	{
		return Refuse(TEXT("family_mismatch"));
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParent))
	{
		return Refuse(TEXT("not_blueprintable"));
	}

	UClass* BlueprintClass = nullptr;
	UClass* GeneratedClass = nullptr;
	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(TEXT("KismetCompiler"))
		.GetBlueprintTypesForClass(const_cast<UClass*>(NewParent), BlueprintClass, GeneratedClass);
	if (BlueprintClass != Blueprint->GetClass())
	{
		return Refuse(TEXT("blueprint_type_mismatch"));
	}

	// Ordinary object Blueprints only convert when they are a plain, normal Blueprint.
	if (To == EBPFamily::Object && Blueprint->BlueprintType != BPTYPE_Normal)
	{
		return Refuse(TEXT("blueprint_type_mismatch"));
	}

	return true;
}
