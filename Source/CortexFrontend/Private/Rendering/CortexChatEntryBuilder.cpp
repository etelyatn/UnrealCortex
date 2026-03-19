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

    // Category matching with fuzzy fallback for LLM hallucinations
    const FString& CatStr = Parts[1];
    if (CatStr == TEXT("bug"))                       OutCategory = ECortexFindingCategory::Bug;
    else if (CatStr == TEXT("performance"))          OutCategory = ECortexFindingCategory::Performance;
    else if (CatStr == TEXT("quality"))              OutCategory = ECortexFindingCategory::Quality;
    else if (CatStr == TEXT("cpp_candidate"))        OutCategory = ECortexFindingCategory::CppCandidate;
    else if (CatStr == TEXT("engine_fix_guidance"))  OutCategory = ECortexFindingCategory::EngineFixGuidance;
    // Fuzzy fallback: map common LLM hallucinations to closest category
    else if (CatStr == TEXT("security") || CatStr == TEXT("error") || CatStr == TEXT("logic"))
        OutCategory = ECortexFindingCategory::Bug;
    else if (CatStr == TEXT("optimization") || CatStr == TEXT("perf"))
        OutCategory = ECortexFindingCategory::Performance;
    else if (CatStr == TEXT("style") || CatStr == TEXT("naming") || CatStr == TEXT("readability"))
        OutCategory = ECortexFindingCategory::Quality;
    else if (CatStr == TEXT("engine") || CatStr == TEXT("fix") || CatStr == TEXT("diagnostic"))
        OutCategory = ECortexFindingCategory::EngineFixGuidance;
    else
        OutCategory = ECortexFindingCategory::Bug;  // Default to bug rather than dropping

    // Severity matching with fuzzy fallback
    const FString& SevStr = Parts[2];
    if (SevStr == TEXT("critical"))          OutSeverity = ECortexFindingSeverity::Critical;
    else if (SevStr == TEXT("warning"))      OutSeverity = ECortexFindingSeverity::Warning;
    else if (SevStr == TEXT("info"))         OutSeverity = ECortexFindingSeverity::Info;
    else if (SevStr == TEXT("suggestion"))   OutSeverity = ECortexFindingSeverity::Suggestion;
    // Fuzzy fallback
    else if (SevStr == TEXT("high") || SevStr == TEXT("error") || SevStr == TEXT("blocker"))
        OutSeverity = ECortexFindingSeverity::Critical;
    else if (SevStr == TEXT("medium") || SevStr == TEXT("warn"))
        OutSeverity = ECortexFindingSeverity::Warning;
    else if (SevStr == TEXT("low") || SevStr == TEXT("minor") || SevStr == TEXT("note"))
        OutSeverity = ECortexFindingSeverity::Info;
    else
        OutSeverity = ECortexFindingSeverity::Warning;  // Default to warning

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
        // Fallback: nested backticks may have corrupted the block. Try extracting
        // JSON by finding the first { and last } in the raw text.
        const int32 FirstBrace = CleanJson.Find(TEXT("{"));
        const int32 LastBrace = CleanJson.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (FirstBrace != INDEX_NONE && LastBrace != INDEX_NONE && LastBrace > FirstBrace)
        {
            FString Extracted = CleanJson.Mid(FirstBrace, LastBrace - FirstBrace + 1);
            TSharedRef<TJsonReader<>> FallbackReader = TJsonReaderFactory<>::Create(Extracted);
            if (!FJsonSerializer::Deserialize(FallbackReader, JsonObj) || !JsonObj.IsValid())
            {
                return false;
            }
        }
        else
        {
            return false;
        }
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

FString FCortexChatEntryBuilder::StripFindingBlocks(const FString& FullText)
{
    TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(FullText);
    FString Result;

    for (const FCortexMarkdownBlock& Block : Blocks)
    {
        if (Block.Type == ECortexMarkdownBlockType::CodeBlock)
        {
            // Skip finding:* and analysis:summary blocks
            if (Block.Language.StartsWith(TEXT("finding:"), ESearchCase::IgnoreCase)
                || Block.Language.Equals(TEXT("analysis:summary"), ESearchCase::IgnoreCase))
            {
                continue;
            }
        }

        if (!Result.IsEmpty())
        {
            Result += TEXT("\n\n");
        }
        Result += Block.RawText;
    }

    return Result;
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
