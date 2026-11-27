#include "Operations/CortexDirtyPackageOps.h"

#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/PackagePath.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

bool FCortexDirtyPackageOps::IsExcludedPackage(const UPackage* Package)
{
	if (Package == nullptr || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
	{
		return true;
	}

	if (Package->HasAnyPackageFlags(PKG_ContainsScript | PKG_CompiledIn | PKG_PlayInEditor))
	{
		return true;
	}

	const FString PackageName = Package->GetName();
	return FPackageName::IsScriptPackage(PackageName) || FPackageName::IsMemoryPackage(PackageName);
}

TArray<FCortexDirtyPackageEntry> FCortexDirtyPackageOps::CollectDirtyPackages()
{
	TArray<FCortexDirtyPackageEntry> Entries;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		const UPackage* Package = *It;
		if (!Package->IsDirty() || IsExcludedPackage(Package))
		{
			continue;
		}

		FCortexDirtyPackageEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.PackageName = Package->GetName();
		// Packages outside a mounted root (a dirty package that outlived its unmounted content root) have no file;
		// TryFromMountedName keeps DoesPackageExist from logging a warning for them.
		FPackagePath SourcePath;
		FPackagePath ExistingPath;
		if (FPackagePath::TryFromMountedName(Entry.PackageName, SourcePath)
			&& FPackageName::DoesPackageExist(SourcePath, &ExistingPath))
		{
			Entry.File = FPaths::ConvertRelativePathToFull(ExistingPath.GetLocalFullPath());
		}
	}

	Entries.Sort([](const FCortexDirtyPackageEntry& A, const FCortexDirtyPackageEntry& B)
	{
		return A.PackageName < B.PackageName;
	});
	return Entries;
}

TArray<TSharedPtr<FJsonValue>> FCortexDirtyPackageOps::BuildEntriesJson(const TArray<FCortexDirtyPackageEntry>& Entries)
{
	TArray<TSharedPtr<FJsonValue>> PackagesJson;
	PackagesJson.Reserve(Entries.Num());
	for (const FCortexDirtyPackageEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("package"), Entry.PackageName);
		EntryJson->SetStringField(TEXT("file"), Entry.File);
		PackagesJson.Add(MakeShared<FJsonValueObject>(EntryJson));
	}

	return PackagesJson;
}

FCortexCommandResult FCortexDirtyPackageOps::ListDirtyPackages(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	const TArray<FCortexDirtyPackageEntry> Entries = CollectDirtyPackages();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Entries.Num());
	Data->SetArrayField(TEXT("packages"), BuildEntriesJson(Entries));
	return FCortexCommandRouter::Success(Data);
}
