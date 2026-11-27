#include "Operations/CortexBPEmbeddedTemplates.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPWidgetUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** Longest instance value echoed in dropped_overrides. */
	constexpr int32 MaxEchoedValueLength = 200;

	/**
	 * The properties umg.get_embedded_widget_diff can show (CPF_Edit, declared below UUserWidget), minus what is never serialized
	 * (transient, deprecated), static arrays, delegates and instanced subobjects (a value copy would alias them).
	 */
	bool IsTrackedProperty(const FProperty* Property)
	{
		const UClass* Owner = Property->GetOwner<UClass>();
		return Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
			&& Property->ArrayDim == 1
			&& !Property->ContainsInstancedObjectProperty()
			&& !CastField<FDelegateProperty>(Property)
			&& !CastField<FMulticastDelegateProperty>(Property)
			&& Owner != nullptr && Owner != UUserWidget::StaticClass() && Owner->IsChildOf(UUserWidget::StaticClass());
	}

	bool IsEmbeddingWidgetBlueprint(const UBlueprint* Blueprint)
	{
		static const UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
		return Blueprint != nullptr && WidgetBlueprintClass != nullptr && Blueprint->IsA(WidgetBlueprintClass);
	}

	struct FInstance
	{
		UBlueprint* Outer = nullptr;
		UUserWidget* Template = nullptr;

		FString Key() const { return Outer->GetPathName() + TEXT("|") + Template->GetName(); }
	};

	/** Templates of exactly Class (reinstanced classes map to their authoritative class) in every other loaded Widget Blueprint. */
	TArray<FInstance> CollectInstances(const UBlueprint* Inner, const UClass* Class)
	{
		TArray<FInstance> Instances;
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* Outer = *It;
			if (Outer == Inner || !IsValid(Outer) || Outer->HasAnyFlags(RF_ClassDefaultObject) || !IsEmbeddingWidgetBlueprint(Outer)
				|| Outer->GetOutermost() == GetTransientPackage() || Outer->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
			{
				continue;
			}
			for (UWidget* Widget : CortexBPWidgetUtils::CollectWidgets(Outer))
			{
				UUserWidget* Template = Cast<UUserWidget>(Widget);
				if (Template != nullptr && IsValid(Template) && Template->GetClass()->GetAuthoritativeClass() == Class)
				{
					Instances.Add({Outer, Template});
				}
			}
		}
		return Instances;
	}

	FString EchoValue(const FProperty* Property, const void* Container)
	{
		// ExportTextItem_* always exports (ExportText_* skips a value equal to the type's zero value when no default is given).
		FString Value;
		Property->ExportTextItem_InContainer(Value, Container, nullptr, nullptr, PPF_None);
		return Value.Len() > MaxEchoedValueLength ? Value.Left(MaxEchoedValueLength) + TEXT("...") : Value;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonNames(const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FName& Name : Names)
		{
			Out.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		return Out;
	}
}

