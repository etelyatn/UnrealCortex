#pragma once
#include "CoreMinimal.h"
#include "Framework/Text/ITextLayoutMarshaller.h"
#include "Styling/SlateTypes.h"

/**
 * Text layout marshaller that colors @mention tokens blue.
 * Passed to SMultiLineEditableTextBox so @word patterns render inline
 * with a distinct color without requiring a custom widget.
 */
class FCortexMentionMarshaller : public ITextLayoutMarshaller
{
public:
    static TSharedRef<FCortexMentionMarshaller> Create(const FSlateFontInfo& InFont);
    explicit FCortexMentionMarshaller(const FSlateFontInfo& InFont);

    virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
    virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;
    virtual bool RequiresLiveUpdate() const override { return false; }
    virtual void MakeDirty() override { bDirty = true; }
    virtual void ClearDirty() override { bDirty = false; }
    virtual bool IsDirty() const override { return bDirty; }

private:
    bool bDirty = false;
    FTextBlockStyle NormalStyle;
    FTextBlockStyle MentionStyle;
};
