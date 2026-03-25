// CortexFrontendColors.h
#pragma once

#include "CoreMinimal.h"

namespace CortexColors
{
    // Message layout
    inline const FLinearColor UserRowBackground    = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1e2a1e")));
    inline const FLinearColor UserRowBorderLeft     = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("3a5a3a")));
    inline const FLinearColor UserPrefixColor       = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("6a9955")));
    inline const FLinearColor AssistantDotColor     = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("444444")));
    inline const FLinearColor TextPrimary           = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("e0e0e0")));
    inline const FLinearColor TextSecondary         = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("cccccc")));

    // Tool call block
    inline const FLinearColor ToolBlockBackground   = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1a1a1a")));
    inline const FLinearColor ToolBlockBorder        = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2a2a2a")));
    inline const FLinearColor ToolLabelColor         = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("666666")));
    inline const FLinearColor ToolDurationColor      = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("444444")));
    inline const FLinearColor ToolFileColor          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2b96ff")));

    // Tool type icons
    inline const FLinearColor IconRead               = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0d82f3")));
    inline const FLinearColor IconSearch             = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("f59e0b")));
    inline const FLinearColor IconEdit               = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("4ade80")));
    inline const FLinearColor IconShell              = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("e0e0e0")));
    inline const FLinearColor IconMcp                = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("c586c0")));
    inline const FLinearColor IconDefault            = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("666666")));

    // Code block
    inline const FLinearColor CodeBackground         = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0d0d0d")));
    inline const FLinearColor CodeHeaderBackground   = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1a1a1a")));
    inline const FLinearColor CodeBorder             = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("383838")));
    inline const FLinearColor CodeHeaderBorder        = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2a2a2a")));
    inline const FLinearColor CodeLangColor          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("666666")));
    inline const FLinearColor CodeButtonBorder        = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("333333")));

    // Input bar
    inline const FLinearColor InputBackground        = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("181818")));
    inline const FLinearColor ChipBackground          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("222222")));
    inline const FLinearColor ChipBorder              = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("333333")));
    inline const FLinearColor ChipNameColor           = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2b96ff")));
    inline const FLinearColor ModeButtonBackground    = FLinearColor(0.29f, 0.87f, 0.50f, 0.10f);  // rgba(74,222,128,0.10)
    inline const FLinearColor ModeButtonText          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("4ade80")));
    inline const FLinearColor SendButtonColor         = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0d82f3")));
    inline const FLinearColor MutedTextColor          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("555555")));

    // Table
    inline const FLinearColor TableBackground         = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("141414")));
    inline const FLinearColor TableBorder              = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2a2a2a")));
    inline const FLinearColor TableHeaderBackground   = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1a1a1a")));
    inline const FLinearColor TableHeaderText          = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("666666")));
    inline const FLinearColor TableRowHover           = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("1e1e1e")));
    inline const FLinearColor BadgeOkText              = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("4ade80")));
    inline const FLinearColor BadgeOkBackground        = FLinearColor(0.29f, 0.87f, 0.50f, 0.12f);
    inline const FLinearColor BadgeErrorText           = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("ef4444")));
    inline const FLinearColor BadgeErrorBackground     = FLinearColor(0.94f, 0.27f, 0.27f, 0.12f);
    inline const FLinearColor BadgeWarnText            = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("f59e0b")));
    inline const FLinearColor BadgeWarnBackground      = FLinearColor(0.96f, 0.62f, 0.04f, 0.12f);
    inline const FLinearColor BadgeInfoText            = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("0d82f3")));
    inline const FLinearColor BadgeInfoBackground      = FLinearColor(0.05f, 0.51f, 0.95f, 0.15f);
    inline const FLinearColor MonoText                 = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("4ec9b0")));
    inline const FLinearColor CountBadgeBackground    = FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("222222")));
}
