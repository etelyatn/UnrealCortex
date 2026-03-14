#pragma once

#include "CoreMinimal.h"

enum class ECortexSyntaxTokenType : uint8
{
    Default,
    Keyword,
    String,
    Comment,
    Preprocessor,
    Number,
    UEType,
    Function
};

struct FCortexSyntaxRun
{
    FString Text;
    ECortexSyntaxTokenType Type = ECortexSyntaxTokenType::Default;
    int32 StartIndex = 0;
};

namespace CortexSyntaxHighlighter
{
    /** Tokenize a single line of C++ code into syntax runs. */
    TArray<FCortexSyntaxRun> Tokenize(const FString& Line);

    /** Tokenize a multi-line code block. Returns runs per line. */
    TArray<TArray<FCortexSyntaxRun>> TokenizeBlock(const FString& Code);
}
