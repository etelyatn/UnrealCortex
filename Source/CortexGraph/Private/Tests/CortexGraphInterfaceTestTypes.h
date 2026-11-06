#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
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

/**
 * Test-only class that is unrelated to every interface of the #122 coverage but declares a function with the same name as the
 * interfaces' `GetValue`: the retarget coverage proves such a call is never reported or rewritten (matching is by resolved owner).
 */
UCLASS()
class UCortexIssue122RetargetUnrelated : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "CortexIssue122")
	static double GetValue() { return 0.0; }
};

/**
 * Test-only actor class for the retarget coverage. Calls to a function of an editor module (the native test interface lives in one)
 * compile only in Blueprints whose base class is itself part of an editor module (`UK2Node_CallFunction::ValidateNodeDuringCompilation`).
 */
UCLASS(Blueprintable)
class ACortexIssue122RetargetCaller : public AActor
{
	GENERATED_BODY()
};
