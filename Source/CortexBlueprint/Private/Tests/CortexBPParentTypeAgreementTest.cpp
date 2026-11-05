#include "Misc/AutomationTest.h"
#include "CortexBPList119TestUtils.h"
#include "Operations/CortexBPAssetOps.h"
#include "GameFramework/Actor.h"

// Issue #119: blueprint.list, get_info, analyze_for_migration and create must agree on `type`,
// `parent_class` and `native_parent_class` for the same Blueprint.
namespace
{
	struct FIdentityFacts
	{
		FString Type;
		FString ParentClass;
		FString NativeParentClass;
	};

	FIdentityFacts ReadFacts(const TSharedPtr<FJsonObject>& Object)
	{
		return {
			CortexBPList119Test::RowString(Object, TEXT("type")),
			CortexBPList119Test::RowString(Object, TEXT("parent_class")),
			CortexBPList119Test::RowString(Object, TEXT("native_parent_class"))};
	}

	void ExpectAgreement(
		FAutomationTestBase& Test,
		FCortexBPCommandHandler& Handler,
		const FString& Dir,
		const FString& Name,
		const FString& ExpectedType,
		const FString& ExpectedParent,
		const FString& ExpectedNative)
	{
		const FCortexCommandResult List = CortexBPList119Test::RunList(Handler, Dir, FString(), nullptr);
		const TSharedPtr<FJsonObject> Row = CortexBPList119Test::FindRow(List, Name);
		if (!Test.TestTrue(FString::Printf(TEXT("%s list row"), *Name), Row.IsValid())) { return; }

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *Dir, *Name));
		const FCortexCommandResult Info = Handler.Execute(TEXT("get_info"), Params);
		const FCortexCommandResult Analysis = Handler.Execute(TEXT("analyze_for_migration"), Params);
		if (!Test.TestTrue(TEXT("get_info succeeds"), Info.bSuccess) || !Test.TestTrue(TEXT("analyze succeeds"), Analysis.bSuccess))
		{
			return;
		}

		const FIdentityFacts ListFacts = ReadFacts(Row);
		const FIdentityFacts InfoFacts = ReadFacts(Info.Data);
		const FIdentityFacts AnalysisFacts = ReadFacts(Analysis.Data);

		Test.TestEqual(TEXT("list type"), ListFacts.Type, ExpectedType);
		Test.TestEqual(TEXT("get_info type matches list"), InfoFacts.Type, ListFacts.Type);
		Test.TestEqual(TEXT("analyze type matches list"), AnalysisFacts.Type, ListFacts.Type);

		Test.TestEqual(TEXT("list parent_class"), ListFacts.ParentClass, ExpectedParent);
		Test.TestEqual(TEXT("get_info parent_class matches list"), InfoFacts.ParentClass, ListFacts.ParentClass);
		Test.TestEqual(TEXT("analyze parent_class matches list"), AnalysisFacts.ParentClass, ListFacts.ParentClass);

		Test.TestEqual(TEXT("list native_parent_class"), ListFacts.NativeParentClass, ExpectedNative);
		Test.TestEqual(TEXT("get_info native_parent_class matches list"), InfoFacts.NativeParentClass, ListFacts.NativeParentClass);
		Test.TestEqual(TEXT("analyze native_parent_class matches list"), AnalysisFacts.NativeParentClass, ListFacts.NativeParentClass);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPParentTypeAgreementBlueprintChildTest,
	"Cortex.Blueprint.ParentTypeAgreement.BlueprintChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPParentTypeAgreementBlueprintChildTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("Agreement"));
	const FString ParentName = TEXT("BP_AG_Parent");
	const FString ChildName = TEXT("BP_AG_Child");
	const FString ParentGeneratedClass = FString::Printf(TEXT("%s/%s.%s_C"), *Dir, *ParentName, *ParentName);
	TestTrue(TEXT("create parent BP"), CreateBlueprint(Handler, Dir, ParentName, TEXT("Actor"), FString()));
	TestTrue(TEXT("create child BP"), CreateBlueprint(Handler, Dir, ChildName, FString(), ParentGeneratedClass));

	ExpectAgreement(*this, Handler, Dir, ChildName, TEXT("Actor"), ParentName, TEXT("Actor"));
	ExpectAgreement(*this, Handler, Dir, ParentName, TEXT("Actor"), TEXT("Actor"), TEXT("Actor"));

	DeleteBlueprint(Handler, Dir, ChildName);
	DeleteBlueprint(Handler, Dir, ParentName);
	RemoveTestDirectory(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPParentTypeAgreementWidgetTest,
	"Cortex.Blueprint.ParentTypeAgreement.Widget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPParentTypeAgreementWidgetTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("AgreementWidget"));
	const FString Name = TEXT("WBP_AG_Widget");

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), Name);
	CreateParams->SetStringField(TEXT("path"), Dir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Widget"));
	const FCortexCommandResult Created = Handler.Execute(TEXT("create"), CreateParams);
	if (!TestTrue(TEXT("create Widget BP"), Created.bSuccess)) { return false; }
	TestEqual(TEXT("create parent_class uses the shared display name"), RowString(Created.Data, TEXT("parent_class")), TEXT("UserWidget"));

	ExpectAgreement(*this, Handler, Dir, Name, TEXT("Widget"), TEXT("UserWidget"), TEXT("UserWidget"));

	DeleteBlueprint(Handler, Dir, Name);
	RemoveTestDirectory(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPParentTypeAgreementClassTagTest,
	"Cortex.Blueprint.ParentTypeAgreement.ClassTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPParentTypeAgreementClassTagTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("None tag yields no path"), FCortexBPAssetOps::ClassTagToObjectPath(TEXT("None")), FString());
	TestEqual(TEXT("Empty tag yields no path"), FCortexBPAssetOps::ClassTagToObjectPath(FString()), FString());
	TestEqual(TEXT("Export-text tag yields the object path"),
		FCortexBPAssetOps::ClassTagToObjectPath(TEXT("Class'/Script/Engine.Actor'")), TEXT("/Script/Engine.Actor"));
	TestEqual(TEXT("Plain path is kept"),
		FCortexBPAssetOps::ClassTagToObjectPath(TEXT("/Script/Engine.Actor")), TEXT("/Script/Engine.Actor"));

	TestEqual(TEXT("Native class keeps its name"), FCortexBPAssetOps::ParentClassDisplayName(AActor::StaticClass()), TEXT("Actor"));
	TestEqual(TEXT("Null class is empty"), FCortexBPAssetOps::ParentClassDisplayName(nullptr), FString());
	TestEqual(TEXT("Native ancestor of a native class is itself"),
		FCortexBPAssetOps::NativeParentClassName(AActor::StaticClass()), TEXT("Actor"));
	return true;
}
