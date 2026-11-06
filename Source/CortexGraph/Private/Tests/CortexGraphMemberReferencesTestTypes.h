#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "CortexGraphMemberReferencesTestTypes.generated.h"

/** Native parent that declares a member of the same name a Blueprint reference query can hit. */
UCLASS(Blueprintable)
class ACortexGraphMemberRefNativeActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bNativeFlag = false;

	UFUNCTION(BlueprintCallable)
	void NativeCall() {}
};
