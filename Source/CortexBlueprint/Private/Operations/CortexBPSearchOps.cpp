#include "Operations/CortexBPSearchOps.h"

#include "CortexBlueprintModule.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Internationalization/Text.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace
{
	const TSet<FString> ValidSearchAreas = {TEXT("pins"), TEXT("cdo"), TEXT("widgets")};
}

bool FCortexBPSearchOps::MatchesQuery(const FString& Value, const FString& Query, bool bCaseSensitive)
{
	return Value.Contains(Query, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
}

FCortexCommandResult FCortexBPSearchOps::Search(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Query;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("query"), Query))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and query"));
	}

	if (Query.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("query must be non-empty"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	bool bCaseSensitive = false;
	Params->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);

	int32 MaxResults = 100;
	Params->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Max(1, MaxResults);

	TSet<FString> SearchIn;
	const TArray<TSharedPtr<FJsonValue>>* SearchInArray = nullptr;
	if (Params->TryGetArrayField(TEXT("search_in"), SearchInArray) && SearchInArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *SearchInArray)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("search_in entries must be strings: pins, cdo, widgets"));
			}

			const FString Area = Value->AsString();
			if (!ValidSearchAreas.Contains(Area))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Invalid search_in value: '%s'. Valid: pins, cdo, widgets"), *Area));
			}

			SearchIn.Add(Area);
		}
	}

	const bool bSearchPins = SearchIn.Num() == 0 || SearchIn.Contains(TEXT("pins"));
	const bool bSearchCDO = SearchIn.Num() == 0 || SearchIn.Contains(TEXT("cdo"));
	const bool bSearchWidgets = SearchIn.Num() == 0 || SearchIn.Contains(TEXT("widgets"));

	TArray<FSearchMatch> Matches;
	if (bSearchPins)
	{
		SearchGraphPins(Blueprint, Query, bCaseSensitive, MaxResults, Matches);
	}
	if (bSearchCDO && Matches.Num() < MaxResults)
	{
		SearchClassDefaults(Blueprint, Query, bCaseSensitive, MaxResults, Matches);
	}
	if (bSearchWidgets && Matches.Num() < MaxResults)
	{
		SearchWidgetTree(Blueprint, Query, bCaseSensitive, MaxResults, Matches);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("query"), Query);
	Data->SetNumberField(TEXT("match_count"), Matches.Num());
	Data->SetBoolField(TEXT("truncated"), Matches.Num() >= MaxResults);

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	MatchesArray.Reserve(Matches.Num());
	for (const FSearchMatch& Match : Matches)
	{
		TSharedPtr<FJsonObject> MatchObject = MakeShared<FJsonObject>();
		MatchObject->SetStringField(TEXT("location"), Match.Location);
		MatchObject->SetStringField(TEXT("property"), Match.Property);
		MatchObject->SetStringField(TEXT("type"), Match.Type);
		MatchObject->SetStringField(TEXT("value"), Match.Value);

		if (!Match.NodeId.IsEmpty())
		{
			MatchObject->SetStringField(TEXT("node_id"), Match.NodeId);
		}
		if (!Match.GraphName.IsEmpty())
		{
			MatchObject->SetStringField(TEXT("graph_name"), Match.GraphName);
		}
		if (!Match.TableId.IsEmpty())
		{
			TSharedPtr<FJsonObject> StringTable = MakeShared<FJsonObject>();
			StringTable->SetStringField(TEXT("table_id"), Match.TableId);
			StringTable->SetStringField(TEXT("key"), Match.Key);
			MatchObject->SetObjectField(TEXT("string_table"), StringTable);
		}

		MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObject));
	}
	Data->SetArrayField(TEXT("matches"), MatchesArray);

	return FCortexCommandRouter::Success(Data);
}

