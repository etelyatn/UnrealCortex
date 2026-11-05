// umg.rename_widget scope:"dependents": the rename reaches unloaded child Blueprints and referencing
// Blueprints through the Asset Registry, is planned with dry_run, guarded by exact per-dependent
// fingerprints, undone by one transaction, and honours inherited BindWidget names.

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
