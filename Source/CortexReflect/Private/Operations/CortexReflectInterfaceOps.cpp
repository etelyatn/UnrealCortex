#include "Operations/CortexReflectInterfaceOps.h"

#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"

TArray<FString> FCortexReflectInterfaceOps::GetImplementedInterfacePaths(const FAssetData& Asset)
{
	TArray<FString> Paths;
	const FAssetTagValueRef Tag = Asset.TagsAndValues.FindTag(FBlueprintTags::ImplementedInterfaces);
	if (!Tag.IsSet())
	{
		return Paths;
	}

	// The tag is the exported ImplementedInterfaces array: "((Interface=<class path>,...),(Interface=...))" where a class
	// path may be quoted and prefixed by its class ("/Script/CoreUObject.Class'/Game/X.BPI_C'").
	static const FString InterfaceIdentifier(TEXT("Interface="));
	FString Rest = Tag.AsString();
	int32 Start = Rest.Find(InterfaceIdentifier);
	while (Start != INDEX_NONE)
	{
		Rest.RightChopInline(Start + InterfaceIdentifier.Len());
		int32 End = INDEX_NONE;
		for (int32 Index = 0; Index < Rest.Len(); ++Index)
		{
			const TCHAR Char = Rest[Index];
			if (Char == TEXT(',') || Char == TEXT(')'))
			{
				End = Index;
				break;
			}
		}
		FString Token = End == INDEX_NONE ? Rest : Rest.Left(End);
		const int32 Quote = Token.Find(TEXT("'"));
		if (Quote != INDEX_NONE)
		{
			Token.RightChopInline(Quote + 1);
		}
		Token.RemoveFromStart(TEXT("\""));
		while (Token.EndsWith(TEXT("'")) || Token.EndsWith(TEXT("\"")))
		{
			Token.LeftChopInline(1);
		}
		if (!Token.IsEmpty())
		{
			Paths.Add(Token);
		}
		Start = Rest.Find(InterfaceIdentifier);
	}
	return Paths;
}

bool FCortexReflectInterfaceOps::TagImplementsInterface(const FAssetData& Asset, const UClass* Interface)
{
	if (!Interface)
	{
		return false;
	}
	const FString InterfacePath = Interface->GetAuthoritativeClass()->GetClassPathName().ToString();
	for (const FString& Path : GetImplementedInterfacePaths(Asset))
	{
		if (Path.Equals(InterfacePath, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}
