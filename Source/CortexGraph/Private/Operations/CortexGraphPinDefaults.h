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
