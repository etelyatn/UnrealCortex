#pragma once

#include "CoreMinimal.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"

/**
 * Custom marshaller for chat messages that parses <Bold>, <Italic>, <Code>
 * tags and creates styled FSlateTextRun entries. Bypasses FRichTextLayoutMarshaller
 * which doesn't render inline style tags in SMultiLineEditableText.
 *
 * Follows the same pattern as FCortexCodeMarshaller in SCortexCodeBlock.cpp.
 */
class FCortexChatMarshaller : public FBaseTextLayoutMarshaller
{
public:
	static TSharedRef<FCortexChatMarshaller> Create();

	virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
	virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;

private:
	FCortexChatMarshaller() = default;
};