FCortexBPEmbeddedTemplates::FPreState FCortexBPEmbeddedTemplates::Capture(UBlueprint* Blueprint)
{
	FPreState State;
	UClass* Class = Blueprint != nullptr ? Blueprint->GeneratedClass.Get() : nullptr;
	const UObject* Cdo = Class != nullptr ? Class->GetDefaultObject(/*bCreateIfNeeded=*/false) : nullptr;
	if (Cdo == nullptr || !IsEmbeddingWidgetBlueprint(Blueprint) || !Class->IsChildOf(UUserWidget::StaticClass()))
	{
		return State;
	}
	State.bApplicable = true;
	State.Blueprint = Blueprint;

	for (const FInstance& Instance : CollectInstances(Blueprint, Class))
	{
		TSet<FName>& Follows = State.FollowsDefault.FindOrAdd(Instance.Key());
		for (TFieldIterator<FProperty> It(Instance.Template->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			const FProperty* CdoProperty = IsTrackedProperty(Property) ? Class->FindPropertyByName(Property->GetFName()) : nullptr;
			if (CdoProperty == nullptr || !CdoProperty->SameType(Property))
			{
				continue;
			}
			if (Property->Identical(Property->ContainerPtrToValuePtr<void>(Instance.Template), CdoProperty->ContainerPtrToValuePtr<void>(Cdo)))
			{
				Follows.Add(Property->GetFName());
			}
			else
			{
				State.Overrides.Add({Instance.Outer->GetPathName(), Instance.Template->GetName(), Property->GetFName(), EchoValue(Property, Instance.Template)});
			}
		}
	}
	return State;
}

void FCortexBPEmbeddedTemplates::ReportInto(
	const FPreState& Before,
	bool bClassCurrent,
	const UClass* NewParent,
	bool bResync,
	bool bSaveResynced,
	bool bReportDropped,
	const TSharedRef<FJsonObject>& Data,
	TArray<FString>& OutWarnings)
{
	UBlueprint* Inner = Before.Blueprint.Get();
	UClass* Class = Inner != nullptr ? Inner->GeneratedClass.Get() : nullptr;
	UObject* Cdo = Class != nullptr ? Class->GetDefaultObject(/*bCreateIfNeeded=*/false) : nullptr;
	if (!Before.bApplicable || Cdo == nullptr)
	{
		return;
	}

	if (bClassCurrent)
	{
		struct FStale
		{
			FInstance Instance;
			TArray<FName> Properties;
			bool bResynced = false;
		};
		TArray<FStale> Stale;
		for (const FInstance& Instance : CollectInstances(Inner, Class))
		{
			const TSet<FName>* Follows = Before.FollowsDefault.Find(Instance.Key());
			if (Follows == nullptr)
			{
				continue;
			}
			FStale Entry;
			Entry.Instance = Instance;
			for (const FName& Name : *Follows)
			{
				const FProperty* Property = Instance.Template->GetClass()->FindPropertyByName(Name);
				const FProperty* CdoProperty = Class->FindPropertyByName(Name);
				if (Property != nullptr && CdoProperty != nullptr && IsTrackedProperty(Property) && CdoProperty->SameType(Property)
					&& !Property->Identical(Property->ContainerPtrToValuePtr<void>(Instance.Template), CdoProperty->ContainerPtrToValuePtr<void>(Cdo)))
				{
					Entry.Properties.Add(Name);
				}
			}
			if (!Entry.Properties.IsEmpty())
			{
				Entry.Properties.Sort(FNameLexicalLess());
				Stale.Add(MoveTemp(Entry));
			}
		}

		TArray<UBlueprint*> ResyncedOuters;
		if (bResync && !Stale.IsEmpty())
		{
			const FScopedTransaction Transaction(FText::FromString(
				FString::Printf(TEXT("Cortex: Resync embedded widget defaults from %s"), *Inner->GetName())));
			for (FStale& Entry : Stale)
			{
				Entry.Instance.Outer->Modify();
				Entry.Instance.Template->Modify();
				for (const FName& Name : Entry.Properties)
				{
					const FProperty* Property = Entry.Instance.Template->GetClass()->FindPropertyByName(Name);
					const FProperty* CdoProperty = Class->FindPropertyByName(Name);
					Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Entry.Instance.Template), CdoProperty->ContainerPtrToValuePtr<void>(Cdo));
				}
				Entry.bResynced = true;
				ResyncedOuters.AddUnique(Entry.Instance.Outer);
			}
			for (UBlueprint* Outer : ResyncedOuters)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Outer);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Instances;
		for (const FStale& Entry : Stale)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), Entry.Instance.Outer->GetPathName());
			Item->SetStringField(TEXT("widget_name"), Entry.Instance.Template->GetName());
			Item->SetArrayField(TEXT("properties"), ToJsonNames(Entry.Properties));
			Item->SetBoolField(TEXT("resynced"), Entry.bResynced);
			Instances.Add(MakeShared<FJsonValueObject>(Item));
		}
		Data->SetArrayField(TEXT("embedded_instances"), Instances);

		if (bResync && bSaveResynced)
		{
			TArray<TSharedPtr<FJsonValue>> Saved;
			for (UBlueprint* Outer : ResyncedOuters)
			{
				TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
				SaveParams->SetStringField(TEXT("asset_path"), Outer->GetPathName());
				const FCortexCommandResult SaveResult = FCortexBPAssetOps::Save(SaveParams);
				if (SaveResult.bSuccess)
				{
					Saved.Add(MakeShared<FJsonValueString>(Outer->GetPathName()));
				}
				else
				{
					OutWarnings.Add(FString::Printf(TEXT("Could not save the resynced Widget Blueprint %s: %s"), *Outer->GetPathName(), *SaveResult.ErrorMessage));
				}
			}
			Data->SetArrayField(TEXT("embedded_saved_assets"), Saved);
		}
	}
	else
	{
		// The class is not recompiled yet: the class defaults the templates would be compared with do not exist.
		Data->SetBoolField(TEXT("embedded_report_pending_compile"), true);
	}

	if (bReportDropped)
	{
		// A recompiled class says which properties exist. Before the recompile, a property survives when the Blueprint declares it itself
		// or the new parent does.
		const auto PropertyExists = [&](const FName& Name)
		{
			const FProperty* Found = Class->FindPropertyByName(Name);
			if (bClassCurrent)
			{
				return Found != nullptr;
			}
			return (Found != nullptr && Found->GetOwnerClass() == Class) || (NewParent != nullptr && NewParent->FindPropertyByName(Name) != nullptr);
		};

		TArray<TSharedPtr<FJsonValue>> Dropped;
		for (const FPreState::FOverride& Override : Before.Overrides)
		{
			if (PropertyExists(Override.Property))
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_path"), Override.OuterPath);
			Item->SetStringField(TEXT("widget_name"), Override.WidgetName);
			Item->SetStringField(TEXT("property"), Override.Property.ToString());
			Item->SetStringField(TEXT("instance_value"), Override.Value);
			Dropped.Add(MakeShared<FJsonValueObject>(Item));
		}
		Data->SetArrayField(TEXT("dropped_overrides"), Dropped);
	}
}
