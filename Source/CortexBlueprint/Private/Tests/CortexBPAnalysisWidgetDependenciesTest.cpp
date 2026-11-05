#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	UWidgetTree* GetWidgetTree(UBlueprint* WidgetBP)
	{
		const FObjectProperty* Prop = CastField<FObjectProperty>(WidgetBP->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		return Prop ? Cast<UWidgetTree>(Prop->GetObjectPropertyValue_InContainer(WidgetBP)) : nullptr;
	}

	bool SaveAndRegister(UBlueprint* WidgetBP, const FString& PackageName)
	{
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(WidgetBP->GetOutermost(), WidgetBP, *Filename, SaveArgs))
		{
			return false;
		}
		IAssetRegistry::GetChecked().ScanModifiedAssetFiles({Filename});
		return true;
	}

	TArray<FString> StringArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsString());
			}
		}
		return Out;
	}

	void RetireAsset(const FString& PackageName, const FString& ObjectPath)
	{
		if (UPackage* Pkg = FindPackage(nullptr, *PackageName))
		{
			ResetLoaders(Pkg);
			if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
			{
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			Pkg->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisWidgetDependenciesTest,
	"Cortex.Blueprint.Analysis.WidgetDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisWidgetDependenciesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString Dir = TEXT("/Game/Temp/Cortex119");
	const FString ChildName = FString::Printf(TEXT("WBP_DepChild_%s"), *Suffix);
	const FString ParentName = FString::Printf(TEXT("WBP_DepParent_%s"), *Suffix);
	const FString ChildPackage = FString::Printf(TEXT("%s/%s"), *Dir, *ChildName);
	const FString ParentPackage = FString::Printf(TEXT("%s/%s"), *Dir, *ParentName);
	const FString ChildObject = FString::Printf(TEXT("%s.%s"), *ChildPackage, *ChildName);
	const FString ParentObject = FString::Printf(TEXT("%s.%s"), *ParentPackage, *ParentName);

	auto Cleanup = [&]()
	{
		RetireAsset(ParentPackage, ParentObject);
		RetireAsset(ChildPackage, ChildObject);
		GEditor->ResetTransaction(FText::FromString(TEXT("Cortex119 dependencies test cleanup")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		for (const FString& Package : {ParentPackage, ChildPackage})
		{
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(Package, Filename, FPackageName::GetAssetPackageExtension()))
			{
				IFileManager::Get().Delete(*Filename);
			}
		}
	};

	for (const FString& Name : {ChildName, ParentName})
	{
		TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
		Create->SetStringField(TEXT("name"), Name);
		Create->SetStringField(TEXT("path"), Dir);
		Create->SetStringField(TEXT("type"), TEXT("Widget"));
		if (!TestTrue(*FString::Printf(TEXT("create %s"), *Name), Handler.Execute(TEXT("create"), Create).bSuccess))
		{
			Cleanup();
			return false;
		}
	}

	UBlueprint* ChildBP = LoadObject<UBlueprint>(nullptr, *ChildObject);
	UBlueprint* ParentBP = LoadObject<UBlueprint>(nullptr, *ParentObject);
	UWidgetTree* ParentTree = ParentBP ? GetWidgetTree(ParentBP) : nullptr;
	if (!TestNotNull(TEXT("child loads"), ChildBP) || !TestNotNull(TEXT("parent tree"), ParentTree))
	{
		Cleanup();
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ChildBP);
	UClass* ChildClass = ChildBP->GeneratedClass;
	if (!TestNotNull(TEXT("child generated class"), ChildClass))
	{
		Cleanup();
		return false;
	}

	UCanvasPanel* Root = ParentTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	ParentTree->RootWidget = Root;
	UUserWidget* Placed = ParentTree->ConstructWidget<UUserWidget>(ChildClass, TEXT("PlacedChild"));
	Root->AddChild(Placed);
	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	if (!TestTrue(TEXT("save child"), SaveAndRegister(ChildBP, ChildPackage))
		|| !TestTrue(TEXT("save parent"), SaveAndRegister(ParentBP, ParentPackage)))
	{
		Cleanup();
		return false;
	}

	const auto Analyze = [&Handler](const FString& Package)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Package);
		return Handler.Execute(TEXT("analyze_for_migration"), Params);
	};
	const auto Dependencies = [](const FCortexCommandResult& Result) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetObjectField(TEXT("widget_dependencies"), Object) ? *Object : nullptr;
	};

	const FCortexCommandResult ParentResult = Analyze(ParentPackage);
	const FCortexCommandResult ChildResult = Analyze(ChildPackage);
	if (TestTrue(TEXT("parent analysis"), ParentResult.bSuccess))
	{
		const TArray<FString> References = StringArrayField(Dependencies(ParentResult), TEXT("references_widgets"));
		TestTrue(TEXT("parent references_widgets lists the placed user widget class"),
			References.Contains(ChildClass->GetPathName()));
		TestTrue(TEXT("references_widgets is sorted"), References == [&References]() { TArray<FString> S = References; S.Sort(); return S; }());
	}
	if (TestTrue(TEXT("child analysis"), ChildResult.bSuccess))
	{
		const TArray<FString> ReferencedBy = StringArrayField(Dependencies(ChildResult), TEXT("referenced_by_widgets"));
		TestTrue(TEXT("child referenced_by_widgets lists the parent widget blueprint"),
			ReferencedBy.Contains(ParentObject));
	}

	Cleanup();
	return true;
}
