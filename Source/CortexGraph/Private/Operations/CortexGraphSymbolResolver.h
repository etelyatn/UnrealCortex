#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UClass;
class UFunction;
class FProperty;

enum class ECortexCallKind : uint8
{
	Ordinary,
	InterfaceMessage,
	Parent,
	Unsupported
};

struct FCortexResolvedSymbol
{
	FString DeclaringClassPath;
	FString ContextClassPath;
	FName MemberName = NAME_None;
	ECortexCallKind CallKind = ECortexCallKind::Ordinary;

	bool bIsFunction = false;
	bool bIsProperty = false;
	bool bIsPure = false;
	bool bIsConst = false;
	bool bIsBlueprintCallable = false;
	bool bIsBlueprintPure = false;
	bool bIsBlueprintVisible = false;
	bool bIsBlueprintReadOnly = false;
	bool bIsProtected = false;
	bool bIsPrivate = false;
	bool bIsStatic = false;

	// Request-local pointers valid ONLY during the request validity window
	UClass* DeclaringClass = nullptr;
	UClass* ContextClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* Property = nullptr;

	TSharedRef<FJsonObject> ToJson() const;
};

class FCortexGraphSymbolResolver
{
public:
	/** Resolve a class identifier (canonical path or short name) to a UClass*. */
	static bool ResolveClass(
		const FString& ClassIdentifier,
		UClass*& OutClass,
		FCortexCommandResult& OutError,
		TArray<FString>* OutAmbiguityCandidates = nullptr);

	/** Parse a member selector from params, validating canonical vs combined fields. */
	static bool ParseMemberSelector(
		const TSharedPtr<FJsonObject>& Params,
		const FString& MemberFieldName,
		FString& OutOwnerClass,
		FString& OutMemberName,
		FCortexCommandResult& OutError);

	/** Resolve a function symbol on the given Blueprint or owner class. */
	static bool ResolveFunction(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		FCortexResolvedSymbol& OutSymbol,
		FCortexCommandResult& OutError);

	/** Resolve a property symbol on the given Blueprint or owner class. */
	static bool ResolveProperty(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params,
		bool bIsWrite,
		FCortexResolvedSymbol& OutSymbol,
		FCortexCommandResult& OutError);

	/** Call kind to string helper. */
	static FString CallKindToString(ECortexCallKind Kind);

	/** Parse call kind string. */
	static bool TryParseCallKind(const FString& KindString, ECortexCallKind& OutKind);
};
