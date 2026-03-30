#include "CortexFileUtils.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

bool FCortexFileUtils::AtomicWriteFile(const FString& FilePath, const FString& Content)
{
	const FString TempPath = FilePath + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	if (!FFileHelper::SaveStringToFile(Content, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}

	return IFileManager::Get().Move(*FilePath, *TempPath, true, true, true, true);
}
