#include "CortexEditorCPUThrottleBypass.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"

namespace
{
int32 GCortexCPUThrottleBypassLeaseCount = 0;
FDelegateHandle GCortexCPUThrottleBypassVetoHandle;

bool CortexVetoEditorCPUThrottle()
{
	return true;
}
}

FCortexEditorCPUThrottleBypassLease::FCortexEditorCPUThrottleBypassLease()
{
	if (++GCortexCPUThrottleBypassLeaseCount == 1 && GEditor != nullptr)
	{
		UEditorEngine::FShouldDisableCPUThrottling Veto =
			UEditorEngine::FShouldDisableCPUThrottling::CreateStatic(&CortexVetoEditorCPUThrottle);
		GCortexCPUThrottleBypassVetoHandle = Veto.GetHandle();
		GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Veto));
	}
}

FCortexEditorCPUThrottleBypassLease::~FCortexEditorCPUThrottleBypassLease()
{
	if (--GCortexCPUThrottleBypassLeaseCount == 0 && GCortexCPUThrottleBypassVetoHandle.IsValid())
	{
		if (GEditor != nullptr)
		{
			GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
				[](const UEditorEngine::FShouldDisableCPUThrottling& Delegate)
				{
					return Delegate.GetHandle() == GCortexCPUThrottleBypassVetoHandle;
				});
		}
		GCortexCPUThrottleBypassVetoHandle.Reset();
	}
}
