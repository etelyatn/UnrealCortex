#include "Operations/CortexBPSCSDiagnostics.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	void AddUniqueKey(TArray<FString>& Keys, const FString& Key)
	{
		if (!Keys.Contains(Key))
		{
			Keys.Add(Key);
		}
	}

	FCortexBPSCSDiagnostics::ECollisionSeverity ResolvePropertyCollisionSeverity(
		UClass* SCSComponentClass,
		FProperty* Property)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (!ObjectProperty || !SCSComponentClass)
		{
			return FCortexBPSCSDiagnostics::ECollisionSeverity::Blocking;
		}

		UClass* PropertyClass = ObjectProperty->PropertyClass;
		if (!PropertyClass || !PropertyClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return FCortexBPSCSDiagnostics::ECollisionSeverity::Blocking;
		}

		return SCSComponentClass->IsChildOf(PropertyClass)
			? FCortexBPSCSDiagnostics::ECollisionSeverity::Adoptable
			: FCortexBPSCSDiagnostics::ECollisionSeverity::Blocking;
	}

	void AddCollisionIfMissing(
		TArray<FCortexBPSCSDiagnostics::FCollision>& Collisions,
		const FCortexBPSCSDiagnostics::FCollision& Candidate)
	{
		const bool bAlreadyExists = Collisions.ContainsByPredicate(
			[&Candidate](const FCortexBPSCSDiagnostics::FCollision& Existing)
			{
				return Existing.SCSNodeName == Candidate.SCSNodeName
					&& Existing.Kind == Candidate.Kind
					&& Existing.InheritedFromClass == Candidate.InheritedFromClass;
			});

		if (!bAlreadyExists)
		{
			Collisions.Add(Candidate);
		}
	}
}

TSharedPtr<FJsonObject> FCortexBPSCSDiagnostics::FDirtyReport::ToRefusalJson() const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FString& Key : AcknowledgmentKeys)
	{
		Keys.Add(MakeShared<FJsonValueString>(Key));
	}
	Data->SetArrayField(TEXT("required_acknowledgment"), Keys);
	Data->SetStringField(TEXT("explanation"), Explanation);

	TArray<TSharedPtr<FJsonValue>> DetailsArray;
	for (const FDirtyDetail& Detail : DirtyDetails)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Detail.Key);
		Entry->SetStringField(TEXT("kind"), Detail.Kind);
		Entry->SetStringField(TEXT("component_class"), Detail.ComponentClass);
		DetailsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("dirty_details"), DetailsArray);

	return Data;
}

TSharedPtr<FJsonObject> FCortexBPSCSDiagnostics::FDirtyReport::ToDiffJson() const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FString& Key : TopLevelKeys)
	{
		Keys.Add(MakeShared<FJsonValueString>(Key));
	}
	Data->SetArrayField(TEXT("dirty_keys"), Keys);

	const FString Summary = TopLevelKeys.IsEmpty()
		? FString(TEXT("no top-level property differences from archetype"))
		: FString::Printf(
			TEXT("%d top-level property differed from archetype: %s"),
			TopLevelKeys.Num(),
			*FString::Join(TopLevelKeys, TEXT(", ")));
	Data->SetStringField(TEXT("diff_summary"), Summary);

	return Data;
}

FCortexBPSCSDiagnostics::FDirtyReport FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(
	USCS_Node* Node,
	UBlueprint* BP)
{
	FDirtyReport Report;

	if (!Node || !BP)
	{
		return Report;
	}

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
	if (!BPGC)
	{
		return Report;
	}

	UActorComponent* Template = Node->GetActualComponentTemplate(BPGC);
	if (!Template)
	{
		return Report;
	}

	UObject* Archetype = Template->GetArchetype();
	if (!Archetype)
	{
		return Report;
	}

	for (TFieldIterator<FProperty> It(Template->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property)
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
		{
			continue;
		}

		if (Property->Identical_InContainer(Template, Archetype, 0, PPF_DeepComparison))
		{
			continue;
		}

		const FString Key = Property->GetName();
		const bool bIsSubObjectLoss =
			Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference);

		if (bIsSubObjectLoss)
		{
			AddUniqueKey(Report.SubObjectKeys, Key);
			AddUniqueKey(Report.AcknowledgmentKeys, Key);
			Report.DirtyDetails.Add(FCortexBPSCSDiagnostics::FDirtyReport::FDirtyDetail{
				Key,
				TEXT("sub_object"),
				Template->GetClass()->GetName()});
		}
		else
		{
			AddUniqueKey(Report.TopLevelKeys, Key);
			Report.DirtyDetails.Add(FCortexBPSCSDiagnostics::FDirtyReport::FDirtyDetail{
				Key,
				TEXT("top_level"),
				Template->GetClass()->GetName()});
		}
	}

	Report.AcknowledgmentKeys.Sort();
	Report.SubObjectKeys.Sort();
	Report.TopLevelKeys.Sort();
	Report.DirtyDetails.Sort([](
		const FCortexBPSCSDiagnostics::FDirtyReport::FDirtyDetail& A,
		const FCortexBPSCSDiagnostics::FDirtyReport::FDirtyDetail& B)
	{
		return A.Key < B.Key;
	});

	if (Report.SubObjectKeys.IsEmpty())
	{
		Report.Explanation = TEXT("No sub-object data loss detected.");
	}
	else
	{
		Report.Explanation = FString::Printf(
			TEXT("Instanced sub-object state differs from archetype for: %s"),
			*FString::Join(Report.SubObjectKeys, TEXT(", ")));
	}

	return Report;
}

