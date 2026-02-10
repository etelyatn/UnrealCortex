#pragma once

#include "Modules/ModuleInterface.h"

class FCortexDataModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
