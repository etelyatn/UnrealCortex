#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "StateTreeSchema.h"
#include "CortexStateTreeTestSchema.generated.h"

UCLASS()
class UCortexStateTreeTestSchema : public UStateTreeSchema
{
	GENERATED_BODY()

public:
	virtual bool IsStructAllowed(const UScriptStruct* InScriptStruct) const override
	{
		return InScriptStruct != nullptr;
	}

	virtual bool IsClassAllowed(const UClass* InClass) const override
	{
		return InClass != nullptr;
	}

	virtual bool IsExternalItemAllowed(const UStruct& InStruct) const override
	{
		return true;
	}

#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	// UStateTreeSchema::IsScheduledTickAllowed arrived with 5.6 scheduled tick.
	virtual bool IsScheduledTickAllowed() const override
	{
		return true;
	}
#endif
};

UCLASS(Abstract)
class UCortexStateTreeAbstractSchema : public UCortexStateTreeTestSchema
{
	GENERATED_BODY()
};

UCLASS(HideDropdown)
class UCortexStateTreeHiddenSchema : public UCortexStateTreeTestSchema
{
	GENERATED_BODY()
};
