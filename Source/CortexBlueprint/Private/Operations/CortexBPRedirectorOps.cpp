#include "Operations/CortexBPRedirectorOps.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "CoreGlobals.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectRedirector.h"
#include "ScopedTransaction.h"

FCortexCommandResult FCortexBPRedirectorOps::FixupRedirectors(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing params object"));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: path"));
	}

	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
	}
	else if (!Path.StartsWith(TEXT("/Game/")))
	{
		Path = TEXT("/Game") + Path;
	}

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*Path));
	Filter.bRecursivePaths = bRecursive;

	TArray<FAssetData> RedirectorAssets;
	AssetRegistry.GetAssets(Filter, RedirectorAssets);

	TArray<UObjectRedirector*> Redirectors;
	Redirectors.Reserve(RedirectorAssets.Num());
	for (const FAssetData& AssetData : RedirectorAssets)
	{
		if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
		{
			Redirectors.Add(Redirector);
		}
	}

	if (Redirectors.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Fixup Redirectors in %s"), *Path)
		));
		// Suppress SFixupRedirectorsReport modal dialog in live editor MCP sessions.
		// GIsRunningUnattendedScript is checked by FSlateApplication::AddModalWindow() — when true
		// it logs a LogSlate Warning and returns without showing the window.
		// Tests are unaffected: NullRHI causes CanAddModalWindow() to return false before this check.
		TGuardValue<bool> SuppressDialogs(GIsRunningUnattendedScript, true);
		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		AssetToolsModule.Get().FixupReferencers(Redirectors);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Path);
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetNumberField(TEXT("redirectors_found"), RedirectorAssets.Num());
	Data->SetNumberField(TEXT("redirectors_fixed"), Redirectors.Num());

	return FCortexCommandRouter::Success(Data);
}
