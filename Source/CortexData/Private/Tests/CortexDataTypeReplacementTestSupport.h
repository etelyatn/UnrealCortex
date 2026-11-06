#pragma once

// Shared fixtures for the #122 user-type replacement tests (plan_type_replacement, verify_type_replacement and the
// CoreRedirects gate). Assets are real saved packages under /Game/Temp/Issue122_<8 hex> so the Asset Registry sees real tags and
// dependencies and a test can unload and reload them under redirects.
//
// GC-safety invariant: the editor undo history is emptied BEFORE any fixture object is retired, because transaction records
// name fixture classes/structs and a later collection would scan freed objects.

#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "CortexDataCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "CortexDataTypeReplacementTestTypes.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace CortexIssue122DataTest
{
	inline void ResetUndoHistory()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue 122 test fixture")));
		}
	}

	inline FEdGraphPinType MakePin(const FName Category)
	{
		FEdGraphPinType Type;
		Type.PinCategory = Category;
		return Type;
	}

	inline FEdGraphPinType IntPin() { return MakePin(UEdGraphSchema_K2::PC_Int); }
	inline FEdGraphPinType StringPin() { return MakePin(UEdGraphSchema_K2::PC_String); }
	inline FEdGraphPinType BoolPin() { return MakePin(UEdGraphSchema_K2::PC_Boolean); }

	inline FEdGraphPinType StructPin(UScriptStruct* Struct)
	{
		FEdGraphPinType Type = MakePin(UEdGraphSchema_K2::PC_Struct);
		Type.PinSubCategoryObject = Struct;
		return Type;
	}

	inline FEdGraphPinType EnumPin(UEnum* Enum)
	{
		FEdGraphPinType Type = MakePin(UEdGraphSchema_K2::PC_Byte);
		Type.PinSubCategoryObject = Enum;
		return Type;
	}

	inline TArray<TSharedPtr<FJsonObject>> Objects(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsObject());
			}
		}
		return Out;
	}

	inline TArray<FString> Strings(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsString());
			}
		}
		return Out;
	}

	inline FCortexCommandResult Run(const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
	{
		FCortexDataCommandHandler Handler;
		return Handler.Execute(Command, Params);
	}

	/** The property of Struct named Name: the friendly name for a user-defined struct, the C++ name for a native one. */
	inline const FProperty* FindMember(const UScriptStruct* Struct, const FString& Name)
	{
		if (const UUserDefinedStruct* UserStruct = Cast<UUserDefinedStruct>(Struct))
		{
			return FStructureEditorUtils::GetPropertyByFriendlyName(UserStruct, Name);
		}
		return Struct->FindPropertyByName(FName(*Name));
	}

	inline bool SetMember(const UScriptStruct* Struct, uint8* Memory, const FString& Name, const FString& Text)
	{
		const FProperty* Property = FindMember(Struct, Name);
		return Property != nullptr
			&& Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Memory), nullptr, PPF_None) != nullptr;
	}

	inline FString GetMember(const UScriptStruct* Struct, const uint8* Memory, const FString& Name)
	{
		FString Out;
		if (const FProperty* Property = FindMember(Struct, Name))
		{
			Property->ExportTextItem_Direct(Out, Property->ContainerPtrToValuePtr<void>(Memory), nullptr, nullptr, PPF_None);
		}
		return Out;
	}

	/** Names of the three row members of a struct: the friendly names of the user struct and the C++ names of the native one. */
	struct FRowMembers
	{
		FString Count = TEXT("Count");
		FString Label = TEXT("Label");
		FString Flag = TEXT("Flag");
	};

	inline FRowMembers NativeMembers()
	{
		FRowMembers Members;
		Members.Flag = TEXT("bFlag");
		return Members;
	}

	/** A row: Count, Label, Flag texts. */
	struct FRowValues
	{
		FString Count;
		FString Label;
		FString Flag;

		bool operator==(const FRowValues& Other) const
		{
			return Count == Other.Count && Label == Other.Label && Flag == Other.Flag;
		}
	};

	inline FRowValues ReadRow(const UScriptStruct* Struct, const uint8* Memory, const FRowMembers& Members)
	{
		return FRowValues{ GetMember(Struct, Memory, Members.Count), GetMember(Struct, Memory, Members.Label), GetMember(Struct, Memory, Members.Flag) };
	}

	/** Every row of Table as text, keyed by row name; Members names the struct members. */
	inline TMap<FName, FRowValues> ReadRows(const UDataTable* Table, const FRowMembers& Members)
	{
		TMap<FName, FRowValues> Rows;
		if (Table != nullptr && Table->GetRowStruct() != nullptr)
		{
			for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
			{
				Rows.Add(Pair.Key, ReadRow(Table->GetRowStruct(), Pair.Value, Members));
			}
		}
		return Rows;
	}

	/** Registers redirects for the lifetime of the scope. */
	struct FRedirectScope
	{
		TArray<FCoreRedirect> Redirects;
		bool bAdded = false;

		explicit FRedirectScope(TArray<FCoreRedirect> InRedirects)
			: Redirects(MoveTemp(InRedirects))
		{
			bAdded = FCoreRedirects::AddRedirectList(Redirects, TEXT("Issue122DataTest"));
		}

		/** Takes over redirects something else registered (the ini reader): they are removed with the scope. */
		FRedirectScope(TArray<FCoreRedirect> InRedirects, const bool bAlreadyRegistered)
			: Redirects(MoveTemp(InRedirects))
			, bAdded(bAlreadyRegistered)
		{
		}

		FRedirectScope(const FRedirectScope&) = delete;
		FRedirectScope& operator=(const FRedirectScope&) = delete;

		~FRedirectScope()
		{
			if (!bAdded)
			{
				return;
			}
			// The engine refuses to remove a redirect that carries value changes, so removal matches on names only; adding a value
			// redirect also registers a NewName -> NewName twin that has to go as well.
			TArray<FCoreRedirect> Removals;
			for (const FCoreRedirect& Redirect : Redirects)
			{
				FCoreRedirect Plain = Redirect;
				Plain.ValueChanges.Empty();
				Removals.Add(Plain);
				if (Redirect.HasValueChanges())
				{
					Plain.OldName = Plain.NewName;
					Removals.Add(Plain);
				}
			}
			FCoreRedirects::RemoveRedirectList(Removals, TEXT("Issue122DataTest"));
		}
	};

	inline FCoreRedirect MakeRedirect(const ECoreRedirectFlags Flags, const FString& OldName, const FString& NewName, const FString& OverrideClassName = FString())
	{
		FCoreRedirect Redirect(Flags, FCoreRedirectObjectName(OldName), FCoreRedirectObjectName(NewName));
		if (!OverrideClassName.IsEmpty())
		{
			Redirect.OverrideClassName = FCoreRedirectObjectName(OverrideClassName);
		}
		return Redirect;
	}

	inline FCoreRedirect MakeEnumRedirect(const FString& OldName, const FString& NewName, const TMap<FString, FString>& ValueChanges)
	{
		FCoreRedirect Redirect = MakeRedirect(ECoreRedirectFlags::Type_Enum, OldName, NewName, TEXT("/Script/CoreUObject.Enum"));
		Redirect.ValueChanges = ValueChanges;
		return Redirect;
	}

	/** Saved assets under /Game/Temp/Issue122_<8 hex>; everything is retired when the fixture goes out of scope. */
	struct FFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Issue122_%s"), *Suffix);
		TArray<FString> SavedFiles;
		TArray<FString> ScratchPaths;
		bool bCleaned = false;

		FFixture() = default;
		FFixture(const FFixture&) = delete;
		FFixture& operator=(const FFixture&) = delete;

		~FFixture()
		{
			Cleanup();
		}

		FString AssetPath(const FString& Name) const
		{
			return FString::Printf(TEXT("%s/%s.%s"), *Dir, *Name, *Name);
		}

		/** A project-relative path under Saved/ that Cleanup deletes (file or directory). */
		FString ScratchPath(const FString& Relative)
		{
			const FString Path = FPaths::Combine(TEXT("Saved/Issue122Data"), Suffix, Relative);
			ScratchPaths.AddUnique(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path));
			return Path;
		}

		static FString Absolute(const FString& ProjectRelative)
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ProjectRelative);
		}

		bool SaveAsset(UObject* Asset, const TCHAR* Extension = nullptr)
		{
			UPackage* Package = Asset->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, Extension ? Extension : *FPackageName::GetAssetPackageExtension()))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
			{
				return false;
			}
			SavedFiles.AddUnique(Filename);
			return true;
		}

		/** Makes every saved file visible to the registry (tags and dependencies) before a command queries it. */
		void RegisterSavedFiles() const
		{
			IAssetRegistry::GetChecked().ScanFilesSynchronous(SavedFiles, true);
		}

		UUserDefinedStruct* MakeStruct(const FString& Name, const TArray<TPair<FString, FEdGraphPinType>>& Members)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
			if (Struct == nullptr || Members.Num() == 0)
			{
				return Struct;
			}
			// The engine creates the struct with one default member; it becomes the first requested member.
			const FGuid FirstGuid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
			FStructureEditorUtils::ChangeVariableType(Struct, FirstGuid, Members[0].Value);
			FStructureEditorUtils::RenameVariable(Struct, FirstGuid, Members[0].Key);
			for (int32 Index = 1; Index < Members.Num(); ++Index)
			{
				FStructureEditorUtils::AddVariable(Struct, Members[Index].Value);
				const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
				FStructureEditorUtils::RenameVariable(Struct, Guid, Members[Index].Key);
			}
			return Struct;
		}

		/** The usual row struct: Count (int), Label (string), Flag (bool). */
		UUserDefinedStruct* MakeRowStruct(const FString& Name)
		{
			return MakeStruct(Name, { { TEXT("Count"), IntPin() }, { TEXT("Label"), StringPin() }, { TEXT("Flag"), BoolPin() } });
		}

		UUserDefinedEnum* MakeEnum(const FString& Name, const TArray<FString>& DisplayNames)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional));
			if (Enum == nullptr)
			{
				return nullptr;
			}
			for (int32 Index = 0; Index < DisplayNames.Num(); ++Index)
			{
				FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
				FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(DisplayNames[Index]));
			}
			return Enum;
		}

		/** A data asset of UCortexIssue122StructDataAsset whose Struct hard-references Struct. */
		UObject* MakeDataAssetReferencing(const FString& Name, UScriptStruct* Struct)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UCortexIssue122StructDataAsset* Asset = NewObject<UCortexIssue122StructDataAsset>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
			if (Asset != nullptr)
			{
				Asset->Struct = Struct;
			}
			return Asset;
		}

		/** A map whose only actor hard-references Struct. */
		UWorld* MakeWorldReferencing(const FString& Name, UScriptStruct* Struct)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*Name), Package);
			if (World == nullptr)
			{
				return nullptr;
			}
			World->SetFlags(RF_Public | RF_Standalone);
			if (ACortexIssue122StructActor* Actor = World->SpawnActor<ACortexIssue122StructActor>())
			{
				Actor->Struct = Struct;
				return World;
			}
			return nullptr;
		}

		UDataTable* MakeTable(const FString& Name, UScriptStruct* RowStruct)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UDataTable* Table = NewObject<UDataTable>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
			if (Table != nullptr)
			{
				Table->RowStruct = RowStruct;
			}
			return Table;
		}

		static bool AddRow(UDataTable* Table, const FName RowName, const FRowMembers& Members, const FRowValues& Values)
		{
			FStructOnScope Row(Table->RowStruct);
			if (!SetMember(Table->RowStruct, Row.GetStructMemory(), Members.Count, Values.Count)
				|| !SetMember(Table->RowStruct, Row.GetStructMemory(), Members.Label, Values.Label)
				|| !SetMember(Table->RowStruct, Row.GetStructMemory(), Members.Flag, Values.Flag))
			{
				return false;
			}
			Table->AddRow(RowName, Row.GetStructMemory(), Table->RowStruct);
			return true;
		}

		/** A Blueprint (compiled by the engine's CreateBlueprint) with one variable of the given type and default text. */
		UBlueprint* MakeBlueprintWithVariable(const FString& Name, const FName VariableName, const FEdGraphPinType& Type, const FString& Default)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
			if (Blueprint != nullptr)
			{
				FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, Type, Default);
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
			}
			return Blueprint;
		}

		/** Unloads the packages of Assets (which must be saved and clean) so the next load goes through the linker again. */
		static bool Unload(const TArray<UObject*>& Assets)
		{
			TArray<UPackage*> Packages;
			for (UObject* Asset : Assets)
			{
				Packages.AddUnique(Asset->GetOutermost());
			}
			ResetUndoHistory();
			FText Error;
			return UPackageTools::UnloadPackages(Packages, Error, true);
		}

		template <typename T>
		static T* Reload(const FString& ObjectPath)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (FindPackage(nullptr, *PackageName) == nullptr && !FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}
			return LoadObject<T>(nullptr, *ObjectPath);
		}

		void Cleanup()
		{
			if (bCleaned)
			{
				return;
			}
			bCleaned = true;

			// Pending scans of the saved files must land before the assets are retired.
			IAssetRegistry::GetChecked().WaitForCompletion();
			ResetUndoHistory();

			TArray<UWorld*> Worlds;
			for (TObjectIterator<UWorld> It; It; ++It)
			{
				if (It->GetOutermost()->GetName().StartsWith(Dir))
				{
					Worlds.Add(*It);
				}
			}
			for (UWorld* World : Worlds)
			{
				World->DestroyWorld(false);
			}

			TArray<UObject*> Retired;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Object = *It;
				if (Object && Object->IsAsset() && Object->GetOutermost()->GetName().StartsWith(Dir))
				{
					Retired.Add(Object);
				}
			}
			for (UObject* Asset : Retired)
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
				{
					if (Blueprint->GeneratedClass)
					{
						Blueprint->GeneratedClass->MarkAsGarbage();
					}
					if (Blueprint->SkeletonGeneratedClass)
					{
						Blueprint->SkeletonGeneratedClass->MarkAsGarbage();
					}
				}
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags);

			for (const FString& File : SavedFiles)
			{
				for (int32 Attempt = 0; Attempt < 50 && IFileManager::Get().FileExists(*File); ++Attempt)
				{
					if (IFileManager::Get().Delete(*File, false, true, true))
					{
						break;
					}
					FlushAsyncLoading();
					FPlatformProcess::Sleep(0.1f);
				}
			}
			for (const FString& Scratch : ScratchPaths)
			{
				IFileManager::Get().Delete(*Scratch, false, true, true);
				IFileManager::Get().DeleteDirectory(*Scratch, false, true);
			}
			IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Issue122Data"), Suffix), false, true);
			IFileManager::Get().DeleteDirectory(
				*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), FString::Printf(TEXT("Issue122_%s"), *Suffix)), false, true);
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
		}
	};

	// --- plan/verify request helpers -------------------------------------------------------------------------------------------
	inline TSharedPtr<FJsonObject> PlanParams(const FString& UserType, const FString& NativeType)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("user_type"), UserType);
		Params->SetStringField(TEXT("native_type"), NativeType);
		return Params;
	}

	inline void SetStringMap(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const TMap<FString, FString>& Map)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Map)
		{
			Object->SetStringField(Pair.Key, Pair.Value);
		}
		Params->SetObjectField(Field, Object);
	}

	/** The GUID-suffixed property name of the member with this friendly name. */
	inline FString UserName(const UUserDefinedStruct* Struct, const FString& Friendly)
	{
		for (const FStructVariableDescription& Member : FStructureEditorUtils::GetVarDesc(Struct))
		{
			if (Member.FriendlyName == Friendly)
			{
				return Member.VarName.ToString();
			}
		}
		return FString();
	}

	inline bool ProblemCodes(const FCortexCommandResult& Result, TArray<FString>& OutCodes, TArray<FString>& OutSubjects)
	{
		if (!Result.ErrorDetails.IsValid())
		{
			return false;
		}
		for (const TSharedPtr<FJsonObject>& Problem : Objects(Result.ErrorDetails, TEXT("problems")))
		{
			OutCodes.Add(Problem->GetStringField(TEXT("code")));
			OutSubjects.Add(Problem->GetStringField(TEXT("subject")));
		}
		return true;
	}

	inline bool HasProblem(const TArray<FString>& Codes, const TArray<FString>& Subjects, const FString& Code, const FString& Subject)
	{
		for (int32 Index = 0; Index < Codes.Num(); ++Index)
		{
			if (Codes[Index] == Code && Subjects[Index] == Subject)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Registers the lines through the engine's own ini machinery: a config file parser turns them into a [CoreRedirects] section,
	 * which goes into the engine ini branch in memory (never written to disk) and is consumed by FCoreRedirects::ReadRedirectsFromIni,
	 * exactly as at editor start. Returns whether the reader added any redirect.
	 */
	inline bool RegisterLinesThroughIni(const TArray<FString>& Lines)
	{
		FString Text = TEXT("[CoreRedirects]\n");
		for (const FString& Line : Lines)
		{
			Text += Line + TEXT("\n");
		}
		FConfigFile File;
		File.CombineFromBuffer(Text, TEXT("Issue122PlanIni"));
		const FConfigSection* Parsed = File.FindSection(TEXT("CoreRedirects"));
		if (Parsed == nullptr)
		{
			return false;
		}
		for (FConfigSection::TConstIterator It(*Parsed); It; ++It)
		{
			GConfig->AddToSection(TEXT("CoreRedirects"), It.Key(), It.Value().GetValue(), GEngineIni);
		}
		const bool bAdded = FCoreRedirects::ReadRedirectsFromIni(GEngineIni);
		GConfig->RemoveSectionFromBranch(TEXT("CoreRedirects"), *GEngineIni);
		return bAdded;
	}

	inline FString ConfigFileHash(const FString& Relative)
	{
		FCortexResolvedFilePath Path;
		FString Code;
		FString Message;
		FString Hash;
		if (FCortexSafeFileContract::ResolveReadPath(Relative, Path, Code, Message))
		{
			FCortexSafeFileContract::HashFileBytesSha256(Path, Hash, Code, Message);
		}
		return Hash;
	}

	/** MD5 of a file's bytes (an absolute path): the proof that a file was or was not rewritten. */
	inline FString FileBytesHash(const FString& AbsolutePath)
	{
		TArray<uint8> Bytes;
		return FFileHelper::LoadFileToArray(Bytes, *AbsolutePath) ? FMD5::HashBytes(Bytes.GetData(), Bytes.Num()) : FString();
	}

	inline FString ReadFile(const FString& ProjectRelative)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FFixture::Absolute(ProjectRelative));
		return Text;
	}

	/** The five redirects a plan for a UDS (Object with OverrideClassName, Struct, one Property per member) registers. */
	inline TArray<FCoreRedirect> MakeStructRedirects(const FString& UserPath, const FString& NativePath, const TArray<TPair<FString, FString>>& GuidToNativeMember)
	{
		TArray<FCoreRedirect> Redirects;
		Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Object, UserPath, NativePath, TEXT("/Script/CoreUObject.ScriptStruct")));
		Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Struct, UserPath, NativePath));
		for (const TPair<FString, FString>& Member : GuidToNativeMember)
		{
			Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Property, NativePath + TEXT(".") + Member.Key, Member.Value));
		}
		return Redirects;
	}
}
