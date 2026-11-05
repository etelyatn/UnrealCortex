#include "CortexBPAdoptTestTypes.h"

ACortexBPAdoptComponentParent::ACortexBPAdoptComponentParent()
{
	NativeMirror = CreateDefaultSubobject<UCortexBPAdoptNativeComponent>(TEXT("NativeMirror"));
	RootComponent = NativeMirror;
}
