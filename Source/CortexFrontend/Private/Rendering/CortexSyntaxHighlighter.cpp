#include "Rendering/CortexSyntaxHighlighter.h"

namespace
{
    const TSet<FString>& GetKeywords()
    {
        static TSet<FString> Keywords = {
            TEXT("void"), TEXT("int"), TEXT("float"), TEXT("double"), TEXT("bool"),
            TEXT("char"), TEXT("short"), TEXT("long"), TEXT("unsigned"), TEXT("signed"),
            TEXT("const"), TEXT("static"), TEXT("inline"), TEXT("virtual"), TEXT("override"),
            TEXT("final"), TEXT("explicit"), TEXT("class"), TEXT("struct"), TEXT("enum"),
            TEXT("namespace"), TEXT("template"), TEXT("typename"), TEXT("return"),
            TEXT("if"), TEXT("else"), TEXT("for"), TEXT("while"), TEXT("do"),
            TEXT("switch"), TEXT("case"), TEXT("break"), TEXT("continue"),
            TEXT("new"), TEXT("delete"), TEXT("nullptr"), TEXT("true"), TEXT("false"),
            TEXT("this"), TEXT("public"), TEXT("private"), TEXT("protected"), TEXT("friend"),
            TEXT("using"), TEXT("typedef"), TEXT("auto"), TEXT("operator"),
            TEXT("try"), TEXT("catch"), TEXT("throw"), TEXT("noexcept"),
            TEXT("constexpr"), TEXT("static_assert"), TEXT("sizeof"), TEXT("decltype")
        };
        return Keywords;
    }

    bool IsWordChar(TCHAR C)
    {
        return FChar::IsAlpha(C) || FChar::IsDigit(C) || C == TEXT('_');
    }

    bool IsWordBoundaryBefore(const FString& Line, int32 Index)
    {
        return Index == 0 || !IsWordChar(Line[Index - 1]);
    }

    bool IsWordBoundaryAfter(const FString& Line, int32 Index)
    {
        return Index >= Line.Len() || !IsWordChar(Line[Index]);
    }
}

TArray<FCortexSyntaxRun> CortexSyntaxHighlighter::Tokenize(const FString& Line)
{
    TArray<FCortexSyntaxRun> Runs;
    const int32 Len = Line.Len();
    int32 Pos = 0;
    int32 DefaultStart = 0;

    auto FlushDefault = [&](int32 End)
    {
        if (DefaultStart < End)
        {
            FCortexSyntaxRun Run;
            Run.Text = Line.Mid(DefaultStart, End - DefaultStart);
            Run.Type = ECortexSyntaxTokenType::Default;
            Run.StartIndex = DefaultStart;
            Runs.Add(MoveTemp(Run));
        }
    };

    auto AddRun = [&](int32 Start, int32 End, ECortexSyntaxTokenType TokenType)
    {
        FlushDefault(Start);
        FCortexSyntaxRun Run;
        Run.Text = Line.Mid(Start, End - Start);
        Run.Type = TokenType;
        Run.StartIndex = Start;
        Runs.Add(MoveTemp(Run));
        Pos = End;
        DefaultStart = Pos;
    };

    // Check for preprocessor line
    int32 TrimStart = 0;
    while (TrimStart < Len && (Line[TrimStart] == TEXT(' ') || Line[TrimStart] == TEXT('\t')))
    {
        ++TrimStart;
    }
    if (TrimStart < Len && Line[TrimStart] == TEXT('#'))
    {
        FCortexSyntaxRun Run;
        Run.Text = Line;
        Run.Type = ECortexSyntaxTokenType::Preprocessor;
        Run.StartIndex = 0;
        Runs.Add(MoveTemp(Run));
        return Runs;
    }

    while (Pos < Len)
    {
        // Line comment
        if (Pos + 1 < Len && Line[Pos] == TEXT('/') && Line[Pos + 1] == TEXT('/'))
        {
            AddRun(Pos, Len, ECortexSyntaxTokenType::Comment);
            break;
        }

        // String literal
        if (Line[Pos] == TEXT('"'))
        {
            int32 End = Pos + 1;
            while (End < Len)
            {
                if (Line[End] == TEXT('\\') && End + 1 < Len)
                {
                    End += 2;
                }
                else if (Line[End] == TEXT('"'))
                {
                    ++End;
                    break;
                }
                else
                {
                    ++End;
                }
            }
            AddRun(Pos, End, ECortexSyntaxTokenType::String);
            continue;
        }

        // Number — must start at word boundary
        if (FChar::IsDigit(Line[Pos]) && IsWordBoundaryBefore(Line, Pos))
        {
            int32 End = Pos + 1;
            while (End < Len && (FChar::IsDigit(Line[End]) || FChar::IsAlpha(Line[End]) || Line[End] == TEXT('.') || Line[End] == TEXT('x') || Line[End] == TEXT('X')))
            {
                ++End;
            }
            // Consume optional suffix
            while (End < Len && (Line[End] == TEXT('f') || Line[End] == TEXT('F') || Line[End] == TEXT('l') || Line[End] == TEXT('L') || Line[End] == TEXT('u') || Line[End] == TEXT('U')))
            {
                ++End;
            }
            if (IsWordBoundaryAfter(Line, End))
            {
                AddRun(Pos, End, ECortexSyntaxTokenType::Number);
                continue;
            }
        }

        // Identifier-based tokens (keyword, UE type, function)
        if (FChar::IsAlpha(Line[Pos]) || Line[Pos] == TEXT('_'))
        {
            int32 End = Pos + 1;
            while (End < Len && IsWordChar(Line[End]))
            {
                ++End;
            }
            FString Word = Line.Mid(Pos, End - Pos);

            // Keyword
            if (GetKeywords().Contains(Word))
            {
                AddRun(Pos, End, ECortexSyntaxTokenType::Keyword);
                continue;
            }

            // UE Type: starts with F, U, A, T, E followed by uppercase
            if (Word.Len() >= 2 && (Word[0] == TEXT('F') || Word[0] == TEXT('U') || Word[0] == TEXT('A') || Word[0] == TEXT('T') || Word[0] == TEXT('E')) && FChar::IsUpper(Word[1]))
            {
                AddRun(Pos, End, ECortexSyntaxTokenType::UEType);
                continue;
            }

            // Function: identifier followed by '('
            int32 LookAhead = End;
            while (LookAhead < Len && (Line[LookAhead] == TEXT(' ') || Line[LookAhead] == TEXT('\t')))
            {
                ++LookAhead;
            }
            if (LookAhead < Len && Line[LookAhead] == TEXT('('))
            {
                AddRun(Pos, End, ECortexSyntaxTokenType::Function);
                continue;
            }

            // Default identifier
            Pos = End;
            continue;
        }

        ++Pos;
    }

    FlushDefault(Len);
    return Runs;
}

TArray<TArray<FCortexSyntaxRun>> CortexSyntaxHighlighter::TokenizeBlock(const FString& Code)
{
    TArray<TArray<FCortexSyntaxRun>> Result;
    TArray<FString> Lines;
    Code.ParseIntoArray(Lines, TEXT("\n"), false);
    for (const FString& Line : Lines)
    {
        Result.Add(Tokenize(Line));
    }
    return Result;
}
