#pragma once

#include "Modules/ModuleInterface.h"

CORTEXEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogCortexEditor, Log, All);

class CORTEXEDITOR_API FCortexEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
