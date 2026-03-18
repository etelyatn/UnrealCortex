// CortexChatEntryBuilder.cpp
#include "Rendering/CortexChatEntryBuilder.h"

#include "Analysis/CortexFindingTypes.h"
#include "Rendering/CortexMarkdownParser.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

TArray<TSharedPtr<FCortexChatEntry>> FCortexChatEntryBuilder::BuildEntries(
    const FString& FullText,
    TArray<FCortexAnalysisFinding>* OutFindings,
    FCortexAnalysisSummary* OutSummary)
{
    TArray<TSharedPtr<FCortexChatEntry>> Entries;
    TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(FullText);

    for (const FCortexMarkdownBlock& Block : Blocks)
    {
        TSharedPtr<FCortexChatEntry> Entry = MakeShared<FCortexChatEntry>();

        if (Block.Type == ECortexMarkdownBlockType::CodeBlock)
        {
            // Check for analysis:summary tag
            if (Block.Language.Equals(TEXT("analysis:summary"), ESearchCase::IgnoreCase) && OutSummary)
            {
                ParseAnalysisSummary(Block.RawText, *OutSummary);
                // Don't render summary blocks in chat — they go to findings panel header
                continue;
            }

            ECortexFindingCategory Category;
            ECortexFindingSeverity Severity;

            if (OutFindings && ParseFindingTag(Block.Language, Category, Severity))
            {
                FCortexAnalysisFinding Finding;
                if (ParseFindingJson(Block.RawText, Category, Severity, Finding))
                {
                    OutFindings->Add(MoveTemp(Finding));

                    // Still create a chat entry for inline display
                    Entry->Type = ECortexChatEntryType::CodeBlock;
                    Entry->Language = Block.Language;
                    Entry->Text = Block.RawText;
                }
                else
                {
                    // Malformed finding — render as regular code block
                    Entry->Type = ECortexChatEntryType::CodeBlock;
                    Entry->Language = TEXT("json");
                    Entry->Text = Block.RawText;
                }
            }
            else
            {
                Entry->Type = ECortexChatEntryType::CodeBlock;
                Entry->Language = Block.Language;
                Entry->CodeBlockTarget = Block.CodeBlockTarget;
                Entry->Text = Block.RawText;
            }
        }
        else
        {
            Entry->Type = ECortexChatEntryType::AssistantMessage;
            Entry->Text = Block.RawText;
        }

        Entries.Add(Entry);
    }

    return Entries;
}

bool FCortexChatEntryBuilder::ParseFindingTag(
    const FString& Tag,
    ECortexFindingCategory& OutCategory,
    ECortexFindingSeverity& OutSeverity)
{
    if (!Tag.StartsWith(TEXT("finding:")))
    {
        return false;
    }

    TArray<FString> Parts;
    Tag.ParseIntoArray(Parts, TEXT(":"));
    if (Parts.Num() != 3)
    {
        return false;
    }

    const FString& CatStr = Parts[1];
    if (CatStr == TEXT("bug"))                       OutCategory = ECortexFindingCategory::Bug;
    else if (CatStr == TEXT("performance"))          OutCategory = ECortexFindingCategory::Performance;
    else if (CatStr == TEXT("quality"))              OutCategory = ECortexFindingCategory::Quality;
    else if (CatStr == TEXT("cpp_candidate"))        OutCategory = ECortexFindingCategory::CppCandidate;
    else if (CatStr == TEXT("engine_fix_guidance"))  OutCategory = ECortexFindingCategory::EngineFixGuidance;
    else return false;

    const FString& SevStr = Parts[2];
    if (SevStr == TEXT("critical"))          OutSeverity = ECortexFindingSeverity::Critical;
    else if (SevStr == TEXT("warning"))      OutSeverity = ECortexFindingSeverity::Warning;
    else if (SevStr == TEXT("info"))         OutSeverity = ECortexFindingSeverity::Info;
    else if (SevStr == TEXT("suggestion"))   OutSeverity = ECortexFindingSeverity::Suggestion;
    else return false;

    return true;
}

bool FCortexChatEntryBuilder::ParseFindingJson(
    const FString& JsonBody,
    ECortexFindingCategory Category,
    ECortexFindingSeverity Severity,
    FCortexAnalysisFinding& OutFinding)
{
    // Strip trailing commas for lenient parsing
    FString CleanJson = JsonBody;
    CleanJson.ReplaceInline(TEXT(",\n}"), TEXT("\n}"));
    CleanJson.ReplaceInline(TEXT(",}"), TEXT("}"));

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanJson);
    TSharedPtr<FJsonObject> JsonObj;
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
    {
        return false;
    }

    if (!JsonObj->HasField(TEXT("title")) || !JsonObj->HasField(TEXT("node")))
    {
        return false;
    }

    OutFinding.Category = Category;
    OutFinding.Severity = Severity;
    OutFinding.Title = JsonObj->GetStringField(TEXT("title"));
    OutFinding.Description = JsonObj->HasField(TEXT("description"))
        ? JsonObj->GetStringField(TEXT("description")) : TEXT("");
    OutFinding.SuggestedFix = JsonObj->HasField(TEXT("suggestedFix"))
        ? JsonObj->GetStringField(TEXT("suggestedFix")) : TEXT("");

    // Store node reference as NodeDisplayName — caller resolves node_N -> FGuid via context
    OutFinding.NodeDisplayName = JsonObj->GetStringField(TEXT("node"));

    // Parse optional confidence field (default 1.0 if missing)
    if (JsonObj->HasField(TEXT("confidence")))
    {
        OutFinding.Confidence = static_cast<float>(JsonObj->GetNumberField(TEXT("confidence")));
    }
    else
    {
        OutFinding.Confidence = 1.0f;
    }

    return true;
}

bool FCortexChatEntryBuilder::ParseAnalysisSummary(const FString& JsonBody, FCortexAnalysisSummary& OutSummary)
{
    // Strip trailing commas for lenient parsing
    FString CleanJson = JsonBody;
    CleanJson.ReplaceInline(TEXT(",\n}"), TEXT("\n}"));
    CleanJson.ReplaceInline(TEXT(",}"), TEXT("}"));

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    OutSummary.Reported = JsonObject->HasField(TEXT("reported"))
        ? static_cast<int32>(JsonObject->GetNumberField(TEXT("reported"))) : 0;
    OutSummary.EstimatedSuppressed = JsonObject->HasField(TEXT("estimated_suppressed"))
        ? static_cast<int32>(JsonObject->GetNumberField(TEXT("estimated_suppressed"))) : 0;
    OutSummary.SuppressionNotes = JsonObject->HasField(TEXT("suppression_notes"))
        ? JsonObject->GetStringField(TEXT("suppression_notes")) : TEXT("");

    return true;
}
