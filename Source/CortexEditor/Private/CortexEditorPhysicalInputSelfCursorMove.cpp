#include "CortexEditorPhysicalInputSelfCursorMove.h"

namespace
{
TOptional<FVector2D> GExpectedPosition;
double GExpectedUntilSeconds = 0.0;
}

namespace CortexEditorSelfCursorMove
{
void Expect(const FVector2D& ScreenPosition, double NowSeconds)
{
	GExpectedPosition = ScreenPosition;
	GExpectedUntilSeconds = NowSeconds + GraceSeconds;
}

bool Consume(const FVector2D& ScreenPosition, double NowSeconds)
{
	if (!GExpectedPosition.IsSet())
	{
		return false;
	}
	if (NowSeconds > GExpectedUntilSeconds)
	{
		Reset();
		return false;
	}
	if (FVector2D::Distance(ScreenPosition, GExpectedPosition.GetValue()) > PositionTolerance)
	{
		return false;
	}
	Reset();
	return true;
}

void Reset()
{
	GExpectedPosition.Reset();
	GExpectedUntilSeconds = 0.0;
}
}
