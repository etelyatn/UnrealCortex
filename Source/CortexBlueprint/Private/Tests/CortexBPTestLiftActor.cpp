#include "CortexBPTestLiftActor.h"

UCortexBPTestSubobjComponent::UCortexBPTestSubobjComponent()
{
	Payload = CreateDefaultSubobject<UCortexBPTestSubobjPayload>(TEXT("Payload"));
}

ACortexBPTestLiftActor::ACortexBPTestLiftActor()
{
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
}
