#include "Operations/CortexBPSCSDiagnostics.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
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
	UBlueprint* /*BP*/)
{
	return {};
}

FCortexBPSCSDiagnostics::FResolveResult FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(
	UBlueprint* /*BP*/,
	const FString& /*Name*/)
{
	return FResolveResult{};
}
