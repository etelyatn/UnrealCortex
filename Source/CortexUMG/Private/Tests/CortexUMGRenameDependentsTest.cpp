// umg.rename_widget scope:"dependents": the rename reaches unloaded child Blueprints and referencing
// Blueprints through the Asset Registry, is planned with dry_run, guarded by exact per-dependent
// fingerprints, undone by one transaction, and honours inherited BindWidget names.

#include "Curves/CurveFloat.h"
#include "Engine/TimelineTemplate.h"
#include "CortexAssetFingerprint.h"
#include "CortexAssetMutationGuard.h"
#include "CortexBatchMutation.h"
#include "CortexBatchScope.h"
#include "Operations/CortexUMGRenameDependents.h"
#include "ScopedTransaction.h"
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "CortexUMGRenameDependentsTestTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
	const FName SharedButtonName(TEXT("SharedButton"));

	UBlueprint* NewSavedFixtureBlueprint(UClass* Parent, UClass* BlueprintClass, UClass* GeneratedClassType, const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		return FKismetEditorUtilities::CreateBlueprint(Parent, Package,
			FName(*FPackageName::GetLongPackageAssetName(PackageName)), BPTYPE_Normal,
			BlueprintClass, GeneratedClassType, NAME_None);
	}

	UWidgetBlueprint* NewFixtureWidgetBlueprint(UClass* Parent, const FString& PackageName)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(NewSavedFixtureBlueprint(Parent,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), PackageName));
		if (Blueprint != nullptr && Blueprint->WidgetTree != nullptr)
		{
			Blueprint->SetFlags(RF_Transactional);
			Blueprint->WidgetTree->RootWidget =
				Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		}
		return Blueprint;
	}

	UButton* AddButton(UWidgetBlueprint* Blueprint, const TCHAR* Name)
	{
		UButton* Button = Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
		Button->bIsVariable = true;
		Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget)->AddChild(Button);
		return Button;
	}

	FString FilenameOf(const UPackage* Package)
	{
		FString Filename;
		FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension());
		return Filename;
	}

	bool SaveFixture(UBlueprint* Blueprint)
	{
		UPackage* Package = Blueprint->GetPackage();
		const FString Filename = FilenameOf(Package);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (Filename.IsEmpty() || !UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))
		{
			return false;
		}
		FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({ Filename }, true);
		return true;
	}

	UEdGraph* EventGraphOf(UBlueprint* Blueprint)
	{
		return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	}

	UK2Node_VariableGet* AddGetter(UBlueprint* Blueprint, const FName MemberName, UClass* ExternalClass)
	{
		UEdGraph* Graph = EventGraphOf(Blueprint);
		UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph, NAME_None, RF_Transactional);
		if (ExternalClass != nullptr)
		{
			Getter->VariableReference.SetExternalMember(MemberName, ExternalClass);
		}
		else
		{
			Getter->VariableReference.SetSelfMember(MemberName);
		}
		Getter->CreateNewGuid();
		Graph->AddNode(Getter, false, false);
		Getter->AllocateDefaultPins();
		return Getter;
	}

	UK2Node_ComponentBoundEvent* AddBoundOnClicked(UBlueprint* Blueprint, UClass* OwnerClass)
	{
		UEdGraph* Graph = EventGraphOf(Blueprint);
		const FObjectProperty* ButtonProperty = FindFProperty<FObjectProperty>(OwnerClass, SharedButtonName);
		const FMulticastDelegateProperty* OnClicked =
			FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
		if (ButtonProperty == nullptr || OnClicked == nullptr)
		{
			return nullptr;
		}
		UK2Node_ComponentBoundEvent* Bound = NewObject<UK2Node_ComponentBoundEvent>(Graph, NAME_None, RF_Transactional);
		Bound->InitializeComponentBoundEventParams(ButtonProperty, OnClicked);
		Bound->CreateNewGuid();
		Graph->AddNode(Bound, false, false);
		Bound->AllocateDefaultPins();
		return Bound;
	}

	int32 CountVariableNodes(const UBlueprint* Blueprint, const FName MemberName)
	{
		int32 Count = 0;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node);
				Count += (Variable != nullptr && Variable->GetVarName() == MemberName) ? 1 : 0;
			}
		}
		return Count;
	}

	int32 CountBoundEvents(const UBlueprint* Blueprint, const FName ComponentName)
	{
		int32 Count = 0;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node);
				Count += (Bound != nullptr && Bound->ComponentPropertyName == ComponentName) ? 1 : 0;
			}
		}
		return Count;
	}

	/** Forgets an in-memory fixture asset so the next load must come from disk. */
	void Unload(UBlueprint* Blueprint)
	{
		UPackage* Package = Blueprint->GetPackage();
		ResetLoaders(Package);
		Package->ClearDirtyFlag();
		Blueprint->ClearFlags(RF_Standalone | RF_Public);
		Blueprint->MarkAsGarbage();
		Package->MarkAsGarbage();
	}

	bool IsLoaded(const FString& PackageName)
	{
		return FindPackage(nullptr, *PackageName) != nullptr;
	}

	bool IsPackageDirty(const FString& PackageName)
	{
		const UPackage* Package = FindPackage(nullptr, *PackageName);
		return Package != nullptr && Package->IsDirty();
	}

	/** Deletes the fixture files and forgets whatever is still loaded. */
	struct FRenameFixtureScope
	{
		TArray<FString> PackageNames;

		FString Add(const FString& Prefix, const TCHAR* Asset)
		{
			const FString Name = FString::Printf(TEXT("/Game/Temp/CortexRenameDependents/%s_%s"), *Prefix, Asset);
			PackageNames.Add(Name);
			return Name;
		}

		~FRenameFixtureScope()
		{
			for (const FString& Name : PackageNames)
			{
				if (UPackage* Package = FindPackage(nullptr, *Name))
				{
					ResetLoaders(Package);
					Package->ClearDirtyFlag();
					if (UBlueprint* Blueprint = FindObject<UBlueprint>(Package, *FPackageName::GetLongPackageAssetName(Name)))
					{
						Blueprint->ClearFlags(RF_Standalone | RF_Public);
						Blueprint->MarkAsGarbage();
					}
					Package->MarkAsGarbage();
				}
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			for (const FString& Name : PackageNames)
			{
				FString Filename;
				if (FPackageName::TryConvertLongPackageNameToFilename(Name, Filename, FPackageName::GetAssetPackageExtension()))
				{
					IFileManager::Get().Delete(*Filename, false, true, true);
				}
			}
		}
	};

	TSharedPtr<FJsonObject> FindPlanAsset(const FCortexCommandResult& Result, const FString& AssetPath)
	{
		const TSharedPtr<FJsonObject>* Plan = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetObjectField(TEXT("plan"), Plan))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : (*Plan)->GetArrayField(TEXT("assets")))
		{
			const TSharedPtr<FJsonObject> Asset = Value->AsObject();
			if (Asset.IsValid() && Asset->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Asset;
			}
		}
		return nullptr;
	}

	int32 PlanReferenceCount(const TSharedPtr<FJsonObject>& Asset)
	{
		return Asset.IsValid() ? Asset->GetArrayField(TEXT("references")).Num() : -1;
	}

	TSharedPtr<FJsonObject> CopyWithField(const TSharedPtr<FJsonObject>& Source, const TCHAR* Field, const FString& Value)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			Copy->SetField(Pair.Key, Pair.Value);
		}
		Copy->SetStringField(Field, Value);
		return Copy;
	}

	struct FRenameRouter
	{
		FCortexCommandRouter Router;

		FRenameRouter()
		{
			Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"), MakeShared<FCortexUMGCommandHandler>());
		}

		TSharedPtr<FJsonObject> TreeFingerprint(const FString& AssetPath)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			const FCortexCommandResult Tree = Router.Execute(TEXT("umg.get_tree"), Params);
			const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
			return Tree.bSuccess && Tree.Data.IsValid() && Tree.Data->TryGetObjectField(TEXT("fingerprint"), Fingerprint)
				? *Fingerprint : nullptr;
		}

		TSharedPtr<FJsonObject> RenameParams(const FString& AssetPath, const TCHAR* WidgetName, const TCHAR* NewName)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetStringField(TEXT("widget_name"), WidgetName);
			Params->SetStringField(TEXT("new_name"), NewName);
			Params->SetObjectField(TEXT("expected_fingerprint"), TreeFingerprint(AssetPath));
			return Params;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsTest,
	"Cortex.UMG.RenameWidgetDependents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr))
	{
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FRenameFixtureScope Scope;
	const FString OwnerPackage = Scope.Add(Suffix, TEXT("Owner"));
	const FString ChildPackage = Scope.Add(Suffix, TEXT("Child"));
	const FString ReferencerPackage = Scope.Add(Suffix, TEXT("Referencer"));
	const FString NoRefPackage = Scope.Add(Suffix, TEXT("NoRef"));
	const FString UnrelatedPackage = Scope.Add(Suffix, TEXT("Unrelated"));

	TStrongObjectPtr<UWidgetBlueprint> Owner(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), OwnerPackage));
	if (!TestNotNull(TEXT("owner created"), Owner.Get())) return false;
	AddButton(Owner.Get(), TEXT("SharedButton"));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	UClass* OwnerClass = Owner->GeneratedClass;
	if (!TestNotNull(TEXT("owner exposes the SharedButton member"), FindFProperty<FObjectProperty>(OwnerClass, SharedButtonName))) return false;
	const FString OwnerPath = Owner->GetPathName();

	FString ChildPath;
	FString ReferencerPath;
	FString NoRefPath;
	FString UnrelatedPath;
	{
		UWidgetBlueprint* Child = NewFixtureWidgetBlueprint(OwnerClass, ChildPackage);
		if (!TestNotNull(TEXT("child created"), Child)) return false;
		UK2Node_VariableGet* ChildGetter = AddGetter(Child, SharedButtonName, nullptr);
		UK2Node_ComponentBoundEvent* ChildBound = AddBoundOnClicked(Child, OwnerClass);
		if (!TestTrue(TEXT("child graph nodes created"), ChildGetter != nullptr && ChildBound != nullptr)) return false;
		ChildPath = Child->GetPathName();

		UBlueprint* Referencer = NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), ReferencerPackage);
		if (!TestNotNull(TEXT("referencer created"), Referencer)) return false;
		AddGetter(Referencer, SharedButtonName, OwnerClass);
		ReferencerPath = Referencer->GetPathName();

		// Holds the owner type but never reads the member: reported, not modified.
		UBlueprint* NoRef = NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NoRefPackage);
		if (!TestNotNull(TEXT("no-reference referencer created"), NoRef)) return false;
		FEdGraphPinType OwnerType;
		OwnerType.PinCategory = UEdGraphSchema_K2::PC_Object;
		OwnerType.PinSubCategoryObject = OwnerClass;
		FBlueprintEditorUtils::AddMemberVariable(NoRef, TEXT("OwnerInstance"), OwnerType);
		NoRefPath = NoRef->GetPathName();

		UBlueprint* Unrelated = NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), UnrelatedPackage);
		if (!TestNotNull(TEXT("unrelated created"), Unrelated)) return false;
		AddGetter(Unrelated, SharedButtonName, nullptr);
		UnrelatedPath = Unrelated->GetPathName();

		const bool bSaved = SaveFixture(Owner.Get()) && SaveFixture(Child) && SaveFixture(Referencer)
			&& SaveFixture(NoRef) && SaveFixture(Unrelated);
		if (!TestTrue(TEXT("fixtures saved"), bSaved)) return false;

		// Only the owner stays loaded: children and referencers must be discovered and loaded by the command.
		Unload(Child);
		Unload(Referencer);
		Unload(NoRef);
		Unload(Unrelated);
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("child is unloaded before the call"), IsLoaded(ChildPackage));
	TestFalse(TEXT("referencer is unloaded before the call"), IsLoaded(ReferencerPackage));

	FRenameRouter Harness;
	const TSharedPtr<FJsonObject> OwnerFingerprint = Harness.TreeFingerprint(OwnerPath);
	if (!TestTrue(TEXT("owner tree fingerprint"), OwnerFingerprint.IsValid())) return false;
	TestFalse(TEXT("owner starts clean"), Owner->GetPackage()->IsDirty());

	// Dry run is the default and publishes the plan without dirtying anything.
	TSharedPtr<FJsonObject> Params = Harness.RenameParams(OwnerPath, TEXT("SharedButton"), TEXT("RenamedButton"));
	Params->SetStringField(TEXT("scope"), TEXT("dependents"));
	const FCortexCommandResult Dry = Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
	if (!TestTrue(TEXT("dependents dry run succeeds by default"), Dry.bSuccess && Dry.Data.IsValid())) return false;
	TestEqual(TEXT("dry run echoes the scope"), Dry.Data->GetStringField(TEXT("scope")), FString(TEXT("dependents")));
	TestTrue(TEXT("dry run is flagged"), Dry.Data->GetBoolField(TEXT("dry_run")));
	TestFalse(TEXT("dry run changes nothing"), Dry.Data->GetBoolField(TEXT("changed")));
	const TSharedPtr<FJsonObject> PlanOwner = FindPlanAsset(Dry, OwnerPath);
	const TSharedPtr<FJsonObject> PlanChild = FindPlanAsset(Dry, ChildPath);
	const TSharedPtr<FJsonObject> PlanReferencer = FindPlanAsset(Dry, ReferencerPath);
	const TSharedPtr<FJsonObject> PlanNoRef = FindPlanAsset(Dry, NoRefPath);
	if (!TestTrue(TEXT("plan lists owner, unloaded child, referencer and the no-reference referencer"),
		PlanOwner.IsValid() && PlanChild.IsValid() && PlanReferencer.IsValid() && PlanNoRef.IsValid())) return false;
	TestEqual(TEXT("owner role"), PlanOwner->GetStringField(TEXT("role")), FString(TEXT("owner")));
	TestEqual(TEXT("child role"), PlanChild->GetStringField(TEXT("role")), FString(TEXT("child")));
	TestEqual(TEXT("referencer role"), PlanReferencer->GetStringField(TEXT("role")), FString(TEXT("referencer")));
	TestEqual(TEXT("child reads the member and binds OnClicked"), PlanReferenceCount(PlanChild), 2);
	TestEqual(TEXT("referencer reads the member"), PlanReferenceCount(PlanReferencer), 1);
	TestEqual(TEXT("no-reference referencer is reported with no references"), PlanReferenceCount(PlanNoRef), 0);
	TestFalse(TEXT("unrelated Blueprint is not part of the plan"), FindPlanAsset(Dry, UnrelatedPath).IsValid());
	TestFalse(TEXT("unrelated Blueprint was never loaded"), IsLoaded(UnrelatedPackage));
	TestFalse(TEXT("dry run leaves the owner clean"), IsPackageDirty(OwnerPackage));
	TestFalse(TEXT("dry run leaves the child clean"), IsPackageDirty(ChildPackage));
	TestFalse(TEXT("dry run leaves the referencer clean"), IsPackageDirty(ReferencerPackage));
	TestFalse(TEXT("dry run leaves the owner widget alone"),
		Owner->WidgetTree->FindWidget(TEXT("SharedButton")) == nullptr);

	// Apply with the plan's exact modified-dependent fingerprints.
	TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
	Expected->SetObjectField(ChildPath, PlanChild->GetObjectField(TEXT("fingerprint")));
	Expected->SetObjectField(ReferencerPath, PlanReferencer->GetObjectField(TEXT("fingerprint")));
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetObjectField(TEXT("expected_dependent_fingerprints"), Expected);
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename dependents test")));
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FCortexCommandResult Applied = Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
	if (!TestTrue(TEXT("apply succeeds"), Applied.bSuccess && Applied.Data.IsValid()))
	{
		AddInfo(Applied.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("apply reports a change"), Applied.Data->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("apply never saves"), Applied.Data->GetBoolField(TEXT("saved")));
	TestEqual(TEXT("exactly one transaction for the whole rename"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	TestNotNull(TEXT("owner widget renamed"), Owner->WidgetTree->FindWidget(TEXT("RenamedButton")));
	TestNull(TEXT("old owner widget name is gone"), Owner->WidgetTree->FindWidget(TEXT("SharedButton")));

	UBlueprint* Child = FindObject<UBlueprint>(nullptr, *ChildPath);
	UBlueprint* Referencer = FindObject<UBlueprint>(nullptr, *ReferencerPath);
	UBlueprint* NoRef = FindObject<UBlueprint>(nullptr, *NoRefPath);
	if (!TestTrue(TEXT("child and referencer were loaded by the command"), Child != nullptr && Referencer != nullptr)) return false;
	const FName NewMember(TEXT("RenamedButton"));
	TestEqual(TEXT("child getter renamed"), CountVariableNodes(Child, NewMember), 1);
	TestEqual(TEXT("child getter old name gone"), CountVariableNodes(Child, SharedButtonName), 0);
	TestEqual(TEXT("child bound event renamed"), CountBoundEvents(Child, NewMember), 1);
	TestEqual(TEXT("child bound event old name gone"), CountBoundEvents(Child, SharedButtonName), 0);
	TestEqual(TEXT("referencer getter renamed"), CountVariableNodes(Referencer, NewMember), 1);
	TestEqual(TEXT("referencer getter old name gone"), CountVariableNodes(Referencer, SharedButtonName), 0);
	TestTrue(TEXT("child is dirty"), IsPackageDirty(ChildPackage));
	TestTrue(TEXT("referencer is dirty"), IsPackageDirty(ReferencerPackage));
	TestTrue(TEXT("no-reference referencer stays clean"), NoRef != nullptr && !NoRef->GetPackage()->IsDirty());
	TestFalse(TEXT("unrelated Blueprint is still untouched"), IsLoaded(UnrelatedPackage));

	// One undo restores the owner, the child and the referencer; redo reapplies.
	GEditor->UndoTransaction();
	TestNotNull(TEXT("undo restores the owner widget"), Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
	TestEqual(TEXT("undo restores the child getter"), CountVariableNodes(Child, SharedButtonName), 1);
	TestEqual(TEXT("undo restores the child bound event"), CountBoundEvents(Child, SharedButtonName), 1);
	TestEqual(TEXT("undo restores the referencer getter"), CountVariableNodes(Referencer, SharedButtonName), 1);
	GEditor->RedoTransaction();
	TestEqual(TEXT("redo renames the child getter again"), CountVariableNodes(Child, NewMember), 1);
	TestEqual(TEXT("redo renames the referencer getter again"), CountVariableNodes(Referencer, NewMember), 1);

	// Every renamed reference resolves against the renamed member.
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	FKismetEditorUtilities::CompileBlueprint(Child);
	FKismetEditorUtilities::CompileBlueprint(Referencer);
	TestTrue(TEXT("child compiles without errors"), Child->Status != BS_Error);
	TestTrue(TEXT("referencer compiles without errors"), Referencer->Status != BS_Error);
	TArray<UEdGraph*> Graphs;
	Referencer->GetAllGraphs(Graphs);
	bool bResolved = false;
	for (const UEdGraph* Graph : Graphs)
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
			{
				bResolved |= Variable->GetPropertyForVariable() != nullptr && Variable->GetVarName() == NewMember;
			}
		}
	}
	TestTrue(TEXT("referencer getter resolves the renamed member"), bResolved);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsRefusalsTest,
	"Cortex.UMG.RenameWidgetDependentsRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FRenameFixtureScope Scope;
	const FString OwnerPackage = Scope.Add(Suffix, TEXT("Owner"));
	const FString ChildPackage = Scope.Add(Suffix, TEXT("Child"));

	TStrongObjectPtr<UWidgetBlueprint> Owner(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), OwnerPackage));
	if (!TestNotNull(TEXT("owner created"), Owner.Get())) return false;
	AddButton(Owner.Get(), TEXT("SharedButton"));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	UClass* OwnerClass = Owner->GeneratedClass;
	TStrongObjectPtr<UWidgetBlueprint> Child(NewFixtureWidgetBlueprint(OwnerClass, ChildPackage));
	if (!TestNotNull(TEXT("child created"), Child.Get())) return false;
	AddGetter(Child.Get(), SharedButtonName, nullptr);
	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Child.Get(), TEXT("Conflicted"), IntType);
	if (!TestTrue(TEXT("fixtures saved"), SaveFixture(Owner.Get()) && SaveFixture(Child.Get()))) return false;
	const FString OwnerPath = Owner->GetPathName();
	const FString ChildPath = Child->GetPathName();

	FRenameRouter Harness;
	auto Dependents = [&](const TCHAR* NewName)
	{
		TSharedPtr<FJsonObject> Params = Harness.RenameParams(OwnerPath, TEXT("SharedButton"), NewName);
		Params->SetStringField(TEXT("scope"), TEXT("dependents"));
		return Params;
	};
	auto ExpectNothingDirty = [&](const TCHAR* Context)
	{
		TestFalse(*FString::Printf(TEXT("%s leaves the owner clean"), Context), Owner->GetPackage()->IsDirty());
		TestFalse(*FString::Printf(TEXT("%s leaves the child clean"), Context), Child->GetPackage()->IsDirty());
		TestNotNull(*FString::Printf(TEXT("%s keeps the widget name"), Context), Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
		TestEqual(*FString::Printf(TEXT("%s keeps the child getter"), Context), CountVariableNodes(Child.Get(), SharedButtonName), 1);
	};

	const FCortexCommandResult Dry = Harness.Router.Execute(TEXT("umg.rename_widget"), Dependents(TEXT("RenamedButton")));
	if (!TestTrue(TEXT("dry run for the refusal fixture succeeds"), Dry.bSuccess && Dry.Data.IsValid())) return false;
	const TSharedPtr<FJsonObject> PlanChild = FindPlanAsset(Dry, ChildPath);
	if (!TestTrue(TEXT("child is planned"), PlanChild.IsValid())) return false;
	const TSharedPtr<FJsonObject> ChildFingerprint = PlanChild->GetObjectField(TEXT("fingerprint"));

	auto Apply = [&](const TSharedPtr<FJsonObject>& ExpectedDependents)
	{
		TSharedPtr<FJsonObject> Params = Dependents(TEXT("RenamedButton"));
		Params->SetBoolField(TEXT("dry_run"), false);
		if (ExpectedDependents.IsValid())
		{
			Params->SetObjectField(TEXT("expected_dependent_fingerprints"), ExpectedDependents);
		}
		return Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
	};

	// Missing entry.
	FCortexCommandResult Missing = Apply(MakeShared<FJsonObject>());
	TestFalse(TEXT("missing dependent fingerprint refuses"), Missing.bSuccess);
	TestEqual(TEXT("missing dependent fingerprint code"), Missing.ErrorCode, CortexErrorCodes::StalePrecondition);
	ExpectNothingDirty(TEXT("missing fingerprint"));

	// Absent parameter behaves as an empty key set.
	FCortexCommandResult Absent = Apply(nullptr);
	TestEqual(TEXT("absent dependent fingerprints code"), Absent.ErrorCode, CortexErrorCodes::StalePrecondition);
	ExpectNothingDirty(TEXT("absent fingerprints"));

	// Extra entry.
	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetObjectField(ChildPath, ChildFingerprint);
	Extra->SetObjectField(TEXT("/Game/Temp/CortexRenameDependents/NotInThePlan.NotInThePlan"), ChildFingerprint);
	FCortexCommandResult ExtraResult = Apply(Extra);
	TestFalse(TEXT("extra dependent fingerprint refuses"), ExtraResult.bSuccess);
	TestEqual(TEXT("extra dependent fingerprint code"), ExtraResult.ErrorCode, CortexErrorCodes::StalePrecondition);
	ExpectNothingDirty(TEXT("extra fingerprint"));

	// Stale entry.
	TSharedPtr<FJsonObject> StaleSet = MakeShared<FJsonObject>();
	StaleSet->SetObjectField(ChildPath, CopyWithField(ChildFingerprint, TEXT("package_saved_hash"), TEXT("stale")));
	FCortexCommandResult Stale = Apply(StaleSet);
	TestFalse(TEXT("stale dependent fingerprint refuses"), Stale.bSuccess);
	TestEqual(TEXT("stale dependent fingerprint code"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
	ExpectNothingDirty(TEXT("stale fingerprint"));

	// A child that already declares new_name is a conflict, for the plan and for the apply.
	FCortexCommandResult ConflictDry = Harness.Router.Execute(TEXT("umg.rename_widget"), Dependents(TEXT("Conflicted")));
	TestFalse(TEXT("child declaring the new name refuses the plan"), ConflictDry.bSuccess);
	TestEqual(TEXT("conflict refusal code"), ConflictDry.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("conflict refusal names the child"), ConflictDry.ErrorMessage.Contains(TEXT("Conflicted")));
	ExpectNothingDirty(TEXT("conflict"));

	// scope asset keeps the existing refusal for a loaded child, and rejects the dependents-only params.
	TSharedPtr<FJsonObject> AssetScope = Harness.RenameParams(OwnerPath, TEXT("SharedButton"), TEXT("RenamedButton"));
	FCortexCommandResult AssetRefusal = Harness.Router.Execute(TEXT("umg.rename_widget"), AssetScope);
	TestFalse(TEXT("scope asset refuses a loaded child"), AssetRefusal.bSuccess);
	TestEqual(TEXT("scope asset refusal code"), AssetRefusal.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("scope asset refusal names the child"), AssetRefusal.ErrorMessage.Contains(ChildPath));
	ExpectNothingDirty(TEXT("scope asset"));
	AssetScope->SetBoolField(TEXT("dry_run"), true);
	TestEqual(TEXT("dry_run requires scope dependents"),
		Harness.Router.Execute(TEXT("umg.rename_widget"), AssetScope).ErrorCode, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> BadScope = Harness.RenameParams(OwnerPath, TEXT("SharedButton"), TEXT("RenamedButton"));
	BadScope->SetStringField(TEXT("scope"), TEXT("everything"));
	TestEqual(TEXT("unknown scope refuses"),
		Harness.Router.Execute(TEXT("umg.rename_widget"), BadScope).ErrorCode, CortexErrorCodes::InvalidField);
	ExpectNothingDirty(TEXT("invalid params"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetOntoBindWidgetTest,
	"Cortex.UMG.RenameWidgetOntoBindWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetOntoBindWidgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	// The fixture is deliberately missing the required native bindings it does not exercise.
	AddExpectedMessagePlain(TEXT("A required widget binding"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	UWidgetBlueprint* Raw = NewFixtureWidgetBlueprint(UCortexUMGRenameBindWidgetBase::StaticClass(),
		FString::Printf(TEXT("/Temp/CortexRenameOntoBindWidget_%s"), *Suffix));
	TStrongObjectPtr<UWidgetBlueprint> Owner(Raw);
	if (!TestNotNull(TEXT("owner with native BindWidget parent created"), Owner.Get())) return false;
	AddButton(Owner.Get(), TEXT("PlainButton"));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	Owner->GetPackage()->ClearDirtyFlag();
	const FString OwnerPath = Owner->GetPathName();
	FRenameRouter Harness;

	// A Button cannot take the name of a BindWidget Image.
	TSharedPtr<FJsonObject> Incompatible = Harness.RenameParams(OwnerPath, TEXT("PlainButton"), TEXT("NativeImage"));
	const FCortexCommandResult Refused = Harness.Router.Execute(TEXT("umg.rename_widget"), Incompatible);
	TestFalse(TEXT("incompatible BindWidget name refuses"), Refused.bSuccess);
	TestEqual(TEXT("incompatible refusal code"), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestNotNull(TEXT("refusal keeps the widget name"), Owner->WidgetTree->FindWidget(TEXT("PlainButton")));
	TestFalse(TEXT("refusal leaves the owner clean"), Owner->GetPackage()->IsDirty());
	Incompatible->SetStringField(TEXT("scope"), TEXT("dependents"));
	TestFalse(TEXT("dependents scope refuses the incompatible name too"),
		Harness.Router.Execute(TEXT("umg.rename_widget"), Incompatible).bSuccess);
	TestNotNull(TEXT("dependents refusal keeps the widget name"), Owner->WidgetTree->FindWidget(TEXT("PlainButton")));

	// A compatible inherited BindWidget name is admitted and binds after compile.
	const FCortexCommandResult Renamed = Harness.Router.Execute(TEXT("umg.rename_widget"),
		Harness.RenameParams(OwnerPath, TEXT("PlainButton"), TEXT("NativeButton")));
	if (!TestTrue(TEXT("compatible BindWidget name is admitted"), Renamed.bSuccess))
	{
		AddInfo(Renamed.ErrorMessage);
		return false;
	}
	TestNotNull(TEXT("widget carries the inherited BindWidget name"), Owner->WidgetTree->FindWidget(TEXT("NativeButton")));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());

	TSharedPtr<FJsonObject> CheckParams = MakeShared<FJsonObject>();
	CheckParams->SetStringField(TEXT("asset_path"), OwnerPath);
	const FCortexCommandResult Check = Harness.Router.Execute(TEXT("umg.check_native_bindings"), CheckParams);
	if (!TestTrue(TEXT("native binding check succeeds"), Check.bSuccess && Check.Data.IsValid())) return false;
	FString NativeButtonStatus;
	for (const TSharedPtr<FJsonValue>& Value : Check.Data->GetArrayField(TEXT("bindings")))
	{
		const TSharedPtr<FJsonObject> Binding = Value->AsObject();
		if (Binding->GetStringField(TEXT("name")) == TEXT("NativeButton"))
		{
			NativeButtonStatus = Binding->GetStringField(TEXT("status"));
		}
	}
	TestEqual(TEXT("renamed widget is matched to the native BindWidget member"), NativeButtonStatus, FString(TEXT("matched")));

	Owner->GetPackage()->ClearDirtyFlag();
	return true;
}

namespace
{
	/** Owner widget Blueprint plus a loaded child and a loaded referencer that both read the renamed member. */
	struct FLoadedRenameFixture
	{
		FRenameFixtureScope Scope;
		TStrongObjectPtr<UWidgetBlueprint> Owner;
		TStrongObjectPtr<UWidgetBlueprint> Child;
		TStrongObjectPtr<UBlueprint> Referencer;
		FString OwnerPath;
		FString ChildPath;
		FString ReferencerPath;

		bool Build(FAutomationTestBase& Test)
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
			Owner.Reset(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), Scope.Add(Suffix, TEXT("Owner"))));
			if (!Test.TestNotNull(TEXT("owner created"), Owner.Get())) return false;
			AddButton(Owner.Get(), TEXT("SharedButton"));
			FKismetEditorUtilities::CompileBlueprint(Owner.Get());
			UClass* OwnerClass = Owner->GeneratedClass;
			Child.Reset(NewFixtureWidgetBlueprint(OwnerClass, Scope.Add(Suffix, TEXT("Child"))));
			if (!Test.TestNotNull(TEXT("child created"), Child.Get())) return false;
			const bool bChildNodes = AddGetter(Child.Get(), SharedButtonName, nullptr) != nullptr
				&& AddBoundOnClicked(Child.Get(), OwnerClass) != nullptr;
			if (!Test.TestTrue(TEXT("child graph nodes created"), bChildNodes)) return false;
			Referencer.Reset(NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), Scope.Add(Suffix, TEXT("Referencer"))));
			if (!Test.TestNotNull(TEXT("referencer created"), Referencer.Get())) return false;
			AddGetter(Referencer.Get(), SharedButtonName, OwnerClass);
			if (!Test.TestTrue(TEXT("fixtures saved"),
				SaveFixture(Owner.Get()) && SaveFixture(Child.Get()) && SaveFixture(Referencer.Get()))) return false;
			OwnerPath = Owner->GetPathName();
			ChildPath = Child->GetPathName();
			ReferencerPath = Referencer->GetPathName();
			return true;
		}
	};

	TSharedPtr<FJsonObject> DependentsParams(FRenameRouter& Harness, const FString& OwnerPath, const TCHAR* NewName)
	{
		TSharedPtr<FJsonObject> Params = Harness.RenameParams(OwnerPath, TEXT("SharedButton"), NewName);
		Params->SetStringField(TEXT("scope"), TEXT("dependents"));
		return Params;
	}

	/** Dry-runs the rename and turns the plan's modified dependents into the exact apply parameters. */
	bool PlanApplyParams(FAutomationTestBase& Test, FRenameRouter& Harness, const FString& OwnerPath,
		const TCHAR* NewName, const TArray<FString>& ModifiedPaths, TSharedPtr<FJsonObject>& OutParams)
	{
		OutParams = DependentsParams(Harness, OwnerPath, NewName);
		const FCortexCommandResult Dry = Harness.Router.Execute(TEXT("umg.rename_widget"), OutParams);
		if (!Test.TestTrue(TEXT("dry run succeeds"), Dry.bSuccess && Dry.Data.IsValid())) return false;
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		for (const FString& Path : ModifiedPaths)
		{
			const TSharedPtr<FJsonObject> PlanAsset = FindPlanAsset(Dry, Path);
			if (!Test.TestTrue(*FString::Printf(TEXT("plan lists %s"), *Path), PlanAsset.IsValid())) return false;
			Expected->SetObjectField(Path, PlanAsset->GetObjectField(TEXT("fingerprint")));
		}
		OutParams->SetBoolField(TEXT("dry_run"), false);
		OutParams->SetObjectField(TEXT("expected_dependent_fingerprints"), Expected);
		return true;
	}

	int32 BlockedCount(const FCortexCommandResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Blocked = nullptr;
		return Result.ErrorDetails.IsValid() && Result.ErrorDetails->TryGetArrayField(TEXT("blocked"), Blocked)
			? Blocked->Num() : -1;
	}

	FString RollbackStatus(const FCortexCommandResult& Result)
	{
		FString Status;
		if (Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails->TryGetStringField(TEXT("rollback_status"), Status);
		}
		return Status;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsRollbackTest,
	"Cortex.UMG.RenameWidgetDependentsRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr)) return false;
	FLoadedRenameFixture Fx;
	if (!Fx.Build(*this)) return false;
	// An already-dirty dependent must come back dirty; clean ones must come back clean.
	Fx.Referencer->MarkPackageDirty();
	FRenameRouter Harness;
	const TSharedPtr<FJsonObject> OwnerBefore = Harness.TreeFingerprint(Fx.OwnerPath);
	const TSharedPtr<FJsonObject> ChildBefore = MakeObjectAssetFingerprint(Fx.Child.Get()).ToJson();
	const TSharedPtr<FJsonObject> ReferencerBefore = MakeObjectAssetFingerprint(Fx.Referencer.Get()).ToJson();
	if (!TestTrue(TEXT("owner fingerprint captured"), OwnerBefore.IsValid())) return false;

	TSharedPtr<FJsonObject> Params2;
	if (!PlanApplyParams(*this, Harness, Fx.OwnerPath, TEXT("RenamedButton"), { Fx.ChildPath, Fx.ReferencerPath }, Params2)) return false;

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename rollback test")));
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	CortexUMGRenameDependents::SetFaultForTesting(TEXT("leftover_reference"));
	const FCortexCommandResult Failed = Harness.Router.Execute(TEXT("umg.rename_widget"), Params2);
	CortexUMGRenameDependents::SetFaultForTesting(NAME_None);

	TestFalse(TEXT("a leftover reference fails the request"), Failed.bSuccess);
	TestEqual(TEXT("failure code"), Failed.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("rollback is reported as restored"), RollbackStatus(Failed), FString(TEXT("restored")));
	TestEqual(TEXT("nothing is blocked"), BlockedCount(Failed), 0);
	TestNotNull(TEXT("owner widget name is restored"), Fx.Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
	TestNull(TEXT("owner has no renamed widget"), Fx.Owner->WidgetTree->FindWidget(TEXT("RenamedButton")));
	const FName NewMember(TEXT("RenamedButton"));
	TestEqual(TEXT("child getter restored"), CountVariableNodes(Fx.Child.Get(), SharedButtonName), 1);
	TestEqual(TEXT("child getter is not renamed"), CountVariableNodes(Fx.Child.Get(), NewMember), 0);
	TestEqual(TEXT("child bound event restored"), CountBoundEvents(Fx.Child.Get(), SharedButtonName), 1);
	TestEqual(TEXT("child bound event is not renamed"), CountBoundEvents(Fx.Child.Get(), NewMember), 0);
	TestEqual(TEXT("referencer getter restored"), CountVariableNodes(Fx.Referencer.Get(), SharedButtonName), 1);
	TestEqual(TEXT("referencer getter is not renamed"), CountVariableNodes(Fx.Referencer.Get(), NewMember), 0);
	TestFalse(TEXT("owner dirty flag restored"), Fx.Owner->GetPackage()->IsDirty());
	TestFalse(TEXT("child dirty flag restored"), Fx.Child->GetPackage()->IsDirty());
	TestTrue(TEXT("referencer dirty flag restored"), Fx.Referencer->GetPackage()->IsDirty());
	TestTrue(TEXT("owner fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(Harness.TreeFingerprint(Fx.OwnerPath), OwnerBefore));
	TestTrue(TEXT("child fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Fx.Child.Get()).ToJson(), ChildBefore));
	TestTrue(TEXT("referencer fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Fx.Referencer.Get()).ToJson(), ReferencerBefore));
	FString Reason;
	TestFalse(TEXT("owner is not blocked"), FCortexAssetMutationGuard::IsBlocked(Fx.Owner.Get(), Reason));
	TestFalse(TEXT("child is not blocked"), FCortexAssetMutationGuard::IsBlocked(Fx.Child.Get(), Reason));
	TestFalse(TEXT("referencer is not blocked"), FCortexAssetMutationGuard::IsBlocked(Fx.Referencer.Get(), Reason));
	TestEqual(TEXT("the undone request leaves no undo record behind"), GEditor->Trans->GetQueueLength(), QueueBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsBatchRollbackTest,
	"Cortex.UMG.RenameWidgetDependentsBatchRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsBatchRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr)) return false;
	FLoadedRenameFixture Fx;
	if (!Fx.Build(*this)) return false;
	FRenameRouter Harness;
	TSharedPtr<FJsonObject> ApplyParams;
	if (!PlanApplyParams(*this, Harness, Fx.OwnerPath, TEXT("RenamedButton"), { Fx.ChildPath, Fx.ReferencerPath }, ApplyParams)) return false;

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename batch rollback test")));
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	FCortexCommandResult Failed;
	{
		// A batch owns one outer transaction; the rename must neither cancel nor undo it.
		FCortexBatchScope BatchScope;
		TOptional<FScopedTransaction> Outer;
		Outer.Emplace(FText::FromString(TEXT("Cortex batch")));
		CortexUMGRenameDependents::SetFaultForTesting(TEXT("leftover_reference"));
		Failed = Harness.Router.Execute(TEXT("umg.rename_widget"), ApplyParams);
		CortexUMGRenameDependents::SetFaultForTesting(NAME_None);
		TestTrue(TEXT("the outer batch transaction is still open"), GEditor->IsTransactionActive());
		Outer.Reset();
	}
	TestFalse(TEXT("a leftover reference fails the request inside a batch"), Failed.bSuccess);
	TestEqual(TEXT("the in-batch rollback cannot be verified"), RollbackStatus(Failed), FString(TEXT("unverified")));
	TestTrue(TEXT("the unverifiable assets are blocked"), BlockedCount(Failed) >= 1);
	FString Reason;
	TestTrue(TEXT("the owner is blocked"), FCortexAssetMutationGuard::IsBlocked(Fx.Owner.Get(), Reason));
	TestEqual(TEXT("the outer batch transaction was kept, not cancelled"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	GEditor->UndoTransaction();
	TestNotNull(TEXT("undoing the outer transaction restores the owner widget"),
		Fx.Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
	TestEqual(TEXT("undoing the outer transaction restores the child getter"),
		CountVariableNodes(Fx.Child.Get(), SharedButtonName), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsChildConflictsTest,
	"Cortex.UMG.RenameWidgetDependentsChildConflicts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsChildConflictsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FRenameFixtureScope Scope;
	TStrongObjectPtr<UWidgetBlueprint> Owner(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), Scope.Add(Suffix, TEXT("Owner"))));
	if (!TestNotNull(TEXT("owner created"), Owner.Get())) return false;
	AddButton(Owner.Get(), TEXT("SharedButton"));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	TStrongObjectPtr<UWidgetBlueprint> Child(NewFixtureWidgetBlueprint(Owner->GeneratedClass, Scope.Add(Suffix, TEXT("Child"))));
	if (!TestNotNull(TEXT("child created"), Child.Get())) return false;
	// The engine rename silently renames a child function graph or timeline that collides with new_name.
	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(Child.Get(), TEXT("ConflictFunction"),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Child.Get(), FunctionGraph, true, nullptr);
	// The editor API refuses timelines in Widget Blueprints; a template that is present still collides.
	// Templates live in the Blueprint's generated class, which the engine's lookup requires.
	Child->Timelines.Add(NewObject<UTimelineTemplate>(Child->GeneratedClass,
		*UTimelineTemplate::TimelineVariableNameToTemplateName(TEXT("ConflictTimeline")), RF_Transactional));
	if (!TestTrue(TEXT("fixtures saved"), SaveFixture(Owner.Get()) && SaveFixture(Child.Get()))) return false;
	const FString OwnerPath = Owner->GetPathName();
	const FString ChildPath = Child->GetPathName();

	FRenameRouter Harness;
	for (const TCHAR* Conflicting : { TEXT("ConflictFunction"), TEXT("ConflictTimeline") })
	{
		for (const bool bApply : { false, true })
		{
			TSharedPtr<FJsonObject> Params = DependentsParams(Harness, OwnerPath, Conflicting);
			if (bApply)
			{
				Params->SetBoolField(TEXT("dry_run"), false);
				Params->SetObjectField(TEXT("expected_dependent_fingerprints"), MakeShared<FJsonObject>());
			}
			const FString Context = FString::Printf(TEXT("%s %s"), Conflicting, bApply ? TEXT("apply") : TEXT("dry run"));
			const FCortexCommandResult Refused = Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
			TestFalse(*(Context + TEXT(" refuses")), Refused.bSuccess);
			TestEqual(*(Context + TEXT(" code")), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestTrue(*(Context + TEXT(" names the colliding name")), Refused.ErrorMessage.Contains(Conflicting));
			TestFalse(*(Context + TEXT(" leaves the owner clean")), Owner->GetPackage()->IsDirty());
			TestFalse(*(Context + TEXT(" leaves the child clean")), Child->GetPackage()->IsDirty());
			TestNotNull(*(Context + TEXT(" keeps the widget name")), Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
		}
	}
	TestTrue(TEXT("child function graph keeps its name"),
		Child->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph->GetFName() == FName(TEXT("ConflictFunction")); }));
	TestNotNull(TEXT("child timeline keeps its name"), Child->FindTimelineTemplateByVariableName(TEXT("ConflictTimeline")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsSameNamedMemberTest,
	"Cortex.UMG.RenameWidgetDependentsSameNamedMember",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsSameNamedMemberTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FRenameFixtureScope Scope;
	TStrongObjectPtr<UWidgetBlueprint> Owner(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), Scope.Add(Suffix, TEXT("Owner"))));
	if (!TestNotNull(TEXT("owner created"), Owner.Get())) return false;
	AddButton(Owner.Get(), TEXT("SharedButton"));
	FKismetEditorUtilities::CompileBlueprint(Owner.Get());
	UClass* OwnerClass = Owner->GeneratedClass;

	// Holds the owner type, owns a variable with the renamed widget's name and reads only that variable.
	TStrongObjectPtr<UBlueprint> Referencer(NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), Scope.Add(Suffix, TEXT("Referencer"))));
	if (!TestNotNull(TEXT("referencer created"), Referencer.Get())) return false;
	FEdGraphPinType OwnerType;
	OwnerType.PinCategory = UEdGraphSchema_K2::PC_Object;
	OwnerType.PinSubCategoryObject = OwnerClass;
	FBlueprintEditorUtils::AddMemberVariable(Referencer.Get(), TEXT("OwnerInstance"), OwnerType);
	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Referencer.Get(), SharedButtonName, IntType);
	AddGetter(Referencer.Get(), SharedButtonName, nullptr);
	if (!TestTrue(TEXT("fixtures saved"), SaveFixture(Owner.Get()) && SaveFixture(Referencer.Get()))) return false;
	const FString OwnerPath = Owner->GetPathName();
	const FString ReferencerPath = Referencer->GetPathName();
	const TSharedPtr<FJsonObject> ReferencerBefore = MakeObjectAssetFingerprint(Referencer.Get()).ToJson();

	FRenameRouter Harness;
	TSharedPtr<FJsonObject> ApplyParams;
	if (!PlanApplyParams(*this, Harness, OwnerPath, TEXT("RenamedButton"), {}, ApplyParams)) return false;
	const FCortexCommandResult Applied = Harness.Router.Execute(TEXT("umg.rename_widget"), ApplyParams);
	if (!TestTrue(TEXT("apply succeeds"), Applied.bSuccess && Applied.Data.IsValid()))
	{
		AddInfo(Applied.ErrorMessage);
		return false;
	}
	TestNotNull(TEXT("owner widget renamed"), Owner->WidgetTree->FindWidget(TEXT("RenamedButton")));
	TestFalse(TEXT("the referencer's own same-named member does not dirty it"), Referencer->GetPackage()->IsDirty());
	TestTrue(TEXT("the referencer fingerprint is unchanged"),
		FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Referencer.Get()).ToJson(), ReferencerBefore));
	TestEqual(TEXT("the referencer keeps its own getter"), CountVariableNodes(Referencer.Get(), SharedButtonName), 1);
	TestEqual(TEXT("the referencer getter is not renamed"), CountVariableNodes(Referencer.Get(), FName(TEXT("RenamedButton"))), 0);
	TestTrue(TEXT("the referencer keeps its own same-named variable"),
		Referencer->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable) { return Variable.VarName == SharedButtonName; }));
	TestFalse(TEXT("the referencer variable is not renamed"),
		Referencer->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable) { return Variable.VarName == FName(TEXT("RenamedButton")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsBlockedDependentTest,
	"Cortex.UMG.RenameWidgetDependentsBlockedDependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsBlockedDependentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr)) return false;
	FLoadedRenameFixture Fx;
	if (!Fx.Build(*this)) return false;
	FCortexAssetMutationGuard::Block(Fx.Referencer.Get(), TEXT("test: unverified recovery"));
	FRenameRouter Harness;
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename blocked dependent test")));
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();

	TSharedPtr<FJsonObject> Params = DependentsParams(Harness, Fx.OwnerPath, TEXT("RenamedButton"));
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetObjectField(TEXT("expected_dependent_fingerprints"), MakeShared<FJsonObject>());
	const FCortexCommandResult Refused = Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
	TestFalse(TEXT("a blocked modified dependent refuses the rename"), Refused.bSuccess);
	TestEqual(TEXT("blocked refusal code"), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("blocked refusal names the asset"), Refused.ErrorMessage.Contains(Fx.ReferencerPath));
	TestNotNull(TEXT("refusal keeps the owner widget"), Fx.Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
	TestFalse(TEXT("refusal leaves the owner clean"), Fx.Owner->GetPackage()->IsDirty());
	TestFalse(TEXT("refusal leaves the child clean"), Fx.Child->GetPackage()->IsDirty());
	TestEqual(TEXT("refusal opens no transaction"), GEditor->Trans->GetQueueLength(), QueueBefore);
	return true;
}

namespace
{
	/** A planned dependent that has no references (its own same-named member only) is touched by an injected fault. */
	bool RunUnmodifiedDependentFault(FAutomationTestBase& Test, const TCHAR* Fault, bool bExpectSuccess)
	{
		if (!Test.TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr)) return false;
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		FRenameFixtureScope Scope;
		TStrongObjectPtr<UWidgetBlueprint> Owner(NewFixtureWidgetBlueprint(UUserWidget::StaticClass(), Scope.Add(Suffix, TEXT("Owner"))));
		if (!Test.TestNotNull(TEXT("owner created"), Owner.Get())) return false;
		AddButton(Owner.Get(), TEXT("SharedButton"));
		FKismetEditorUtilities::CompileBlueprint(Owner.Get());
		TStrongObjectPtr<UBlueprint> Referencer(NewSavedFixtureBlueprint(UObject::StaticClass(), UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), Scope.Add(Suffix, TEXT("Referencer"))));
		if (!Test.TestNotNull(TEXT("referencer created"), Referencer.Get())) return false;
		FEdGraphPinType OwnerType;
		OwnerType.PinCategory = UEdGraphSchema_K2::PC_Object;
		OwnerType.PinSubCategoryObject = Owner->GeneratedClass;
		FBlueprintEditorUtils::AddMemberVariable(Referencer.Get(), TEXT("OwnerInstance"), OwnerType);
		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
		FBlueprintEditorUtils::AddMemberVariable(Referencer.Get(), SharedButtonName, IntType);
		AddGetter(Referencer.Get(), SharedButtonName, nullptr);
		if (!Test.TestTrue(TEXT("fixtures saved"), SaveFixture(Owner.Get()) && SaveFixture(Referencer.Get()))) return false;
		const FString OwnerPath = Owner->GetPathName();
		const TSharedPtr<FJsonObject> ReferencerBefore = MakeObjectAssetFingerprint(Referencer.Get()).ToJson();
		const EBlueprintStatus StatusBefore = Referencer->Status;

		FRenameRouter Harness;
		TSharedPtr<FJsonObject> ApplyParams;
		if (!PlanApplyParams(Test, Harness, OwnerPath, TEXT("RenamedButton"), {}, ApplyParams)) return false;
		GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename unmodified dependent test")));
		const int32 QueueBefore = GEditor->Trans->GetQueueLength();
		CortexUMGRenameDependents::SetFaultForTesting(Fault);
		const FCortexCommandResult Result = Harness.Router.Execute(TEXT("umg.rename_widget"), ApplyParams);
		CortexUMGRenameDependents::SetFaultForTesting(NAME_None);

		Test.TestEqual(*FString::Printf(TEXT("%s: request outcome"), Fault), Result.bSuccess, bExpectSuccess);
		Test.TestFalse(*FString::Printf(TEXT("%s: the referencer is back to its planned dirty flag"), Fault), Referencer->GetPackage()->IsDirty());
		Test.TestTrue(*FString::Printf(TEXT("%s: the referencer fingerprint is exactly the planned one"), Fault),
			FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Referencer.Get()).ToJson(), ReferencerBefore));
		Test.TestEqual(*FString::Printf(TEXT("%s: the referencer status is the planned one"), Fault), Referencer->Status, StatusBefore);
		Test.TestEqual(*FString::Printf(TEXT("%s: the referencer keeps its own getter"), Fault),
			CountVariableNodes(Referencer.Get(), SharedButtonName), 1);
		if (bExpectSuccess)
		{
			Test.TestNotNull(TEXT("owner widget renamed"), Owner->WidgetTree->FindWidget(TEXT("RenamedButton")));
			return true;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: the unplanned edit is rolled back"), Fault), RollbackStatus(Result), FString(TEXT("restored")));
		Test.TestEqual(*FString::Printf(TEXT("%s: nothing is blocked"), Fault), BlockedCount(Result), 0);
		Test.TestNotNull(TEXT("owner widget name is restored"), Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
		Test.TestEqual(TEXT("the undone request leaves no undo record behind"), GEditor->Trans->GetQueueLength(), QueueBefore);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsMarkedUnmodifiedTest,
	"Cortex.UMG.RenameWidgetDependentsMarkedUnmodified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsMarkedUnmodifiedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// The engine only marks the reference-free dependent modified: it returns to its planned dirty flag and status.
	return RunUnmodifiedDependentFault(*this, TEXT("dirty_unmodified_dependent"), true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsEditedUnmodifiedTest,
	"Cortex.UMG.RenameWidgetDependentsEditedUnmodified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsEditedUnmodifiedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// An unplanned edit of a reference-free dependent fails the request, which is undone and verified.
	return RunUnmodifiedDependentFault(*this, TEXT("edit_unmodified_dependent"), false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetDependentsRollbackWithRedoHistoryTest,
	"Cortex.UMG.RenameWidgetDependentsRollbackWithRedoHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetDependentsRollbackWithRedoHistoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestTrue(TEXT("editor transaction buffer available"), GEditor != nullptr && GEditor->Trans != nullptr)) return false;
	FLoadedRenameFixture Fx;
	if (!Fx.Build(*this)) return false;
	// An unrelated transaction that was performed and undone leaves redo history behind.
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex rename redo history test")));
	Fx.Referencer->SetFlags(RF_Transactional);
	{
		FScopedTransaction Unrelated(FText::FromString(TEXT("Unrelated edit")));
		Fx.Referencer->Modify();
		Fx.Referencer->BlueprintCategory = TEXT("UnrelatedEdit");
	}
	TestTrue(TEXT("the unrelated transaction was recorded"), GEditor->Trans->GetQueueLength() == 1);
	TestTrue(TEXT("the unrelated transaction undoes"), GEditor->UndoTransaction());
	if (!TestTrue(TEXT("redo history exists"), GEditor->Trans->GetUndoCount() > 0)) return false;

	FRenameRouter Harness;
	const TSharedPtr<FJsonObject> OwnerBefore = Harness.TreeFingerprint(Fx.OwnerPath);
	const TSharedPtr<FJsonObject> ChildBefore = MakeObjectAssetFingerprint(Fx.Child.Get()).ToJson();
	const TSharedPtr<FJsonObject> ReferencerBefore = MakeObjectAssetFingerprint(Fx.Referencer.Get()).ToJson();
	TSharedPtr<FJsonObject> ApplyParams;
	if (!PlanApplyParams(*this, Harness, Fx.OwnerPath, TEXT("RenamedButton"), { Fx.ChildPath, Fx.ReferencerPath }, ApplyParams)) return false;

	CortexUMGRenameDependents::SetFaultForTesting(TEXT("leftover_reference"));
	const FCortexCommandResult Failed = Harness.Router.Execute(TEXT("umg.rename_widget"), ApplyParams);
	CortexUMGRenameDependents::SetFaultForTesting(NAME_None);

	TestFalse(TEXT("a leftover reference fails the request"), Failed.bSuccess);
	TestEqual(TEXT("rollback is reported as restored"), RollbackStatus(Failed), FString(TEXT("restored")));
	TestEqual(TEXT("nothing is blocked"), BlockedCount(Failed), 0);
	TestNotNull(TEXT("owner widget name is restored"), Fx.Owner->WidgetTree->FindWidget(TEXT("SharedButton")));
	TestEqual(TEXT("child getter restored"), CountVariableNodes(Fx.Child.Get(), SharedButtonName), 1);
	TestEqual(TEXT("referencer getter restored"), CountVariableNodes(Fx.Referencer.Get(), SharedButtonName), 1);
	TestTrue(TEXT("owner fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(Harness.TreeFingerprint(Fx.OwnerPath), OwnerBefore));
	TestTrue(TEXT("child fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Fx.Child.Get()).ToJson(), ChildBefore));
	TestTrue(TEXT("referencer fingerprint restored exactly"),
		FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Fx.Referencer.Get()).ToJson(), ReferencerBefore));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGRenameWidgetAwayFromBindWidgetTest,
	"Cortex.UMG.RenameWidgetAwayFromBindWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetAwayFromBindWidgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	AddExpectedMessagePlain(TEXT("A required widget binding"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	FRenameFixtureScope Scope;
	TStrongObjectPtr<UWidgetBlueprint> Required(NewFixtureWidgetBlueprint(UCortexUMGRenameBindWidgetBase::StaticClass(),
		Scope.Add(Suffix, TEXT("Required"))));
	if (!TestNotNull(TEXT("required-binding Blueprint created"), Required.Get())) return false;
	AddButton(Required.Get(), TEXT("NativeButton"));
	UImage* Image = Required->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("NativeImage"));
	Image->bIsVariable = true;
	Cast<UCanvasPanel>(Required->WidgetTree->RootWidget)->AddChild(Image);
	FKismetEditorUtilities::CompileBlueprint(Required.Get());
	Required->GetPackage()->ClearDirtyFlag();
	const FString RequiredPath = Required->GetPathName();

	FRenameRouter Harness;
	for (const bool bDependents : { false, true })
	{
		TSharedPtr<FJsonObject> Params = Harness.RenameParams(RequiredPath, TEXT("NativeButton"), TEXT("Label"));
		if (bDependents)
		{
			Params->SetStringField(TEXT("scope"), TEXT("dependents"));
		}
		const FString Context = bDependents ? TEXT("dependents scope") : TEXT("asset scope");
		const FCortexCommandResult Refused = Harness.Router.Execute(TEXT("umg.rename_widget"), Params);
		TestFalse(*(Context + TEXT(" refuses renaming away from a required BindWidget name")), Refused.bSuccess);
		TestEqual(*(Context + TEXT(" code")), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(*(Context + TEXT(" names the native property")), Refused.ErrorMessage.Contains(TEXT("NativeButton")));
		TestNotNull(*(Context + TEXT(" keeps the widget name")), Required->WidgetTree->FindWidget(TEXT("NativeButton")));
		TestFalse(*(Context + TEXT(" leaves the owner clean")), Required->GetPackage()->IsDirty());
	}

	// An optional binding may be unbound, and the result says so.
	TStrongObjectPtr<UWidgetBlueprint> Optional(NewFixtureWidgetBlueprint(UCortexUMGRenameOptionalBindWidgetBase::StaticClass(),
		Scope.Add(Suffix, TEXT("Optional"))));
	if (!TestNotNull(TEXT("optional-binding Blueprint created"), Optional.Get())) return false;
	AddButton(Optional.Get(), TEXT("OptionalButton"));
	FKismetEditorUtilities::CompileBlueprint(Optional.Get());
	const FCortexCommandResult Renamed = Harness.Router.Execute(TEXT("umg.rename_widget"),
		Harness.RenameParams(Optional->GetPathName(), TEXT("OptionalButton"), TEXT("Label")));
	if (!TestTrue(TEXT("renaming away from an optional BindWidget name succeeds"), Renamed.bSuccess && Renamed.Data.IsValid()))
	{
		AddInfo(Renamed.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("the result reports the unbound optional BindWidget"),
		Renamed.Data->HasField(TEXT("unbinds_optional_bindwidget")) && Renamed.Data->GetBoolField(TEXT("unbinds_optional_bindwidget")));
	Optional->GetPackage()->ClearDirtyFlag();
	Required->GetPackage()->ClearDirtyFlag();
	return true;
}
