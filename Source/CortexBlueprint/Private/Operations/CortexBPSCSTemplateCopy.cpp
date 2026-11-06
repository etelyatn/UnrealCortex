#include "Operations/CortexBPSCSTemplateCopy.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace CortexSCSTemplateCopyPrivate
{
	/** Placement and creation bookkeeping the engine owns; it is not template data. */
	bool IsEngineOwned(const FName Name)
	{
		static const TSet<FName> Names = {
			FName(TEXT("AttachParent")), FName(TEXT("AttachSocketName")), FName(TEXT("CreationMethod")),
			FName(TEXT("UCSSerializationIndex")), FName(TEXT("bNetAddressable"))};
		return Names.Contains(Name);
	}

	bool IsTransformProperty(const FName Name)
	{
		static const TSet<FName> Names = {FName(TEXT("RelativeLocation")), FName(TEXT("RelativeRotation")), FName(TEXT("RelativeScale3D"))};
		return Names.Contains(Name);
	}

	bool IsDiffCandidate(const FProperty* Property, const UObject* Archetype)
	{
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
			|| IsEngineOwned(Property->GetFName()))
		{
			return false;
		}
		const UClass* Owner = Property->GetOwnerClass();
		return Owner != nullptr && Archetype != nullptr && Archetype->IsA(Owner);
	}

	FString DigestObject(const UObject* Object, const int32 Depth)
	{
		FString Buffer = Object->GetClass()->GetPathName() + TEXT("\n");
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
				|| IsEngineOwned(Property->GetFName()))
			{
				continue;
			}
			Buffer += Property->GetName() + TEXT("=");
			const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
			if (ObjectProperty != nullptr && Property->HasAnyPropertyFlags(CPF_InstancedReference))
			{
				const UObject* Sub = ObjectProperty->GetObjectPropertyValue_InContainer(Object);
				Buffer += Sub == nullptr ? FString(TEXT("null")) : (Depth < 4 ? DigestObject(Sub, Depth + 1) : FString(TEXT("deep")));
			}
			else
			{
				FString Text;
				Property->ExportText_InContainer(0, Text, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
				Buffer += Text;
			}
			Buffer += TEXT("\n");
		}
		return FMD5::HashAnsiString(*Buffer);
	}
}

TSharedRef<FJsonObject> FCortexBPSCSTemplateCopy::FDiff::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("property"), Name.ToString());
	Json->SetStringField(TEXT("source"), SourceText);
	Json->SetStringField(TEXT("target"), TargetText);
	Json->SetBoolField(TEXT("instanced"), bInstanced);
	Json->SetBoolField(TEXT("on_target"), bOnTarget);
	Json->SetBoolField(TEXT("blueprint_declared"), bBlueprintDeclared);
	return Json;
}

void FCortexBPSCSTemplateCopy::CollectDifferences(const UActorComponent* Source, const UClass* TargetClass, const UActorComponent* TargetOrNull, TArray<FDiff>& OutDiffs)
{
	using namespace CortexSCSTemplateCopyPrivate;
	OutDiffs.Reset();
	const UObject* Archetype = Source != nullptr ? Source->GetArchetype() : nullptr;
	if (Archetype == nullptr)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Source->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
	{
		const FProperty* Property = *It;
		if (!IsDiffCandidate(Property, Archetype) || Property->Identical_InContainer(Source, Archetype, 0, PPF_DeepComparison))
		{
			continue;
		}

		FDiff& Diff = OutDiffs.AddDefaulted_GetRef();
		Diff.Name = Property->GetFName();
		Diff.bTransform = IsTransformProperty(Diff.Name);
		Diff.bInstanced = Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference);
		const FProperty* TargetProperty = TargetClass != nullptr ? TargetClass->FindPropertyByName(Diff.Name) : nullptr;
		Diff.bOnTarget = TargetProperty != nullptr && TargetProperty->SameType(Property);
		const UClass* Owner = Property->GetOwnerClass();
		Diff.bBlueprintDeclared = Owner != nullptr && !Owner->HasAnyClassFlags(CLASS_Native);
		if (Diff.bInstanced)
		{
			Diff.SourceText = TEXT("(instanced subobject)");
			Diff.TargetText = TEXT("(instanced subobject)");
		}
		else
		{
			Property->ExportText_InContainer(0, Diff.SourceText, Source, nullptr, const_cast<UActorComponent*>(Source), PPF_None);
			if (TargetOrNull != nullptr && Diff.bOnTarget)
			{
				TargetProperty->ExportText_InContainer(0, Diff.TargetText, TargetOrNull, nullptr, const_cast<UActorComponent*>(TargetOrNull), PPF_None);
			}
		}
	}
	OutDiffs.Sort([](const FDiff& A, const FDiff& B) { return A.Name.LexicalLess(B.Name); });
}

