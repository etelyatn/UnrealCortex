#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UEdGraphPin;

class FCortexGraphPinDefaults
{
public:
	/**
	 * Validates whether a tagged literal descriptor can be applied to the given input pin.
	 * Returns true if valid, or false and sets OutError if invalid.
	 */
	static bool Validate(
		const UEdGraphPin* Pin,
		const TSharedPtr<FJsonObject>& Literal,
		FCortexCommandResult& OutError);

	/**
	 * Applies a validated tagged literal descriptor to the pin in-memory.
	 * Strictly NO persistence: does not save packages or reload them.
	 */
	static bool ApplyDefault(
		UEdGraphPin* Pin,
		const TSharedPtr<FJsonObject>& Literal,
		FCortexCommandResult& OutError);

	/**
	 * Compares one planned tagged literal descriptor against the pin's native default state.
	 *
	 * Returns false and sets OutFailure when the literal cannot be compared with native state at
	 * all (unsupported kind, unresolvable symbol, or a requested reference mode the pin's storage
	 * cannot hold); callers must treat that as a mismatch, never as success. On success
	 * OutExpected/OutActual hold canonical identities (including reference storage mode and
	 * string-table identity) and the defaults match exactly when they are equal.
	 */
	static bool CompareAppliedLiteral(
		const UEdGraphPin* Pin,
		const TSharedPtr<FJsonObject>& Literal,
		FString& OutExpected,
		FString& OutActual,
		FString& OutFailure);

	/**
	 * Tagged literal kind that matches a reference pin's native storage mode ("class", "soft_class",
	 * "object", "soft_object"). Descriptors must use this kind so they validate against the pin.
	 */
	static const TCHAR* ReferenceLiteralKind(const UEdGraphPin& Pin);

	/**
	 * Reads the default value of a pin into a tagged literal descriptor.
	 * Does not perform persistence or reload.
	 */
	static bool ReadDefault(
		const UEdGraphPin* Pin,
		TSharedPtr<FJsonObject>& OutDescriptor,
		FCortexCommandResult& OutError);

	/**
	 * Helper to apply FText default without saving/reloading the package.
	 */
	static bool ApplyTextDefault(
		UEdGraphPin* Pin,
		const TSharedPtr<FJsonObject>& TextObject,
		FCortexCommandResult& OutError);
};
