// blueprint.export_snapshot writes a deterministic, sorted description of a Blueprint (variables, CDO with
// declaring class, components, widgets, bindings, animations, text, interfaces, graph entries) to out_path
// and answers with a compact summary only.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPSnapshotTestSupport.h"
#include "CortexBPSnapshotTestTypes.h"
#include "CortexTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexBPTypeUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	bool AreKeysSorted(const TArray<TSharedPtr<FJsonValue>>& Records)
	{
		for (int32 Index = 1; Index < Records.Num(); ++Index)
		{
			const FString Prev = Records[Index - 1]->AsObject()->GetStringField(TEXT("key"));
			const FString Next = Records[Index]->AsObject()->GetStringField(TEXT("key"));
			if (Prev.Compare(Next, ESearchCase::CaseSensitive) > 0)
			{
				return false;
			}
		}
		return true;
	}

	/** Widget Blueprint with a variable TextBlock "Caption", a text binding and an animation "Intro" bound to it. */
	UBlueprint* MakeWidgetFixture(const FString& Suffix, FString& OutAssetPath)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), FString::Printf(TEXT("WBP_Snapshot_%s"), *Suffix));
		CreateParams->SetStringField(TEXT("path"), FString::Printf(TEXT("/Game/Temp/CortexSnapshot_%s"), *Suffix));
		CreateParams->SetStringField(TEXT("type"), TEXT("Widget"));
		const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		if (!CreateResult.bSuccess || !CreateResult.Data.IsValid() || !CreateResult.Data->TryGetStringField(TEXT("asset_path"), OutAssetPath))
		{
			return nullptr;
		}
		UBlueprint* Widget = LoadObject<UBlueprint>(nullptr, *OutAssetPath);
		if (!Widget)
		{
			return nullptr;
		}

		const FObjectProperty* WidgetTreeProperty = CastField<FObjectProperty>(Widget->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		UObject* WidgetTree = WidgetTreeProperty ? WidgetTreeProperty->GetObjectPropertyValue_InContainer(Widget) : nullptr;
		UClass* TextBlockClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.TextBlock"));
		UClass* AnimationClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetAnimation"));
		if (!WidgetTree || !TextBlockClass || !AnimationClass)
		{
			return nullptr;
		}

		UObject* Caption = NewObject<UObject>(WidgetTree, TextBlockClass, TEXT("Caption"), RF_Transient);
		const FBoolProperty* IsVariable = CastField<FBoolProperty>(TextBlockClass->FindPropertyByName(TEXT("bIsVariable")));
		if (!Caption || !IsVariable)
		{
			return nullptr;
		}
		IsVariable->SetPropertyValue_InContainer(Caption, true);

		// Animation "Intro" with one binding to Caption.
		UObject* Animation = NewObject<UObject>(Widget, AnimationClass, TEXT("Intro"));
		const FArrayProperty* BindingsProperty = CastField<FArrayProperty>(AnimationClass->FindPropertyByName(TEXT("AnimationBindings")));
		const FStructProperty* BindingStruct = BindingsProperty ? CastField<FStructProperty>(BindingsProperty->Inner) : nullptr;
		const FNameProperty* BindingWidgetName = BindingStruct
			? CastField<FNameProperty>(BindingStruct->Struct->FindPropertyByName(TEXT("WidgetName"))) : nullptr;
		if (!Animation || !BindingsProperty || !BindingWidgetName)
		{
			return nullptr;
		}
		{
			FScriptArrayHelper Bindings(BindingsProperty, BindingsProperty->ContainerPtrToValuePtr<void>(Animation));
			const int32 Index = Bindings.AddValue();
			BindingWidgetName->SetPropertyValue_InContainer(Bindings.GetRawPtr(Index), FName(TEXT("Caption")));
		}
		const FArrayProperty* AnimationsProperty = CastField<FArrayProperty>(Widget->GetClass()->FindPropertyByName(TEXT("Animations")));
		const FObjectProperty* AnimationInner = AnimationsProperty ? CastField<FObjectProperty>(AnimationsProperty->Inner) : nullptr;
		if (!AnimationInner)
		{
			return nullptr;
		}
		{
			FScriptArrayHelper Animations(AnimationsProperty, AnimationsProperty->ContainerPtrToValuePtr<void>(Widget));
			const int32 Index = Animations.AddValue();
			AnimationInner->SetObjectPropertyValue(Animations.GetRawPtr(Index), Animation);
		}

		// Text binding Caption.Text -> GetCaptionText.
		const FArrayProperty* WidgetBindingsProperty = CastField<FArrayProperty>(Widget->GetClass()->FindPropertyByName(TEXT("Bindings")));
		const FStructProperty* WidgetBindingStruct = WidgetBindingsProperty ? CastField<FStructProperty>(WidgetBindingsProperty->Inner) : nullptr;
		if (!WidgetBindingStruct)
		{
			return nullptr;
		}
		const FStrProperty* ObjectName = CastField<FStrProperty>(WidgetBindingStruct->Struct->FindPropertyByName(TEXT("ObjectName")));
		const FNameProperty* PropertyName = CastField<FNameProperty>(WidgetBindingStruct->Struct->FindPropertyByName(TEXT("PropertyName")));
		const FNameProperty* FunctionName = CastField<FNameProperty>(WidgetBindingStruct->Struct->FindPropertyByName(TEXT("FunctionName")));
		if (!ObjectName || !PropertyName || !FunctionName)
		{
			return nullptr;
		}
		FScriptArrayHelper WidgetBindings(WidgetBindingsProperty, WidgetBindingsProperty->ContainerPtrToValuePtr<void>(Widget));
		const int32 BindingIndex = WidgetBindings.AddValue();
		ObjectName->SetPropertyValue_InContainer(WidgetBindings.GetRawPtr(BindingIndex), TEXT("Caption"));
		PropertyName->SetPropertyValue_InContainer(WidgetBindings.GetRawPtr(BindingIndex), FName(TEXT("Text")));
		FunctionName->SetPropertyValue_InContainer(WidgetBindings.GetRawPtr(BindingIndex), FName(TEXT("GetCaptionText")));
		return Widget;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPExportSnapshotTest,
	"Cortex.Blueprint.ExportSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPExportSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPSnapshotTestSupport;

	const FString Suffix = NewSuffix();
	TStrongObjectPtr<UBlueprint> Actor(MakeBlueprint(FString::Printf(TEXT("CortexSnapshot_%s"), *Suffix), AActor::StaticClass()));
	if (!TestNotNull(TEXT("actor Blueprint created"), Actor.Get()))
	{
		return false;
	}

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FEdGraphPinType TextType;
	TextType.PinCategory = UEdGraphSchema_K2::PC_Text;
	if (!TestTrue(TEXT("variables added"),
		FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("Health"), IntType)
		&& FBlueprintEditorUtils::AddMemberVariable(Actor.Get(), TEXT("Title"), TextType)))
	{
		Retire(Actor.Get());
		return false;
	}
	for (FBPVariableDescription& Variable : Actor->NewVariables)
	{
		if (Variable.VarName == TEXT("Health"))
		{
			Variable.DefaultValue = TEXT("5");
		}
	}
	FBlueprintEditorUtils::SetBlueprintVariableCategory(Actor.Get(), TEXT("Health"), nullptr, FText::FromString(TEXT("Stats")));

	// SCS component with one override against its class default.
	USCS_Node* MeshNode = Actor->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Mesh"));
	Actor->SimpleConstructionScript->AddNode(MeshNode);
	CastChecked<UStaticMeshComponent>(MeshNode->ComponentTemplate)->bHiddenInGame = true;

	FBlueprintEditorUtils::ImplementNewInterface(Actor.Get(), FTopLevelAssetPath(UCortexBPSnapshotTestInterface::StaticClass()));

	UEdGraph* EventGraph = Actor->UbergraphPages.Num() > 0 ? Actor->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		Retire(Actor.Get());
		return false;
	}
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
	Event->CustomFunctionName = TEXT("OnSnapshot");
	EventGraph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	FKismetEditorUtilities::CompileBlueprint(Actor.Get());
	UClass* GeneratedClass = Actor->GeneratedClass;
	FTextProperty* TitleProperty = GeneratedClass ? CastField<FTextProperty>(GeneratedClass->FindPropertyByName(TEXT("Title"))) : nullptr;
	if (!TestTrue(TEXT("compiled class carries Title"), TitleProperty != nullptr))
	{
		Retire(Actor.Get());
		return false;
	}
	TitleProperty->SetPropertyValue_InContainer(GeneratedClass->GetDefaultObject(), FText::AsCultureInvariant(TEXT("Snapshot Title")));

	// Full export.
	const FString OutPath = OutputPath(TEXT("actor"), Suffix);
	const FCortexCommandResult Exported = Export(Actor->GetPathName(), OutPath);
	if (!TestTrue(*FString::Printf(TEXT("export_snapshot succeeds (%s: %s)"), *Exported.ErrorCode, *Exported.ErrorMessage),
		Exported.bSuccess && Exported.Data.IsValid()))
	{
		Retire(Actor.Get());
		return false;
	}
	TestFalse(TEXT("response carries no section bodies"), Exported.Data->HasField(TEXT("sections")));
	TestFalse(TEXT("response carries no variables body"), Exported.Data->HasField(TEXT("variables")));
	TestTrue(TEXT("response names the file"),
		Exported.Data->GetStringField(TEXT("out_path")).EndsWith(FString::Printf(TEXT("actor_%s.json"), *Suffix)));
	const TSharedPtr<FJsonObject>* ResponseCounts = nullptr;
	if (TestTrue(TEXT("response carries per-section counts"), Exported.Data->TryGetObjectField(TEXT("counts"), ResponseCounts)))
	{
		double Variables = -1.0;
		TestTrue(TEXT("variables count present"), (*ResponseCounts)->TryGetNumberField(TEXT("variables"), Variables));
		TestEqual(TEXT("two variables counted"), static_cast<int32>(Variables), 2);
	}

	FString FirstText;
	const TSharedPtr<FJsonObject> Snapshot = ReadJsonFile(OutPath, &FirstText);
	if (TestTrue(TEXT("snapshot file parses"), Snapshot.IsValid()))
	{
		TestEqual(TEXT("format_version"), static_cast<int32>(Snapshot->GetNumberField(TEXT("format_version"))), 1);
		TestEqual(TEXT("asset_path"), Snapshot->GetStringField(TEXT("asset_path")), Actor->GetPathName());
		TestEqual(TEXT("parent_class"), Snapshot->GetStringField(TEXT("parent_class")), AActor::StaticClass()->GetPathName());
		TestFalse(TEXT("engine_version recorded"), Snapshot->GetStringField(TEXT("engine_version")).IsEmpty());

		const TSharedPtr<FJsonObject>* Sections = nullptr;
		if (TestTrue(TEXT("sections object"), Snapshot->TryGetObjectField(TEXT("sections"), Sections)))
		{
			for (const TCHAR* Name : {TEXT("variables"), TEXT("cdo"), TEXT("components"), TEXT("widgets"), TEXT("bindings"),
				TEXT("animations"), TEXT("text"), TEXT("interfaces"), TEXT("graph_entries")})
			{
				const TArray<TSharedPtr<FJsonValue>>* Records = SectionRecords(Snapshot, Name);
				if (TestNotNull(*FString::Printf(TEXT("section %s present"), Name), Records))
				{
					TestTrue(*FString::Printf(TEXT("section %s sorted by key"), Name), AreKeysSorted(*Records));
				}
			}
		}

		const TSharedPtr<FJsonObject> Health = FindRecord(Snapshot, TEXT("variables"), TEXT("Health"));
		if (TestTrue(TEXT("Health variable record"), Health.IsValid()))
		{
			TestEqual(TEXT("Health type"), Health->GetStringField(TEXT("type")), CortexBPTypeUtils::FriendlyTypeName(IntType));
			// A full compile copies the default into the CDO and clears the editor-side value (KismetCompiler), so the
			// record carries the field and the CDO record below carries the value.
			TestTrue(TEXT("Health default field present"), Health->HasField(TEXT("default")));
			TestEqual(TEXT("Health category"), Health->GetStringField(TEXT("category")), FString(TEXT("Stats")));
		}

		const TSharedPtr<FJsonObject> HealthCdo = FindRecord(Snapshot, TEXT("cdo"), TEXT("Health"));
		if (TestTrue(TEXT("Health CDO record"), HealthCdo.IsValid()))
		{
			TestEqual(TEXT("Health declared in the generated class"),
				HealthCdo->GetStringField(TEXT("declared_in")), GeneratedClass->GetPathName());
			TestEqual(TEXT("Health CDO value"), static_cast<int32>(HealthCdo->GetNumberField(TEXT("value"))), 5);
		}
		const TSharedPtr<FJsonObject> LifeSpan = FindRecord(Snapshot, TEXT("cdo"), TEXT("InitialLifeSpan"));
		if (TestTrue(TEXT("inherited InitialLifeSpan CDO record"), LifeSpan.IsValid()))
		{
			TestEqual(TEXT("InitialLifeSpan declared in Actor"),
				LifeSpan->GetStringField(TEXT("declared_in")), AActor::StaticClass()->GetPathName());
		}

		const TSharedPtr<FJsonObject> Mesh = FindRecord(Snapshot, TEXT("components"), TEXT("scs:Mesh"));
		if (TestTrue(TEXT("SCS component record"), Mesh.IsValid()))
		{
			TestEqual(TEXT("component class"), Mesh->GetStringField(TEXT("class_path")), UStaticMeshComponent::StaticClass()->GetPathName());
			const TSharedPtr<FJsonObject>* Overrides = nullptr;
			if (TestTrue(TEXT("component overrides object"), Mesh->TryGetObjectField(TEXT("overrides"), Overrides)))
			{
				TestTrue(TEXT("bHiddenInGame override recorded"), (*Overrides)->HasField(TEXT("bHiddenInGame")));
			}
		}

		TestTrue(TEXT("interface record"),
			FindRecord(Snapshot, TEXT("interfaces"), UCortexBPSnapshotTestInterface::StaticClass()->GetPathName()).IsValid());

		const TSharedPtr<FJsonObject> Title = FindRecord(Snapshot, TEXT("text"), TEXT("class_default|CDO|Title"));
		if (TestTrue(TEXT("text entry reuses the text inventory collector"), Title.IsValid()))
		{
			TestEqual(TEXT("text value"), Title->GetStringField(TEXT("value")), FString(TEXT("Snapshot Title")));
			TestEqual(TEXT("text source kind"), Title->GetStringField(TEXT("source_kind")), FString(TEXT("culture_invariant")));
		}

		const TSharedPtr<FJsonObject> EventEntry = FindRecord(Snapshot, TEXT("graph_entries"),
			FString::Printf(TEXT("%s|custom_event|OnSnapshot"), *EventGraph->GetName()));
		if (TestTrue(TEXT("custom event entry"), EventEntry.IsValid()))
		{
			TestEqual(TEXT("event node guid"), EventEntry->GetStringField(TEXT("node_guid")),
				Event->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}

		TestEqual(TEXT("an actor has no widgets"), SectionRecords(Snapshot, TEXT("widgets"))->Num(), 0);
		TestEqual(TEXT("an actor has no bindings"), SectionRecords(Snapshot, TEXT("bindings"))->Num(), 0);
		TestEqual(TEXT("an actor has no animations"), SectionRecords(Snapshot, TEXT("animations"))->Num(), 0);
	}

	// Deterministic: a second export is byte identical.
	const FString SecondPath = OutputPath(TEXT("actor_again"), Suffix);
	const FCortexCommandResult Again = Export(Actor->GetPathName(), SecondPath);
	TestTrue(TEXT("second export succeeds"), Again.bSuccess);
	FString SecondText;
	TestTrue(TEXT("second file readable"), FFileHelper::LoadFileToString(SecondText, *SecondPath));
	TestEqual(TEXT("two exports are identical"), SecondText, FirstText);

	// Section subset writes only the requested sections.
	const FString SubsetPath = OutputPath(TEXT("subset"), Suffix);
	const TArray<FString> Subset = {TEXT("variables"), TEXT("interfaces")};
	const FCortexCommandResult SubsetResult = Export(Actor->GetPathName(), SubsetPath, &Subset);
	TestTrue(TEXT("subset export succeeds"), SubsetResult.bSuccess);
	const TSharedPtr<FJsonObject> SubsetSnapshot = ReadJsonFile(SubsetPath);
	if (TestTrue(TEXT("subset file parses"), SubsetSnapshot.IsValid()))
	{
		const TSharedPtr<FJsonObject>* Sections = nullptr;
		if (TestTrue(TEXT("subset sections object"), SubsetSnapshot->TryGetObjectField(TEXT("sections"), Sections)))
		{
			TestEqual(TEXT("only requested sections written"), (*Sections)->Values.Num(), 2);
			TestTrue(TEXT("variables written"), (*Sections)->HasField(TEXT("variables")));
			TestTrue(TEXT("interfaces written"), (*Sections)->HasField(TEXT("interfaces")));
		}
	}

	// Unknown section is refused before anything is written.
	const FString BogusPath = OutputPath(TEXT("bogus"), Suffix);
	const TArray<FString> Bogus = {TEXT("variables"), TEXT("not_a_section")};
	const FCortexCommandResult BogusResult = Export(Actor->GetPathName(), BogusPath, &Bogus);
	TestFalse(TEXT("unknown section refused"), BogusResult.bSuccess);
	TestEqual(TEXT("unknown section error code"), BogusResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("unknown section is named"), BogusResult.ErrorMessage.Contains(TEXT("not_a_section")));
	TestFalse(TEXT("nothing written for unknown section"), IFileManager::Get().FileExists(*BogusPath));

	// out_path outside the allowed roots is refused with the safe-file contract's code.
	const FString OutsidePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Source"), FString::Printf(TEXT("CortexSnapshotOutside_%s.json"), *Suffix)));
	const FCortexCommandResult Outside = Export(Actor->GetPathName(), OutsidePath);
	TestFalse(TEXT("out_path outside allowed roots refused"), Outside.bSuccess);
	TestEqual(TEXT("outside refusal code"), Outside.ErrorCode, CortexErrorCodes::InvalidFilePath);
	TestFalse(TEXT("nothing written outside allowed roots"), IFileManager::Get().FileExists(*OutsidePath));

	// Missing out_path and unknown asset.
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> NoOut = MakeShared<FJsonObject>();
	NoOut->SetStringField(TEXT("asset_path"), Actor->GetPathName());
	const FCortexCommandResult NoOutResult = Handler.Execute(TEXT("export_snapshot"), NoOut);
	TestFalse(TEXT("missing out_path refused"), NoOutResult.bSuccess);
	TestEqual(TEXT("missing out_path code"), NoOutResult.ErrorCode, CortexErrorCodes::InvalidField);
	const FCortexCommandResult Missing = Export(TEXT("/Game/Temp/CortexSnapshot_DoesNotExist/BP_Missing"), OutputPath(TEXT("missing"), Suffix));
	TestFalse(TEXT("unknown asset fails"), Missing.bSuccess);
	TestEqual(TEXT("unknown asset error code"), Missing.ErrorCode, CortexErrorCodes::AssetNotFound);

	// Widget Blueprint: widgets, bindings and animations by reflection.
	FString WidgetAssetPath;
	UBlueprint* Widget = MakeWidgetFixture(Suffix, WidgetAssetPath);
	if (TestNotNull(TEXT("widget fixture"), Widget))
	{
		const FString WidgetOut = OutputPath(TEXT("widget"), Suffix);
		const FCortexCommandResult WidgetResult = Export(WidgetAssetPath, WidgetOut);
		TestTrue(*FString::Printf(TEXT("widget export succeeds (%s)"), *WidgetResult.ErrorMessage), WidgetResult.bSuccess);
		const TSharedPtr<FJsonObject> WidgetSnapshot = ReadJsonFile(WidgetOut);
		if (TestTrue(TEXT("widget snapshot parses"), WidgetSnapshot.IsValid()))
		{
			const TSharedPtr<FJsonObject> Caption = FindRecord(WidgetSnapshot, TEXT("widgets"), TEXT("Caption"));
			if (TestTrue(TEXT("Caption widget record"), Caption.IsValid()))
			{
				TestEqual(TEXT("widget class"), Caption->GetStringField(TEXT("class_path")), FString(TEXT("/Script/UMG.TextBlock")));
				TestTrue(TEXT("widget is a variable"), Caption->GetBoolField(TEXT("is_variable")));
				TestTrue(TEXT("widget parent field present"), Caption->HasField(TEXT("parent")));
			}
			const TSharedPtr<FJsonObject> Binding = FindRecord(WidgetSnapshot, TEXT("bindings"), TEXT("Caption.Text"));
			if (TestTrue(TEXT("text binding record"), Binding.IsValid()))
			{
				TestEqual(TEXT("bound function"), Binding->GetStringField(TEXT("bound_function")), FString(TEXT("GetCaptionText")));
			}
			const TSharedPtr<FJsonObject> Intro = FindRecord(WidgetSnapshot, TEXT("animations"), TEXT("Intro"));
			if (TestTrue(TEXT("Intro animation record"), Intro.IsValid()))
			{
				const TArray<TSharedPtr<FJsonValue>>& Bound = Intro->GetArrayField(TEXT("bound_widget_names"));
				if (TestEqual(TEXT("one bound widget"), Bound.Num(), 1))
				{
					TestEqual(TEXT("bound widget name"), Bound[0]->AsString(), FString(TEXT("Caption")));
				}
			}
		}
		IFileManager::Get().Delete(*WidgetOut);
		Retire(Widget);
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"),
			FString::Printf(TEXT("CortexSnapshot_%s"), *Suffix)), false, true);
	}

	IFileManager::Get().Delete(*OutPath);
	IFileManager::Get().Delete(*SecondPath);
	IFileManager::Get().Delete(*SubsetPath);
	Retire(Actor.Get());
	return true;
}