bool FCortexBPSCSTemplateCopy::CopySelected(UActorComponent* Source, UActorComponent* Target, const TSet<FName>& Selected, FString& OutError)
{
	const UObject* Archetype = Source != nullptr ? Source->GetArchetype() : nullptr;
	if (Source == nullptr || Target == nullptr || Archetype == nullptr)
	{
		OutError = TEXT("The source or target template is missing");
		return false;
	}

	// The temporary duplicate owns copies of the source's instanced subobjects. Every property that must stay as the target has
	// it is reset to the archetype value, so the delta copy has nothing to say about it.
	UActorComponent* Temp = DuplicateObject<UActorComponent>(Source, GetTransientPackage());
	if (Temp == nullptr)
	{
		OutError = TEXT("Could not duplicate the source template");
		return false;
	}
	for (TFieldIterator<FProperty> It(Temp->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
	{
		const FProperty* Property = *It;
		const UClass* Owner = Property->GetOwnerClass();
		if (Selected.Contains(Property->GetFName()) || Owner == nullptr || !Archetype->IsA(Owner))
		{
			continue;
		}
		Property->CopyCompleteValue_InContainer(Temp, Archetype);
	}

	UEngine::FCopyPropertiesForUnrelatedObjectsParams Params;
	Params.bDoDelta = true;
	// bClearReferences stays on: it is the pass that swaps references to the temporary's instanced subobjects for their duplicates under Target.
	Params.bNotifyObjectReplacement = false;
	Params.bReplaceObjectClassReferences = false;
	Params.SourceObjectArchetype = const_cast<UObject*>(Archetype);
	Target->Modify();
	UEngine::CopyPropertiesForUnrelatedObjects(Temp, Target, Params);

	Temp->MarkAsGarbage();
	return true;
}

bool FCortexBPSCSTemplateCopy::CopyByName(const UActorComponent* Source, UActorComponent* Target, const TArray<FName>& Names, FString& OutError)
{
	for (const FName Name : Names)
	{
		const FProperty* From = Source != nullptr ? Source->GetClass()->FindPropertyByName(Name) : nullptr;
		const FProperty* To = Target != nullptr ? Target->GetClass()->FindPropertyByName(Name) : nullptr;
		if (From == nullptr || To == nullptr || !To->SameType(From))
		{
			OutError = FString::Printf(TEXT("Property '%s' has no counterpart of the same type on the target class"), *Name.ToString());
			return false;
		}
		if (To->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
		{
			continue;
		}
		Target->Modify();
		To->CopyCompleteValue(To->ContainerPtrToValuePtr<void>(Target), From->ContainerPtrToValuePtr<void>(Source));
	}
	return true;
}

bool FCortexBPSCSTemplateCopy::IsInitialValue(const FProperty* Property, const UObject* Container)
{
	if (Property == nullptr || Container == nullptr)
	{
		return true;
	}
	uint8* Initial = static_cast<uint8*>(FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment()));
	Property->InitializeValue(Initial);
	const bool bEqual = Property->Identical(Property->ContainerPtrToValuePtr<void>(Container), Initial);
	Property->DestroyValue(Initial);
	FMemory::Free(Initial);
	return bEqual;
}

FString FCortexBPSCSTemplateCopy::Digest(const UObject* Object)
{
	return Object != nullptr ? CortexSCSTemplateCopyPrivate::DigestObject(Object, 0) : FString(TEXT("null"));
}