void FCortexBPSearchOps::SearchGraphPins(
	UBlueprint* Blueprint,
	const FString& Query,
	bool bCaseSensitive,
	int32 MaxResults,
	TArray<FSearchMatch>& OutMatches)
{
	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);
	Graphs.Append(Blueprint->MacroGraphs);

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph || OutMatches.Num() >= MaxResults)
		{
			break;
		}

		const FString GraphName = Graph->GetName();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || OutMatches.Num() >= MaxResults)
			{
				break;
			}

			const FString NodeId = Node->GetName();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || OutMatches.Num() >= MaxResults)
				{
					break;
				}

				const bool bIsTextPin = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text;
				if (bIsTextPin && !Pin->DefaultTextValue.IsEmpty())
				{
					const FString ResolvedText = Pin->DefaultTextValue.ToString();
					FName TableId;
					FString Key;
					const bool bTableBacked = FTextInspector::GetTableIdAndKey(Pin->DefaultTextValue, TableId, Key);

					const bool bMatches = MatchesQuery(ResolvedText, Query, bCaseSensitive)
						|| (bTableBacked && MatchesQuery(TableId.ToString(), Query, bCaseSensitive))
						|| (bTableBacked && MatchesQuery(Key, Query, bCaseSensitive));
					if (bMatches)
					{
						FSearchMatch Match;
						Match.Location = FString::Printf(TEXT("%s/%s/%s"),
							*GraphName, *NodeId, *Pin->PinName.ToString());
						Match.NodeId = NodeId;
						Match.GraphName = GraphName;
						Match.Property = Pin->PinName.ToString();
						Match.Type = TEXT("pin");
						Match.Value = ResolvedText;
						if (bTableBacked)
						{
							Match.TableId = TableId.ToString();
							Match.Key = Key;
						}
						OutMatches.Add(Match);
					}
				}
				else if (!bIsTextPin
					&& !Pin->DefaultValue.IsEmpty()
					&& MatchesQuery(Pin->DefaultValue, Query, bCaseSensitive))
				{
					FSearchMatch Match;
					Match.Location = FString::Printf(TEXT("%s/%s/%s"),
						*GraphName, *NodeId, *Pin->PinName.ToString());
					Match.NodeId = NodeId;
					Match.GraphName = GraphName;
					Match.Property = Pin->PinName.ToString();
					Match.Type = TEXT("pin");
					Match.Value = Pin->DefaultValue;
					OutMatches.Add(Match);
				}

				if (Pin->DefaultObject
					&& OutMatches.Num() < MaxResults
					&& MatchesQuery(Pin->DefaultObject->GetPathName(), Query, bCaseSensitive))
				{
					FSearchMatch Match;
					Match.Location = FString::Printf(TEXT("%s/%s/%s"),
						*GraphName, *NodeId, *Pin->PinName.ToString());
					Match.NodeId = NodeId;
					Match.GraphName = GraphName;
					Match.Property = Pin->PinName.ToString();
					Match.Type = TEXT("pin");
					Match.Value = Pin->DefaultObject->GetPathName();
					OutMatches.Add(Match);
				}
			}
		}
	}
}

void FCortexBPSearchOps::SearchClassDefaults(
	UBlueprint* Blueprint,
	const FString& Query,
	bool bCaseSensitive,
	int32 MaxResults,
	TArray<FSearchMatch>& OutMatches)
{
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!GeneratedClass)
	{
		return;
	}

	UObject* CDO = GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(GeneratedClass); It; ++It)
	{
		if (OutMatches.Num() >= MaxResults)
		{
			break;
		}

		FProperty* Property = *It;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);

		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			const FText& TextValue = TextProperty->GetPropertyValue(ValuePtr);
			if (TextValue.IsEmpty())
			{
				continue;
			}

			const FString ResolvedText = TextValue.ToString();
			FName TableId;
			FString Key;
			const bool bTableBacked = FTextInspector::GetTableIdAndKey(TextValue, TableId, Key);

			const bool bMatches = MatchesQuery(ResolvedText, Query, bCaseSensitive)
				|| (bTableBacked && MatchesQuery(TableId.ToString(), Query, bCaseSensitive))
				|| (bTableBacked && MatchesQuery(Key, Query, bCaseSensitive));
			if (bMatches)
			{
				FSearchMatch Match;
				Match.Location = TEXT("ClassDefaults");
				Match.Property = Property->GetName();
				Match.Type = TEXT("cdo");
				Match.Value = ResolvedText;
				if (bTableBacked)
				{
					Match.TableId = TableId.ToString();
					Match.Key = Key;
				}
				OutMatches.Add(Match);
			}
			continue;
		}

		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			const FString& StringValue = StringProperty->GetPropertyValue(ValuePtr);
			if (!StringValue.IsEmpty() && MatchesQuery(StringValue, Query, bCaseSensitive))
			{
				FSearchMatch Match;
				Match.Location = TEXT("ClassDefaults");
				Match.Property = Property->GetName();
				Match.Type = TEXT("cdo");
				Match.Value = StringValue;
				OutMatches.Add(Match);
			}
			continue;
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			const FString NameValue = NameProperty->GetPropertyValue(ValuePtr).ToString();
			if (!NameValue.IsEmpty()
				&& NameValue != TEXT("None")
				&& MatchesQuery(NameValue, Query, bCaseSensitive))
			{
				FSearchMatch Match;
				Match.Location = TEXT("ClassDefaults");
				Match.Property = Property->GetName();
				Match.Type = TEXT("cdo");
				Match.Value = NameValue;
				OutMatches.Add(Match);
			}
		}
	}
}

