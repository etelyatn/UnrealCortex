#include "Operations/CortexLevelLifecycleOps.h"

#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"

FCortexCommandResult FCortexLevelLifecycleOps::ListTemplates(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::CreateLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::OpenLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::DuplicateLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::RenameLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

FCortexCommandResult FCortexLevelLifecycleOps::DeleteLevel(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Not implemented"));
}

bool FCortexLevelLifecycleOps::IsLevelCurrentlyOpen(const FString& ContentPath)
{
	return false;
}

bool FCortexLevelLifecycleOps::IsCurrentLevelDirty()
{
	return false;
}

bool FCortexLevelLifecycleOps::IsValidContentPath(const FString& Path)
{
	return Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Plugins/"));
}

bool FCortexLevelLifecycleOps::DoesLevelExist(const FString& ContentPath)
{
	return false;
}
