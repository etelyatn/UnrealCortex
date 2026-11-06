#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGPropertyBindingTestUtils.h"
#include "PackageTools.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"
#include "WidgetEditingProjectSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingPersistenceTest,
								 "Cortex.UMG.PropertyBinding.Persistence.ExplicitLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingPersistenceTest::RunTest(const FString& Parameters)
{
	const FString PackageName =
		TEXT("/Game/Temp/CortexPropertyBinding_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filename =
		FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*Filename) || FPackageName::DoesPackageExist(PackageName))
	{
		AddError(TEXT("Refusing to overwrite existing fixture package"));
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*Filename);
	};
	FCortexUMGPropertyBindingFixture Fixture(PackageName);
	TSharedPtr<FJsonObject> RetainedParams = Fixture.WriteParams(Fixture.Property(TEXT("RenderOpacity")));
	RetainedParams->SetStringField(TEXT("widget_name"), TEXT("RetainedDisplay"));
	const FCortexCommandResult RetainedSet = Fixture.Router.Execute(TEXT("umg.set_property_binding"), RetainedParams);
	if (!TestTrue(TEXT("Retained binding authored"), RetainedSet.bSuccess))
	{
		return false;
	}
	const FCortexCommandResult Set = Fixture.Router.Execute(
		TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Property(TEXT("ElapsedValue"))));
	if (!TestTrue(TEXT("Reproduction variable binding authored"), Set.bSuccess))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint.Get());
	TestEqual(TEXT("Initial binding compiles"), Fixture.Blueprint->Status, BS_UpToDate);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("Initial explicit save"),
				  UPackage::SavePackage(Fixture.Blueprint->GetPackage(), Fixture.Blueprint.Get(), *Filename, Args)))
	{
		return false;
	}
	Fixture.Blueprint->GetPackage()->ClearDirtyFlag();
	FString RetainedIdentity;
	FDelegateEditorBinding::StaticStruct()->ExportText(RetainedIdentity, &Fixture.Blueprint->Bindings[0], nullptr,
													   nullptr, PPF_None, nullptr);
	const FCortexCommandResult Clear = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	if (!TestTrue(TEXT("Clear before source retirement"), Clear.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Only unrelated serialized record remains before compile"), Fixture.Blueprint->Bindings.Num(), 1);
	FBlueprintEditorUtils::RemoveMemberVariable(Fixture.Blueprint.Get(), TEXT("ElapsedValue"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint.Get());
	TestEqual(TEXT("Cleared Blueprint compiles after source retirement"), Fixture.Blueprint->Status, BS_UpToDate);
	if (!TestTrue(TEXT("Explicit save of requested state"),
				  UPackage::SavePackage(Fixture.Blueprint->GetPackage(), Fixture.Blueprint.Get(), *Filename, Args)))
	{
		return false;
	}
	Fixture.Blueprint->GetPackage()->ClearDirtyFlag();
	const FString AssetPath = Fixture.Blueprint->GetPathName();
	UWidgetBlueprint* OldBlueprint = Fixture.Blueprint.Get();
	UPackage* Package = Fixture.Blueprint->GetPackage();
	Fixture.Blueprint.Reset();
	Fixture.Progress = nullptr;
	Fixture.Retained = nullptr;
	Fixture.Animation = nullptr;
	if (!TestTrue(TEXT("Fixture package unloads"), UPackageTools::UnloadPackages({Package})))
	{
		return false;
	}
	TestNull(TEXT("Old package no longer in memory"), FindPackage(nullptr, *PackageName));
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		return false;
	}
	UWidgetBlueprint* Reloaded = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!TestNotNull(TEXT("Fresh persisted Blueprint loads"), Reloaded))
	{
		return false;
	}
	Fixture.Blueprint.Reset(Reloaded);
	TestTrue(TEXT("Fresh object instance"), Reloaded != OldBlueprint);
	const TSharedPtr<FJsonObject> State = Fixture.State();
	if (!TestTrue(TEXT("Fresh serialized readback complete"),
				  State.IsValid() && State->GetBoolField(TEXT("reader_complete"))))
	{
		return false;
	}
	TestEqual(TEXT("Retained serialized binding persists"), Reloaded->Bindings.Num(), 1);
	TestEqual(TEXT("Retained target unchanged"), Reloaded->Bindings[0].ObjectName, FString(TEXT("RetainedDisplay")));
	TestEqual(TEXT("Retained source unchanged"), Reloaded->Bindings[0].SourcePath.Segments[0].GetMemberName(),
			  FName(TEXT("RenderOpacity")));
	FString ReloadedIdentity;
	FDelegateEditorBinding::StaticStruct()->ExportText(ReloadedIdentity, &Reloaded->Bindings[0], nullptr, nullptr,
													   PPF_None, nullptr);
	TestEqual(TEXT("Every retained serialized field survives lifecycle"), ReloadedIdentity, RetainedIdentity);
	UProgressBar* Progress = Cast<UProgressBar>(Reloaded->WidgetTree->FindWidget(TEXT("ProgressDisplay")));
	if (!TestNotNull(TEXT("Authored ProgressBar persists"), Progress))
	{
		return false;
	}
	TestEqual(TEXT("Literal default survives"), Progress->GetPercent(), 0.25f);
	TestEqual(TEXT("Style survives"), Progress->GetFillColorAndOpacity(), FLinearColor(0.2f, 0.4f, 0.6f, 1.0f));
	TestEqual(TEXT("Hierarchy survives"), Progress->GetParent()->GetName(), FString(TEXT("Root")));
	TestEqual(TEXT("Animation survives"), Reloaded->Animations.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingPolicyTest,
								 "Cortex.UMG.PropertyBinding.ValidatesPolicyPurityAndNestedOwners",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingPolicyTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
	Nested->SetStringField(TEXT("kind"), TEXT("property"));
	Nested->SetArrayField(TEXT("source_path"),
						  {MakeShared<FJsonValueString>(TEXT("RenderTransform")),
						   MakeShared<FJsonValueString>(TEXT("Translation")), MakeShared<FJsonValueString>(TEXT("X"))});
	const FCortexCommandResult Set = Fixture.Router.Execute(TEXT("umg.set_property_binding"),
															Fixture.WriteParams(MakeShared<FJsonValueObject>(Nested)));
	if (!TestTrue(TEXT("Connected nested struct source accepted"), Set.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("All source segments retained"), Fixture.Blueprint->Bindings[0].SourcePath.Segments.Num(), 3);
	Nested->SetArrayField(TEXT("source_path"),
						  {MakeShared<FJsonValueString>(TEXT("Slot")), MakeShared<FJsonValueString>(TEXT("Parent")),
						   MakeShared<FJsonValueString>(TEXT("RenderOpacity"))});
	TestTrue(TEXT("Connected nested object source accepted"),
			 Fixture.Router
				 .Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(MakeShared<FJsonValueObject>(Nested)))
				 .bSuccess);
	Nested->SetArrayField(TEXT("source_path"),
						  {MakeShared<FJsonValueString>(TEXT("Samples")), MakeShared<FJsonValueString>(TEXT("X"))});
	TestFalse(TEXT("Array intermediate refuses rather than flattening source names"),
			  Fixture.Router
				  .Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(MakeShared<FJsonValueObject>(Nested)))
				  .bSuccess);
	Nested->SetArrayField(TEXT("source_path"), {MakeShared<FJsonValueString>(TEXT("RenderOpacity")),
												MakeShared<FJsonValueString>(TEXT("X"))});
	TestFalse(TEXT("Scalar cannot connect to unrelated owner"),
			  Fixture.Router
				  .Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(MakeShared<FJsonValueObject>(Nested)))
				  .bSuccess);
	UFunction* Function = Fixture.Blueprint->SkeletonGeneratedClass->FindFunctionByName(TEXT("GetRenderOpacity"));
	if (!TestNotNull(TEXT("Compatible function exists"), Function))
	{
		return false;
	}
	const EFunctionFlags Flags = Function->FunctionFlags;
	Function->FunctionFlags &= ~(FUNC_Const | FUNC_BlueprintPure);
	const FCortexCommandResult Impure =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Function()));
	Function->FunctionFlags = Flags;
	TestFalse(TEXT("Signature-compatible impure source refuses"), Impure.bSuccess);
	UClass* Skeleton = Fixture.Blueprint->SkeletonGeneratedClass;
	Fixture.Blueprint->SkeletonGeneratedClass = nullptr;
	const FCortexCommandResult MissingMetadata =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Property()));
	TestTrue(TEXT("No implicit skeleton compilation"), Fixture.Blueprint->SkeletonGeneratedClass == nullptr);
	Fixture.Blueprint->SkeletonGeneratedClass = Skeleton;
	TestFalse(TEXT("Missing source metadata refuses authoring"), MissingMetadata.bSuccess);
	UWidgetEditingProjectSettings* Settings = Fixture.Blueprint->GetRelevantSettings();
	const EPropertyBindingPermissionLevel Policy = Settings->DefaultCompilerOptions.PropertyBindingRule;
	Settings->DefaultCompilerOptions.PropertyBindingRule = EPropertyBindingPermissionLevel::PreventAndError;
	const FCortexCommandResult Forbidden =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Property()));
	const FCortexCommandResult Repair = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	Settings->DefaultCompilerOptions.PropertyBindingRule = Policy;
	TestFalse(TEXT("Disabled binding policy refuses authoring"), Forbidden.bSuccess);
	TestTrue(TEXT("Disabled policy still permits clear repair"), Repair.bSuccess);
	return true;
}