TArray<FCortexBPSCSDiagnostics::FCollision> FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(
	UBlueprint* BP)
{
	TArray<FCollision> Collisions;

	if (!BP || !BP->ParentClass || !BP->SimpleConstructionScript)
	{
		return Collisions;
	}

	const TArray<USCS_Node*>& OwnNodes = BP->SimpleConstructionScript->GetAllNodes();
	for (USCS_Node* SCSNode : OwnNodes)
	{
		if (!SCSNode)
		{
			continue;
		}

		const FName NodeName = SCSNode->GetVariableName();
		UClass* SCSComponentClass = SCSNode->ComponentClass;
		if (!SCSComponentClass && SCSNode->ComponentTemplate)
		{
			SCSComponentClass = SCSNode->ComponentTemplate->GetClass();
		}

		// Pass A: inherited class properties/delegates.
		for (TFieldIterator<FProperty> PropIt(BP->ParentClass, EFieldIterationFlags::IncludeSuper); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (!Property || Property->GetFName() != NodeName)
			{
				continue;
			}

			FCollision Collision;
			Collision.SCSNodeName = NodeName;
			Collision.SCSComponentClass = SCSComponentClass;
			Collision.InheritedProperty = Property;
			Collision.InheritedFromClass = Cast<UClass>(Property->GetOwnerStruct());

			if (CastField<FMulticastDelegateProperty>(Property) || CastField<FDelegateProperty>(Property))
			{
				Collision.Kind = ECollisionInheritedKind::Delegate;
				Collision.Severity = ECollisionSeverity::Blocking;
			}
			else
			{
				Collision.Kind = ECollisionInheritedKind::UProperty;
				Collision.Severity = ResolvePropertyCollisionSeverity(SCSComponentClass, Property);
			}

			AddCollisionIfMissing(Collisions, Collision);
		}

		// Pass B: properties on implemented interfaces.
		for (const FImplementedInterface& InterfaceDesc : BP->ParentClass->Interfaces)
		{
			UClass* InterfaceClass = InterfaceDesc.Class;
			if (!InterfaceClass)
			{
				continue;
			}

			for (TFieldIterator<FProperty> PropIt(InterfaceClass); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property || Property->GetFName() != NodeName)
				{
					continue;
				}

				FCollision Collision;
				Collision.SCSNodeName = NodeName;
				Collision.SCSComponentClass = SCSComponentClass;
				Collision.InheritedProperty = Property;
				Collision.InheritedFromClass = InterfaceClass;
				Collision.Kind = ECollisionInheritedKind::Interface;
				Collision.Severity = ECollisionSeverity::Blocking;
				AddCollisionIfMissing(Collisions, Collision);
			}
		}

		// Pass C: sparse class data fields.
		if (UScriptStruct* SparseStruct = BP->ParentClass->GetSparseClassDataStruct())
		{
			for (TFieldIterator<FProperty> PropIt(SparseStruct); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property || Property->GetFName() != NodeName)
				{
					continue;
				}

				FCollision Collision;
				Collision.SCSNodeName = NodeName;
				Collision.SCSComponentClass = SCSComponentClass;
				Collision.InheritedProperty = Property;
				Collision.InheritedFromClass = BP->ParentClass;
				Collision.Kind = ECollisionInheritedKind::SparseClassData;
				Collision.Severity = ECollisionSeverity::Blocking;
				AddCollisionIfMissing(Collisions, Collision);
			}
		}

		// Pass D: inherited SCS nodes from parent Blueprint classes.
		for (UClass* ClassCursor = BP->ParentClass; ClassCursor; ClassCursor = ClassCursor->GetSuperClass())
		{
			UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(ClassCursor);
			if (!ParentBPGC || !ParentBPGC->SimpleConstructionScript)
			{
				continue;
			}

			USCS_Node* InheritedNode = ParentBPGC->SimpleConstructionScript->FindSCSNode(NodeName);
			if (!InheritedNode)
			{
				continue;
			}

			UClass* InheritedSCSClass = InheritedNode->ComponentClass;
			if (!InheritedSCSClass && InheritedNode->ComponentTemplate)
			{
				InheritedSCSClass = InheritedNode->ComponentTemplate->GetClass();
			}

			FCollision Collision;
			Collision.SCSNodeName = NodeName;
			Collision.SCSComponentClass = SCSComponentClass;
			Collision.InheritedSCSClass = InheritedSCSClass;
			Collision.InheritedFromClass = ClassCursor;
			Collision.Kind = ECollisionInheritedKind::SCS;
			Collision.Severity = (SCSComponentClass && InheritedSCSClass && SCSComponentClass == InheritedSCSClass)
				? ECollisionSeverity::Adoptable
				: ECollisionSeverity::Blocking;
			AddCollisionIfMissing(Collisions, Collision);
			break;
		}
	}

	return Collisions;
}

FCortexBPSCSDiagnostics::FResolveResult FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(
	UBlueprint* /*BP*/,
	const FString& /*Name*/)
{
	return FResolveResult{};
}