void FCortexBPSearchOps::SearchWidgetTree(
	UBlueprint* Blueprint,
	const FString& Query,
	bool bCaseSensitive,
	int32 MaxResults,
	TArray<FSearchMatch>& OutMatches)
{
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (!WidgetBlueprintClass || !Blueprint->IsA(WidgetBlueprintClass))
	{
		return;
	}

	const FObjectProperty* WidgetTreeProperty = CastField<FObjectProperty>(
		Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	UObject* WidgetTreeObject = WidgetTreeProperty
		? WidgetTreeProperty->GetObjectPropertyValue_InContainer(Blueprint)
		: nullptr;
	if (!WidgetTreeObject)
	{
		return;
	}

	ForEachObjectWithOuter(WidgetTreeObject, [&](UObject* Widget)
	{
		if (!Widget || OutMatches.Num() >= MaxResults)
		{
			return;
		}

		const FString WidgetName = Widget->GetName();

		for (TFieldIterator<FTextProperty> TextIt(Widget->GetClass()); TextIt; ++TextIt)
		{
			if (OutMatches.Num() >= MaxResults)
			{
				break;
			}

			const FTextProperty* TextProperty = *TextIt;
			const FText& TextValue = TextProperty->GetPropertyValue(
				TextProperty->ContainerPtrToValuePtr<void>(Widget));
			if (TextValue.IsEmpty())
			{
				continue;
			}

			const FString ResolvedText = TextValue.ToString();
			FName TableId;
			FString Key;
			const bool bTableBacked = FTextInspector::GetTableIdAndKey(TextValue, TableId, Key);

			const bool bMatches = MatchesQuery(ResolvedText, Query, bCaseSensitive)
				|| (bTableBacked && MatchesQuery(TableId.ToString(), Query, bCaseSensitive))
				|| (bTableBacked && MatchesQuery(Key, Query, bCaseSensitive));
			if (bMatches)
			{
				FSearchMatch Match;
				Match.Location = FString::Printf(TEXT("WidgetTree/%s"), *WidgetName);
				Match.Property = TextProperty->GetName();
				Match.Type = TEXT("widget");
				Match.Value = ResolvedText;
				if (bTableBacked)
				{
					Match.TableId = TableId.ToString();
					Match.Key = Key;
				}
				OutMatches.Add(Match);
			}
		}

		for (TFieldIterator<FStrProperty> StringIt(Widget->GetClass()); StringIt; ++StringIt)
		{
			if (OutMatches.Num() >= MaxResults)
			{
				break;
			}

			const FStrProperty* StringProperty = *StringIt;
			const FString& StringValue = StringProperty->GetPropertyValue(
				StringProperty->ContainerPtrToValuePtr<void>(Widget));
			if (!StringValue.IsEmpty() && MatchesQuery(StringValue, Query, bCaseSensitive))
			{
				FSearchMatch Match;
				Match.Location = FString::Printf(TEXT("WidgetTree/%s"), *WidgetName);
				Match.Property = StringProperty->GetName();
				Match.Type = TEXT("widget");
				Match.Value = StringValue;
				OutMatches.Add(Match);
			}
		}
	});
}
