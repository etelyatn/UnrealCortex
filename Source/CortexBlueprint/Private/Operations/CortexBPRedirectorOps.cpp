#include "Operations/CortexBPRedirectorOps.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "CoreGlobals.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectIterator.h"
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

	Path = FCortexEditorUtils::NormalizeMountedContentPath(Path);

	FString ValidationError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(Path, ValidationError))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			ValidationError);
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

	const FString PathWithSlash = Path.EndsWith(TEXT("/")) ? Path : Path / TEXT("");
	for (TObjectIterator<UObjectRedirector> It; It; ++It)
	{
		UObjectRedirector* Redirector = *It;
		if (!IsValid(Redirector))
		{
			continue;
		}

		const FString RedirectorPackageName = Redirector->GetOutermost()->GetName();
		if ((RedirectorPackageName == Path || RedirectorPackageName.StartsWith(PathWithSlash)) &&
			!Redirectors.Contains(Redirector))
		{
			Redirectors.Add(Redirector);
		}
	}

	int32 DirectlyDeletedRedirectors = 0;
	TArray<UObjectRedirector*> RedirectorsWithReferencers;
	RedirectorsWithReferencers.Reserve(Redirectors.Num());
	TArray<UObject*> UnreferencedRedirectors;
	for (UObjectRedirector* Redirector : Redirectors)
	{
		TArray<FAssetIdentifier> Referencers;
		AssetRegistry.GetReferencers(FAssetIdentifier(Redirector->GetOutermost()->GetFName()), Referencers);
		if (Referencers.Num() > 0)
		{
			RedirectorsWithReferencers.Add(Redirector);
		}
		else
		{
			UnreferencedRedirectors.Add(Redirector);
		}
	}

	if (UnreferencedRedirectors.Num() > 0)
	{
		DirectlyDeletedRedirectors = ObjectTools::ForceDeleteObjects(UnreferencedRedirectors, false);
	}

	if (RedirectorsWithReferencers.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Fixup Redirectors in %s"), *Path)
		));
		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		AssetToolsModule.Get().FixupReferencers(
			RedirectorsWithReferencers,
			false,
			ERedirectFixupMode::DeleteFixedUpRedirectors);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Path);
	Data->SetBoolField(TEXT("recursive"), bRecursive);
	Data->SetNumberField(TEXT("redirectors_found"), Redirectors.Num());
	Data->SetNumberField(TEXT("redirectors_fixed"), DirectlyDeletedRedirectors + RedirectorsWithReferencers.Num());

	return FCortexCommandRouter::Success(Data);
}
