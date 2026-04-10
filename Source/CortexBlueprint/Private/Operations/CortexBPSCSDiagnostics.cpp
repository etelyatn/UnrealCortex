#include "Operations/CortexBPSCSDiagnostics.h"

#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"

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
	USCS_Node* /*Node*/,
	UBlueprint* /*BP*/)
{
	return FDirtyReport{};
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
