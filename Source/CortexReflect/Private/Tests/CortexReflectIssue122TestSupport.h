#pragma once

// Shared fixtures for the #122 reflect discovery tests (find_usages / find_overrides interface fixes,
// find_interface_implementers, find_type_usages). Assets are real saved packages under /Game/Temp/Issue122_<8 hex> so the
// Asset Registry sees real tags and dependencies, and a test can unload them to exercise the not-loaded paths.
//
// GC-safety invariant: the editor undo history is emptied BEFORE any fixture object is retired, because transaction records
// name fixture classes/graphs and a later collection would scan freed objects (see CortexAdoptTestHelpers.h).

#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "CortexReflectCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Message.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace CortexIssue122ReflectTest
{
	inline void ResetUndoHistory()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue 122 test fixture")));
		}
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
		FCortexReflectCommandHandler Handler;
		return Handler.Execute(Command, Params);
	}

	/** Saved assets under /Game/Temp/Issue122_<8 hex>; everything is retired when the fixture goes out of scope. */
	struct FFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Issue122_%s"), *Suffix);
		TArray<FString> SavedFiles;
		TArray<FString> ReportFiles;
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

		bool SaveAsset(UObject* Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
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

		/** Creates (and by the engine's own CreateBlueprint, compiles) a Blueprint package under the fixture folder. */
		UBlueprint* MakeBlueprint(
			const FString& Name,
			UClass* ParentClass,
			const EBlueprintType Type = BPTYPE_Normal,
			UClass* BlueprintClass = nullptr,
			UClass* GeneratedClass = nullptr)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			return FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*Name),
				Type,
				BlueprintClass ? BlueprintClass : UBlueprint::StaticClass(),
				GeneratedClass ? GeneratedClass : UBlueprintGeneratedClass::StaticClass(),
				NAME_None);
		}

		UBlueprint* MakeWidgetBlueprint(const FString& Name)
		{
			UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
			UClass* WidgetGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetBlueprintGeneratedClass"));
			if (!WidgetBlueprintClass || !WidgetGeneratedClass)
			{
				return nullptr;
			}
			UBlueprint* Blueprint = MakeBlueprint(Name, UUserWidget::StaticClass(), BPTYPE_Normal, WidgetBlueprintClass, WidgetGeneratedClass);
			if (Blueprint && !GetWidgetTree(Blueprint))
			{
				const FObjectProperty* Property = CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
				if (Property)
				{
					Property->SetObjectPropertyValue_InContainer(
						Blueprint, NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional));
				}
			}
			return Blueprint;
		}

		static UWidgetTree* GetWidgetTree(UBlueprint* WidgetBlueprint)
		{
			const FObjectProperty* Property = CastField<FObjectProperty>(WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
			return Property ? Cast<UWidgetTree>(Property->GetObjectPropertyValue_InContainer(WidgetBlueprint)) : nullptr;
		}

		static UEdGraph* EventGraph(UBlueprint* Blueprint)
		{
			return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
		}

		/** A user function graph; with bWithOutput a function result node carries one double output named Value. */
		static UEdGraph* AddFunctionGraphTo(UBlueprint* Blueprint, const FName Name, const bool bWithOutput)
		{
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!Graph)
			{
				return nullptr;
			}
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
			if (bWithOutput)
			{
				UK2Node_FunctionEntry* Entry = nullptr;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Entry)
					{
						Entry = Cast<UK2Node_FunctionEntry>(Node);
					}
				}
				if (!Entry)
				{
					return nullptr;
				}
				FEdGraphPinType DoubleType;
				DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
				DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
				UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
				Result->FunctionReference = Entry->FunctionReference;
				Result->CreateNewGuid();
				Result->AllocateDefaultPins();
				Graph->AddNode(Result, true, false);
				Result->CreateUserDefinedPin(TEXT("Value"), DoubleType, EGPD_Input, false);
			}
			return Graph;
		}

		static UK2Node_Message* AddMessage(UEdGraph* Graph, const FName FunctionName, UClass* InterfaceClass)
		{
			FGraphNodeCreator<UK2Node_Message> Creator(*Graph);
			UK2Node_Message* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(FunctionName, InterfaceClass);
			Creator.Finalize();
			return Node;
		}

		static UK2Node_CallFunction* AddTypedCall(UEdGraph* Graph, const FName FunctionName, UClass* OwnerClass)
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(FunctionName, OwnerClass);
			Creator.Finalize();
			return Node;
		}

		static UK2Node_DynamicCast* AddCast(UEdGraph* Graph, UClass* Target)
		{
			FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
			UK2Node_DynamicCast* Node = Creator.CreateNode();
			Node->TargetType = Target;
			Creator.Finalize();
			return Node;
		}

		/** An interface event node (override of the interface function); bWithBody links its exec output to a sequence node. */
		static UK2Node_Event* AddInterfaceEvent(UEdGraph* Graph, const FName EventName, UClass* InterfaceClass, const bool bWithBody)
		{
			UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
			Event->EventReference.SetExternalMember(EventName, InterfaceClass);
			Event->bOverrideFunction = true;
			Event->CreateNewGuid();
			Event->AllocateDefaultPins();
			Graph->AddNode(Event, true, false);
			if (bWithBody)
			{
				UK2Node_ExecutionSequence* Body = NewObject<UK2Node_ExecutionSequence>(Graph);
				Body->CreateNewGuid();
				Body->AllocateDefaultPins();
				Graph->AddNode(Body, true, false);
				UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				UEdGraphPin* Execute = Body->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
				if (Then && Execute)
				{
					GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Then, Execute);
				}
			}
			return Event;
		}

		/** Unloads the packages of Assets (which must be saved and clean) so a command meets them as not loaded. */
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

		FString ReportPath(const FString& Label)
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("Issue122"), FString::Printf(TEXT("%s_%s.json"), *Label, *Suffix)));
			ReportFiles.Add(Path);
			return Path;
		}

		void Cleanup()
		{
			if (bCleaned)
			{
				return;
			}
			bCleaned = true;

			for (const FString& File : ReportFiles)
			{
				IFileManager::Get().Delete(*File);
			}

			// Pending scans of the saved files must land before the assets are retired.
			IAssetRegistry::GetChecked().WaitForCompletion();
			ResetUndoHistory();

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
			IFileManager::Get().DeleteDirectory(
				*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), FString::Printf(TEXT("Issue122_%s"), *Suffix)), false, true);
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
		}
	};
}
