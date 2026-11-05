#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/StaticMeshComponent.h"
#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPDependentScan.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

namespace
{
	FString ScanPackageFilename(const UBlueprint* Blueprint)
	{
		return FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	}

	bool ScanSaveBlueprint(UBlueprint* Blueprint)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		return UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *ScanPackageFilename(Blueprint), Args);
	}

	FEdGraphPinType ScanObjectPin(UClass* Class)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategoryObject = Class;
		return PinType;
	}

	UK2Node_VariableGet* ScanAddGet(UBlueprint* Blueprint, FName Name, UClass* ExternalParent)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		if (ExternalParent != nullptr)
		{
			Node->VariableReference.SetExternalMember(Name, ExternalParent);
		}
		else
		{
			Node->VariableReference.SetSelfMember(Name);
		}
		Creator.Finalize();
		return Node;
	}

	const FCortexBPDependentHit* ScanFindHit(const TArray<FCortexBPDependentHit>& Hits, const FString& Path)
	{
		return Hits.FindByPredicate([&Path](const FCortexBPDependentHit& Hit) { return Hit.AssetPath == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDependentScanTest,
	"Cortex.Blueprint.DependentScan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPDependentScanTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	const FName Tracked(TEXT("Tracked"));
	FEdGraphPinType BoolPin;
	BoolPin.PinCategory = UEdGraphSchema_K2::PC_Boolean;

	UBlueprint* Owner = MakeBlueprint(TEXT("BP_ScanOwner"), AActor::StaticClass(), TEXT("ScanOwner"));
	if (!TestNotNull(TEXT("Owner"), Owner))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Owner, Tracked, BoolPin);
	FKismetEditorUtilities::CompileBlueprint(Owner);
	UClass* OwnerClass = Owner->GeneratedClass;

	UBlueprint* Child = MakeBlueprint(TEXT("BP_ScanChild"), OwnerClass, TEXT("ScanChild"));
	UBlueprint* Referencer = MakeBlueprint(TEXT("BP_ScanReferencer"), AActor::StaticClass(), TEXT("ScanReferencer"));
	UBlueprint* SameName = MakeBlueprint(TEXT("BP_ScanSameName"), AActor::StaticClass(), TEXT("ScanSameName"));
	UBlueprint* Unrelated = MakeBlueprint(TEXT("BP_ScanUnrelated"), AActor::StaticClass(), TEXT("ScanUnrelated"));
	UBlueprint* Missing = MakeBlueprint(TEXT("BP_ScanMissing"), AActor::StaticClass(), TEXT("ScanMissing"));
	if (!TestNotNull(TEXT("Child"), Child) || !TestNotNull(TEXT("Referencer"), Referencer) || !TestNotNull(TEXT("SameName"), SameName)
		|| !TestNotNull(TEXT("Unrelated"), Unrelated) || !TestNotNull(TEXT("Missing"), Missing))
	{
		return false;
	}

	const FGuid ChildNode = ScanAddGet(Child, Tracked, nullptr)->NodeGuid;
	FBlueprintEditorUtils::AddMemberVariable(Referencer, TEXT("Holder"), ScanObjectPin(OwnerClass));
	const FGuid ReferencerNode = ScanAddGet(Referencer, Tracked, OwnerClass)->NodeGuid;
	FBlueprintEditorUtils::AddMemberVariable(SameName, TEXT("Holder"), ScanObjectPin(OwnerClass));
	// A same-named member of a different, resolvable class.
	UBlueprint* OtherOwner = MakeBlueprint(TEXT("BP_ScanOtherOwner"), AActor::StaticClass(), TEXT("ScanOtherOwner"));
	if (!TestNotNull(TEXT("OtherOwner"), OtherOwner))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(OtherOwner, Tracked, BoolPin);
	FKismetEditorUtilities::CompileBlueprint(OtherOwner);
	const FGuid SameNameNode = ScanAddGet(SameName, Tracked, OtherOwner->GeneratedClass)->NodeGuid;
	ScanAddGet(Unrelated, Tracked, nullptr);
	FBlueprintEditorUtils::AddMemberVariable(Missing, TEXT("Holder"), ScanObjectPin(OwnerClass));

	const TArray<UBlueprint*> All = {Owner, OtherOwner, Child, Referencer, SameName, Unrelated, Missing};
	TArray<FName> AllPackages;
	for (const UBlueprint* Blueprint : All)
	{
		AllPackages.Add(Blueprint->GetOutermost()->GetFName());
	}
	TArray<FString> Filenames;
	for (UBlueprint* Blueprint : All)
	{
		if (!TestTrue(FString::Printf(TEXT("%s saved"), *Blueprint->GetName()), ScanSaveBlueprint(Blueprint)))
		{
			return false;
		}
		Filenames.Add(ScanPackageFilename(Blueprint));
	}
	const FString OwnerPath = Owner->GetPathName();
	const FString ChildPath = Child->GetPathName();
	const FString ReferencerPath = Referencer->GetPathName();
	const FString SameNamePath = SameName->GetPathName();
	const FString UnrelatedPath = Unrelated->GetPathName();
	const FString MissingPath = Missing->GetPathName();
	const FName UnrelatedPackage = Unrelated->GetOutermost()->GetFName();
	const FName MissingPackage = Missing->GetOutermost()->GetFName();

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous(Filenames, true);

	// Unload everything but the owner so the scan has to load what it needs.
	TArray<UPackage*> ToUnload = {Child->GetOutermost(), Referencer->GetOutermost(), SameName->GetOutermost(), Unrelated->GetOutermost(), Missing->GetOutermost()};
	FText UnloadError;
	TestTrue(TEXT("Dependents unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true));

	// The registry still lists this one, but the file is gone.
	IFileManager::Get().Delete(*Filenames.Last(), false, true);

	TArray<FCortexBPDependentHit> Hits;
	FCortexBPDependentScan::Find(Owner, Tracked, FCortexBPDependentScan::EMemberKind::Variable, Hits);

	const FCortexBPDependentHit* ChildHit = ScanFindHit(Hits, ChildPath);
	if (TestNotNull(TEXT("Unloaded child found"), ChildHit))
	{
		TestEqual(TEXT("Child role"), ChildHit->Role, FString(TEXT("child")));
		TestFalse(TEXT("Child was loaded by the scan"), ChildHit->bWasLoaded);
		if (TestEqual(TEXT("Child reference count"), ChildHit->References.Num(), 1))
		{
			TestEqual(TEXT("Child reference node GUID"), ChildHit->References[0].NodeGuid, ChildNode);
			TestEqual(TEXT("Child reference kind"), ChildHit->References[0].Kind, FString(TEXT("k2_variable_node")));
		}
	}

	const FCortexBPDependentHit* ReferencerHit = ScanFindHit(Hits, ReferencerPath);
	if (TestNotNull(TEXT("Unloaded referencer found"), ReferencerHit))
	{
		TestEqual(TEXT("Referencer role"), ReferencerHit->Role, FString(TEXT("referencer")));
		TestFalse(TEXT("Referencer was loaded by the scan"), ReferencerHit->bWasLoaded);
		if (TestEqual(TEXT("Referencer reference count"), ReferencerHit->References.Num(), 1))
		{
			TestEqual(TEXT("Referencer node GUID"), ReferencerHit->References[0].NodeGuid, ReferencerNode);
		}
	}

	const FCortexBPDependentHit* SameNameHit = ScanFindHit(Hits, SameNamePath);
	if (TestNotNull(TEXT("Same-named variable of an unrelated class is reported (conservative)"), SameNameHit))
	{
		if (TestEqual(TEXT("Same-name reference count"), SameNameHit->References.Num(), 1))
		{
			TestEqual(TEXT("Same-name node GUID"), SameNameHit->References[0].NodeGuid, SameNameNode);
			TestEqual(TEXT("Kind stays intact"), SameNameHit->References[0].Kind, FString(TEXT("k2_variable_node")));
		}
	}

	TestNull(TEXT("Unrelated Blueprint is not reported"), ScanFindHit(Hits, UnrelatedPath));
	TestNull(TEXT("Unrelated package was not loaded"), FindPackage(nullptr, *UnrelatedPackage.ToString()));
	TestNull(TEXT("Missing package is skipped, not reported"), ScanFindHit(Hits, MissingPath));
	TestNull(TEXT("Missing package was not loaded"), FindPackage(nullptr, *MissingPackage.ToString()));
	TestNull(TEXT("Owner is never its own dependent"), ScanFindHit(Hits, OwnerPath));

	// Loaded dependents report was_loaded and are found without a second load.
	TArray<FCortexBPDependentHit> SecondHits;
	FCortexBPDependentScan::Find(Owner, Tracked, FCortexBPDependentScan::EMemberKind::Variable, SecondHits);
	const FCortexBPDependentHit* LoadedChild = ScanFindHit(SecondHits, ChildPath);
	if (TestNotNull(TEXT("Child found again"), LoadedChild))
	{
		TestTrue(TEXT("Second scan sees it loaded"), LoadedChild->bWasLoaded);
	}

	// JSON shape.
	if (ChildHit != nullptr)
	{
		const TSharedRef<FJsonObject> Json = ChildHit->ToJson();
		TestEqual(TEXT("json asset_path"), Json->GetStringField(TEXT("asset_path")), ChildPath);
		TestEqual(TEXT("json role"), Json->GetStringField(TEXT("role")), FString(TEXT("child")));
		TestTrue(TEXT("json references"), Json->HasTypedField<EJson::Array>(TEXT("references")));
	}

	// Cleanup: release every loaded package (open file handles), then disk and registry.
	TArray<UPackage*> Loaded;
	for (const FName PackageName : AllPackages)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName.ToString()))
		{
			Loaded.Add(Package);
		}
	}
	FText CleanupError;
	TestTrue(TEXT("Fixture packages unloaded"), UPackageTools::UnloadPackages(Loaded, CleanupError, true));
	for (const FString& Filename : Filenames)
	{
		IFileManager::Get().Delete(*Filename, false, true);
	}
	Registry.ScanModifiedAssetFiles(Filenames);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSCSCollisionCandidateParentTest,
	"Cortex.Blueprint.SCSCollisionCandidateParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSCSCollisionCandidateParentTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("BP_ScsCandidate"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	// An SCS node named like a property of the candidate parent.
	USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Speed"));
	Blueprint->SimpleConstructionScript->AddNode(Node);

	UClass* Candidate = ACortexBPAdoptParent::StaticClass();
	const TArray<FCortexBPSCSDiagnostics::FCollision> Without = FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint);
	TestEqual(TEXT("No candidate: today's result (no collision against AActor)"), Without.Num(), 0);

	const TArray<FCortexBPSCSDiagnostics::FCollision> AgainstCandidate = FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint, Candidate);
	TestEqual(TEXT("Candidate parent produces the collision"), AgainstCandidate.Num(), 1);
	TestTrue(TEXT("Blueprint parent untouched by the query"), Blueprint->ParentClass == AActor::StaticClass());

	UClass* OriginalParent = Blueprint->ParentClass;
	Blueprint->ParentClass = Candidate;
	const TArray<FCortexBPSCSDiagnostics::FCollision> AfterAssign = FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint);
	Blueprint->ParentClass = OriginalParent;

	if (TestEqual(TEXT("Same collision count after assigning the parent"), AfterAssign.Num(), AgainstCandidate.Num()) && AgainstCandidate.Num() == 1)
	{
		const FCortexBPSCSDiagnostics::FCollision& A = AgainstCandidate[0];
		const FCortexBPSCSDiagnostics::FCollision& B = AfterAssign[0];
		TestEqual(TEXT("SCS node name"), A.SCSNodeName, B.SCSNodeName);
		TestTrue(TEXT("Component class"), A.SCSComponentClass == B.SCSComponentClass);
		TestTrue(TEXT("Inherited property"), A.InheritedProperty == B.InheritedProperty);
		TestTrue(TEXT("Inherited from class"), A.InheritedFromClass == B.InheritedFromClass);
		TestTrue(TEXT("Inherited from the candidate"), A.InheritedFromClass == Candidate);
		TestTrue(TEXT("Kind"), A.Kind == B.Kind);
		TestTrue(TEXT("Severity"), A.Severity == B.Severity);
	}

	// A null candidate is the no-candidate form.
	TestEqual(TEXT("Explicit null candidate equals the default form"),
		FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint, nullptr).Num(), Without.Num());

	Discard(Blueprint);
	return true;
}
