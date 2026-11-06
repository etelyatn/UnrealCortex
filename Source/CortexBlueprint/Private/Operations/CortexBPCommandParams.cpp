#include "Operations/CortexBPCommandParams.h"

#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CortexBPParams
{
	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default, bool& OutValue, FCortexCommandResult& OutError)
	{
		OutValue = Default;
		if (!Params->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Params->TryGetBoolField(Field, OutValue))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a boolean"), Field));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String || !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Missing required param: %s (a non-empty string)"), Field));
			return false;
		}
		return true;
	}

	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues, bool& bOutPresent, FCortexCommandResult& OutError)
	{
		bOutPresent = Params->HasField(Field);
		if (!bOutPresent)
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be an array of strings"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Text) || Text.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be an array of non-empty strings"), Field));
				return false;
			}
			OutValues.Add(Text);
		}
		return true;
	}

	bool ReadExpectedFingerprint(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutExpected, FCortexCommandResult& OutError)
	{
		OutExpected.Reset();
		if (!Params->HasField(TEXT("expected_fingerprint")))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* ExpectedField = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedField) || ExpectedField == nullptr || !ExpectedField->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
			return false;
		}
		OutExpected = *ExpectedField;
		return true;
	}

	bool CheckAcknowledged(const TArray<FString>& Required, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError)
	{
		if (Required == Acknowledged)
		{
			return true;
		}
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(Required));
		Details->SetArrayField(TEXT("acknowledged"), ToJsonStrings(Acknowledged));
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::PotentialDataLoss,
			TEXT("The change would lose data; echo required_acknowledgment as acknowledged_losses to proceed (keys that are not required are refused as stale)"), Details);
		return false;
	}
}
