#include "CortexBPTestLiftActor.h"

UCortexBPTestSubobjComponent::UCortexBPTestSubobjComponent()
{
	Payload = CreateDefaultSubobject<UCortexBPTestSubobjPayload>(TEXT("Payload"));
	PlainPayload = nullptr;
}

ACortexBPTestLiftActor::ACortexBPTestLiftActor()
{
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
}
