#include "Misc/AutomationTest.h"

#include "Components/ActorComponent.h"
#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPParentRules.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPParentRulesResolveTest,
	"Cortex.Blueprint.ParentRules.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPParentRulesResolveTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UClass* Resolved = nullptr;
	FCortexCommandResult Error;

	TestTrue(TEXT("Full class path resolves"), FCortexBPClassResolver::Resolve(TEXT("/Script/Engine.Actor"), Resolved, Error));
	TestTrue(TEXT("Full path gives AActor"), Resolved == AActor::StaticClass());

	Resolved = nullptr;
	TestTrue(TEXT("Prefixed short name resolves"), FCortexBPClassResolver::Resolve(TEXT("ACortexBPAdoptParent"), Resolved, Error));
	TestTrue(TEXT("Prefixed short name gives the native class"), Resolved == ACortexBPAdoptParent::StaticClass());

	Resolved = nullptr;
	TestTrue(TEXT("Unprefixed short name resolves"), FCortexBPClassResolver::Resolve(TEXT("CortexBPAdoptOtherActorParent"), Resolved, Error));
	TestTrue(TEXT("Unprefixed short name gives the native class"), Resolved == ACortexBPAdoptOtherActorParent::StaticClass());

	Resolved = nullptr;
	TestFalse(TEXT("Unknown class fails"), FCortexBPClassResolver::Resolve(TEXT("CortexBPNoSuchClassAnywhere"), Resolved, Error));
	TestEqual(TEXT("Unknown short name code"), Error.ErrorCode, CortexErrorCodes::ClassNotFound);
	TestFalse(TEXT("Unknown full path fails"), FCortexBPClassResolver::Resolve(TEXT("/Script/Engine.CortexBPNoSuchClassAnywhere"), Resolved, Error));
	TestEqual(TEXT("Unknown full path code"), Error.ErrorCode, CortexErrorCodes::ClassNotFound);

	// Blueprint asset path resolves to its generated class.
	UBlueprint* Single = MakeBlueprint(TEXT("ParentRulesSingle"), AActor::StaticClass());
	if (TestNotNull(TEXT("Single Blueprint"), Single))
	{
		Resolved = nullptr;
		TestTrue(TEXT("Blueprint asset path resolves"), FCortexBPClassResolver::Resolve(Single->GetPathName(), Resolved, Error));
		TestTrue(TEXT("Asset path gives generated class"), Resolved == Single->GeneratedClass);
	}

	// Two Blueprints with the same asset name in different packages.
	UBlueprint* DupA = MakeBlueprint(TEXT("ParentRulesDup"), AActor::StaticClass(), TEXT("ParentRulesDupPkgA"));
	UBlueprint* DupB = MakeBlueprint(TEXT("ParentRulesDup"), AActor::StaticClass(), TEXT("ParentRulesDupPkgB"));
	if (TestNotNull(TEXT("DupA"), DupA) && TestNotNull(TEXT("DupB"), DupB))
	{
		Resolved = nullptr;
		TestFalse(TEXT("Duplicate short name is ambiguous"), FCortexBPClassResolver::Resolve(TEXT("ParentRulesDup"), Resolved, Error));
		TestEqual(TEXT("Ambiguous code"), Error.ErrorCode, CortexErrorCodes::AmbiguousClass);
		TestTrue(TEXT("Nothing resolved"), Resolved == nullptr);
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (TestTrue(TEXT("Details list candidates"),
			Error.ErrorDetails.IsValid() && Error.ErrorDetails->TryGetArrayField(TEXT("candidates"), Candidates)))
		{
			TArray<FString> Paths;
			for (const TSharedPtr<FJsonValue>& Value : *Candidates)
			{
				Paths.Add(Value->AsString());
			}
			TestEqual(TEXT("Two candidates"), Paths.Num(), 2);
			TestTrue(TEXT("Candidate A listed"), Paths.Contains(DupA->GeneratedClass->GetPathName()));
			TestTrue(TEXT("Candidate B listed"), Paths.Contains(DupB->GeneratedClass->GetPathName()));
		}

		// A full asset path of either still resolves unambiguously.
		Resolved = nullptr;
		TestTrue(TEXT("Asset path disambiguates"), FCortexBPClassResolver::Resolve(DupB->GetPathName(), Resolved, Error));
		TestTrue(TEXT("Disambiguated to B"), Resolved == DupB->GeneratedClass);
	}

	// A Blueprint named like a native class makes the short name ambiguous with the native class.
	UBlueprint* NativeTwin = MakeBlueprint(TEXT("CortexBPAdoptParent"), AActor::StaticClass());
	if (TestNotNull(TEXT("Native twin Blueprint"), NativeTwin))
	{
		Resolved = nullptr;
		TestFalse(TEXT("Blueprint named like a native class is ambiguous"),
			FCortexBPClassResolver::Resolve(TEXT("CortexBPAdoptParent"), Resolved, Error));
		TestEqual(TEXT("Native twin code"), Error.ErrorCode, CortexErrorCodes::AmbiguousClass);
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (TestTrue(TEXT("Native twin details"),
			Error.ErrorDetails.IsValid() && Error.ErrorDetails->TryGetArrayField(TEXT("candidates"), Candidates)))
		{
			TArray<FString> Paths;
			for (const TSharedPtr<FJsonValue>& Value : *Candidates)
			{
				Paths.Add(Value->AsString());
			}
			TestTrue(TEXT("Native class listed"), Paths.Contains(ACortexBPAdoptParent::StaticClass()->GetPathName()));
			TestTrue(TEXT("Blueprint class listed"), Paths.Contains(NativeTwin->GeneratedClass->GetPathName()));
		}
	}

	// A saved but unloaded Blueprint's generated class path (/Game/..._C) loads the Blueprint and resolves.
	UBlueprint* OnDisk = MakeBlueprint(TEXT("BP_ParentRulesOnDisk"), AActor::StaticClass());
	if (TestNotNull(TEXT("On-disk Blueprint"), OnDisk))
	{
		UPackage* Package = OnDisk->GetOutermost();
		const FString PackageName = Package->GetName();
		const FString ClassPath = FString::Printf(TEXT("%s.%s_C"), *PackageName, *OnDisk->GetName());
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		if (TestTrue(TEXT("On-disk Blueprint saved"), UPackage::SavePackage(Package, OnDisk, *Filename, SaveArgs)))
		{
			FText UnloadError;
			TestTrue(TEXT("On-disk Blueprint unloaded"), UPackageTools::UnloadPackages({Package}, UnloadError, true));
			TestNull(TEXT("Class is no longer in memory"), FindObject<UClass>(nullptr, *ClassPath));
			Resolved = nullptr;
			TestTrue(TEXT("Unloaded /Game/..._C class path resolves"), FCortexBPClassResolver::Resolve(ClassPath, Resolved, Error));
			TestNotNull(TEXT("Resolved class"), Resolved);
			if (Resolved != nullptr)
			{
				TestTrue(TEXT("Resolved to the Blueprint's generated class"), Resolved->GetPathName() == ClassPath);
			}
			if (UPackage* Reloaded = FindPackage(nullptr, *PackageName))
			{
				UPackageTools::UnloadPackages({Reloaded}, UnloadError, true);
			}
			IFileManager::Get().Delete(*Filename, false, true);
		}
		else
		{
			Discard(OnDisk);
		}
	}

	for (UBlueprint* Blueprint : { Single, DupA, DupB, NativeTwin })
	{
		Discard(Blueprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPParentRulesFamilyTest,
	"Cortex.Blueprint.ParentRules.Family",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPParentRulesFamilyTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	struct FCase
	{
		const TCHAR* Label;
		UClass* BlueprintParent;
		UClass* BlueprintClass;
		UClass* Target;
		bool bExpectOk;
		const TCHAR* ExpectedFrom;
		const TCHAR* ExpectedTo;
		const TCHAR* ExpectedReason;
	};

	UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));

	TArray<FCase> Cases = {
		{TEXT("Actor->Actor"), AActor::StaticClass(), UBlueprint::StaticClass(), ACortexBPAdoptParent::StaticClass(), true, TEXT("Actor"), TEXT("Actor"), nullptr},
		{TEXT("Actor->Component"), AActor::StaticClass(), UBlueprint::StaticClass(), UActorComponent::StaticClass(), false, TEXT("Actor"), TEXT("Component"), TEXT("family_mismatch")},
		{TEXT("Object->Actor"), UObject::StaticClass(), UBlueprint::StaticClass(), AActor::StaticClass(), false, TEXT("Object"), TEXT("Actor"), TEXT("family_mismatch")},
		{TEXT("Component->Actor"), UActorComponent::StaticClass(), UBlueprint::StaticClass(), AActor::StaticClass(), false, TEXT("Component"), TEXT("Actor"), TEXT("family_mismatch")},
		{TEXT("Object->non-Blueprintable"), UObject::StaticClass(), UBlueprint::StaticClass(), UCortexBPTestSubobjPayload::StaticClass(), false, TEXT("Object"), TEXT("Object"), TEXT("not_blueprintable")},
	};
	if (UserWidgetClass != nullptr && WidgetBlueprintClass != nullptr)
	{
		Cases.Add({TEXT("Widget->Actor"), UserWidgetClass, WidgetBlueprintClass, AActor::StaticClass(), false, TEXT("Widget"), TEXT("Actor"), TEXT("family_mismatch")});
		Cases.Add({TEXT("Widget->Widget"), UserWidgetClass, WidgetBlueprintClass, UserWidgetClass, true, TEXT("Widget"), TEXT("Widget"), nullptr});
	}
	else
	{
		AddInfo(TEXT("UMG not available; skipping widget family cases"));
	}

	for (const FCase& Case : Cases)
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("ParentRulesFamily"), Case.BlueprintParent, nullptr, Case.BlueprintClass);
		if (!TestNotNull(Case.Label, Blueprint))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Details;
		const bool bOk = FCortexBPFamily::CheckReparent(Blueprint, Case.Target, Details);
		TestEqual(FString::Printf(TEXT("%s verdict"), Case.Label), bOk, Case.bExpectOk);
		FString From;
		FString To;
		FString Reason;
		TestTrue(TEXT("Details exist"), Details.IsValid());
		if (Details.IsValid())
		{
			Details->TryGetStringField(TEXT("from_family"), From);
			Details->TryGetStringField(TEXT("to_family"), To);
			Details->TryGetStringField(TEXT("reason"), Reason);
		}
		TestEqual(FString::Printf(TEXT("%s from_family"), Case.Label), From, FString(Case.ExpectedFrom));
		TestEqual(FString::Printf(TEXT("%s to_family"), Case.Label), To, FString(Case.ExpectedTo));
		if (!Case.bExpectOk)
		{
			TestEqual(FString::Printf(TEXT("%s reason"), Case.Label), Reason, FString(Case.ExpectedReason));
		}
		Discard(Blueprint);
	}

	// Own generated class and its descendants are refused.
	UBlueprint* Parent = MakeBlueprint(TEXT("ParentRulesCycleParent"), AActor::StaticClass());
	if (TestNotNull(TEXT("Cycle parent"), Parent) && TestNotNull(TEXT("Parent generated class"), Parent->GeneratedClass.Get()))
	{
		UBlueprint* Child = MakeBlueprint(TEXT("ParentRulesCycleChild"), Parent->GeneratedClass);
		TSharedPtr<FJsonObject> Details;
		TestFalse(TEXT("Own generated class refused"), FCortexBPFamily::CheckReparent(Parent, Parent->GeneratedClass, Details));
		FString Reason;
		Details->TryGetStringField(TEXT("reason"), Reason);
		TestEqual(TEXT("Own class reason"), Reason, FString(TEXT("self_or_descendant")));
		if (TestNotNull(TEXT("Child Blueprint"), Child) && TestNotNull(TEXT("Child generated class"), Child->GeneratedClass.Get()))
		{
			TestFalse(TEXT("Descendant refused"), FCortexBPFamily::CheckReparent(Parent, Child->GeneratedClass, Details));
			Details->TryGetStringField(TEXT("reason"), Reason);
			TestEqual(TEXT("Descendant reason"), Reason, FString(TEXT("self_or_descendant")));
		}
		Discard(Child);
	}
	Discard(Parent);
	return true;
}
