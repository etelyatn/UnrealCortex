#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexDiffParser.h"
#include "CortexConversionTypes.h"

class FCortexCliSession;

// ── Code tab enum ──
enum class ECortexCodeTab : uint8 { Header, Implementation, Snippet };

// ── Code document — shared data object observed by canvas and chat ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCodeDocumentChanged, ECortexCodeTab /*ChangedTab*/);

struct FCortexCodeSnapshot
{
    FString HeaderCode;
    FString ImplementationCode;
    FString SnippetCode;
};

struct FCortexCodeDocument
{
    FString HeaderCode;
    FString ImplementationCode;
    FString SnippetCode;
    FString ClassName;
    FString TargetPath;
    bool bIsSnippetMode = false;

    // Multi-level snapshot stack for undo
    TArray<FCortexCodeSnapshot> SnapshotStack;

    FOnCodeDocumentChanged OnDocumentChanged;

    void UpdateHeader(const FString& NewCode)
    {
        HeaderCode = CortexDiffParser::NormalizeForDiff(NewCode);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Header);
    }

    void UpdateImplementation(const FString& NewCode)
    {
        ImplementationCode = CortexDiffParser::NormalizeForDiff(NewCode);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Implementation);
    }

    void UpdateSnippet(const FString& NewCode)
    {
        SnippetCode = CortexDiffParser::NormalizeForDiff(NewCode);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Snippet);
    }

    void PushSnapshot()
    {
        FCortexCodeSnapshot Snapshot;
        Snapshot.HeaderCode = HeaderCode;
        Snapshot.ImplementationCode = ImplementationCode;
        Snapshot.SnippetCode = SnippetCode;
        SnapshotStack.Add(MoveTemp(Snapshot));
    }

    void PopSnapshot()
    {
        if (SnapshotStack.IsEmpty())
        {
            return;
        }
        FCortexCodeSnapshot Snapshot = SnapshotStack.Pop();
        HeaderCode = MoveTemp(Snapshot.HeaderCode);
        ImplementationCode = MoveTemp(Snapshot.ImplementationCode);
        SnippetCode = MoveTemp(Snapshot.SnippetCode);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Header);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Implementation);
        OnDocumentChanged.Broadcast(ECortexCodeTab::Snippet);
    }

    void ClearSnapshots()
    {
        SnapshotStack.Empty();
    }
};

// ── Per-tab conversion context ──
struct FCortexConversionContext
{
    explicit FCortexConversionContext(const FCortexConversionPayload& InPayload)
        : Payload(InPayload)
    {
        TabGuid = FGuid::NewGuid();
        TabId = *FString::Printf(TEXT("CortexConversion_%s"), *TabGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Document = MakeShared<FCortexCodeDocument>();

        // Derive class name from Blueprint name (strip UE asset prefixes, add A/U type prefix)
        FString DerivedName = InPayload.BlueprintName;
        if (DerivedName.StartsWith(TEXT("WBP_")))
        {
            DerivedName = DerivedName.Mid(4);
        }
        else if (DerivedName.StartsWith(TEXT("ABP_")))
        {
            DerivedName = DerivedName.Mid(4);
        }
        else if (DerivedName.StartsWith(TEXT("BP_")))
        {
            DerivedName = DerivedName.Mid(3);
        }
        else if (DerivedName.StartsWith(TEXT("B_")))
        {
            DerivedName = DerivedName.Mid(2);
        }
        if (!DerivedName.IsEmpty() && DerivedName[0] != TEXT('A') && DerivedName[0] != TEXT('U'))
        {
            if (InPayload.bIsWidgetBlueprint)
            {
                DerivedName = TEXT("U") + DerivedName;
            }
            else if (InPayload.ParentClassName.Contains(TEXT("Actor"))
                || InPayload.ParentClassName.Contains(TEXT("Pawn"))
                || InPayload.ParentClassName.Contains(TEXT("Character")))
            {
                DerivedName = TEXT("A") + DerivedName;
            }
            else
            {
                DerivedName = TEXT("U") + DerivedName;
            }
        }
        else if (InPayload.bIsWidgetBlueprint && !DerivedName.IsEmpty()
            && DerivedName[0] == TEXT('A'))
        {
            // Widget BPs should have U prefix — correct A to U (e.g., WBP_AWidget → UWidget)
            DerivedName[0] = TEXT('U');
        }
        // Name already starts with U — no prefix change needed for widget BPs
        Document->ClassName = DerivedName;

        // Auto-select logic-referenced widgets for BindWidget
        SelectedWidgetBindings = InPayload.LogicReferencedWidgets;
    }

    FGuid TabGuid;
    FName TabId;
    FCortexConversionPayload Payload;
    TSharedPtr<FCortexCodeDocument> Document;
    TSharedPtr<FCortexCliSession> Session;
    ECortexConversionScope SelectedScope = ECortexConversionScope::EntireBlueprint;
    FString TargetEventOrFunction;   // For EventOrFunction scope — stores selected event name
    TArray<FString> SelectedFunctions; // For multi-select function scope — stores checked function names
    ECortexConversionDepth SelectedDepth = ECortexConversionDepth::CppCore;
    FString CustomInstructions;        // Used when SelectedDepth == Custom
    ECortexConversionDestination SelectedDestination = ECortexConversionDestination::CreateNewClass;
    FString TargetClassName;       // selected ancestor class name, empty if CreateNewClass
    FString TargetHeaderPath;      // path to existing .h file, empty if CreateNewClass
    FString TargetSourcePath;      // path to existing .cpp file, empty if CreateNewClass

    // Original file content — read once at conversion start for diff view and prompt assembly
    FString OriginalHeaderText;
    FString OriginalSourceText;  // empty if header-only class

    // Build verification checkbox state
    bool bVerifyAfterSave = false;

    bool bConversionStarted = false;
    bool bIsInitialGeneration = true;

    // Widget binding selection — auto-populated from LogicReferencedWidgets, user can adjust
    TArray<FString> SelectedWidgetBindings;

    // Token estimation (populated by background serialization on config panel open)
    int32 EstimatedTotalTokens = 0;   // EntireBlueprint scope total
    bool bTokenEstimateReady = false;
    TMap<FString, int32> PerFunctionTokens;  // per event/function token estimates
};
