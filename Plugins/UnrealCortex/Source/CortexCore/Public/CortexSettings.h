
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CortexSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (DisplayName = "Unreal Data Bridge"))
class CORTEXCORE_API UUDBSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** TCP server port. Default: 8742 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 Port = 8742;

	/** Start TCP server automatically when editor loads */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	bool bAutoStart = true;

	/** Log all incoming commands to Output Log */
	UPROPERTY(Config, EditAnywhere, Category = "Debugging")
	bool bLogCommands = false;

	/** Map tag prefix to .ini file for auto-detection in register_gameplay_tag */
	UPROPERTY(Config, EditAnywhere, Category = "GameplayTags")
	TMap<FString, FString> TagPrefixToIniFile;

	static const UUDBSettings* Get()
	{
		return GetDefault<UUDBSettings>();
	}
};
