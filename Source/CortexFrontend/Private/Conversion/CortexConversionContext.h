#pragma once

#include "CoreMinimal.h"
#include "CortexConversionTypes.h"

class FCortexCliSession;

// ── Code tab enum ──
enum class ECortexCodeTab : uint8 { Header, Implementation, Snippet };

// ── Code document — shared data object observed by canvas and chat ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCodeDocumentChanged, ECortexCodeTab /*ChangedTab*/);

struct FCortexCodeDocument
{
    FString HeaderCode;
    FString ImplementationCode;
    FString SnippetCode;
    FString ClassName;
    FString TargetPath;
    bool bIsSnippetMode = false;

    FOnCodeDocumentChanged OnDocumentChanged;

    void UpdateHeader(const FString& NewCode)
    {
        HeaderCode = NewCode;
        OnDocumentChanged.Broadcast(ECortexCodeTab::Header);
    }

    void UpdateImplementation(const FString& NewCode)
    {
        ImplementationCode = NewCode;
        OnDocumentChanged.Broadcast(ECortexCodeTab::Implementation);
    }

    void UpdateSnippet(const FString& NewCode)
    {
        SnippetCode = NewCode;
        OnDocumentChanged.Broadcast(ECortexCodeTab::Snippet);
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
            if (InPayload.ParentClassName.Contains(TEXT("Actor"))
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
        Document->ClassName = DerivedName;
    }

    FGuid TabGuid;
    FName TabId;
    FCortexConversionPayload Payload;
    TSharedPtr<FCortexCodeDocument> Document;
    TSharedPtr<FCortexCliSession> Session;
    ECortexConversionScope SelectedScope = ECortexConversionScope::EntireBlueprint;
    FString TargetEventOrFunction;  // For EventOrFunction scope — stores selected name
    bool bConversionStarted = false;
    bool bIsInitialGeneration = true;
};
