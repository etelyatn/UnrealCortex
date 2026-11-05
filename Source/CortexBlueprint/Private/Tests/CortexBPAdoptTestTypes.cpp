#include "CortexBPAdoptTestTypes.h"

ACortexBPAdoptParent::ACortexBPAdoptParent()
{
	// Differs from AActor's default so a reparent that keeps the old CDO value is detectable.
	InitialLifeSpan = 7.f;
}

ACortexBPAdoptComponentParent::ACortexBPAdoptComponentParent()
{
	NativeMirror = CreateDefaultSubobject<UCortexBPAdoptNativeComponent>(TEXT("NativeMirror"));
	RootComponent = NativeMirror;
}
