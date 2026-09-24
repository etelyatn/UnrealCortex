#pragma once

#include "Interfaces/IPluginManager.h"

namespace CortexPluginIdentity
{
inline const FString& GetVersionName()
{
	static const FString VersionName = []()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
		checkf(Plugin.IsValid(), TEXT("UnrealCortex plugin descriptor is unavailable"));
		return Plugin->GetDescriptor().VersionName;
	}();
	return VersionName;
}
}
