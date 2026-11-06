#pragma once

#include "CoreMinimal.h"

#include "UObject/Interface.h"

#include "CortexGraphInterfaceTestTypes.generated.h"

/**
 * Test-only native interface for the #122 interface-implementation migration coverage. GetValue has an output, so the engine
 * cannot place it as an event (function-style: its graph lives in FBPInterfaceDescription::Graphs); Notify is void, so it is
 * placed as an event node.
 */
UINTERFACE(Blueprintable, MinimalAPI)
class UCortexIssue122GraphInterface : public UInterface
{
	GENERATED_BODY()
};

class ICortexIssue122GraphInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "CortexIssue122")
	double GetValue();

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "CortexIssue122")
	void Notify(int32 Amount);
};
