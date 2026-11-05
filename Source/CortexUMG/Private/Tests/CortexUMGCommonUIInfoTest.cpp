// umg.get_commonui_info reports CommonUI activatable settings, implemented overrides, button / text /
// border styles and style Blueprint assets, resolving every CommonUI class dynamically.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	UBlueprint* CreateCommonUIBlueprint(
		UClass* ParentClass, UClass* BlueprintClass, UClass* GeneratedClassType, const TCHAR* Prefix)
	{
		UPackage* Package = CreatePackage(*(FString(TEXT("/Temp/")) + Prefix +
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FName Name = FName(*FPackageName::GetLongPackageAssetName(Package->GetName()));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, Name, BPTYPE_Normal, BlueprintClass, GeneratedClassType, NAME_None);
		if (Blueprint)
		{
			Blueprint->SetFlags(RF_Transactional);
		}
		return Blueprint;
	}

	UWidgetBlueprint* CreateCommonUIWidgetBlueprint(UClass* ParentClass, const TCHAR* Prefix)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(CreateCommonUIBlueprint(
			ParentClass, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), Prefix));
		if (Blueprint && Blueprint->WidgetTree)
		{
			Blueprint->WidgetTree->RootWidget =
				Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		}
		return Blueprint;
	}

	void RetireCommonUIBlueprint(UBlueprint* Blueprint)
	{
		if (Blueprint)
		{
			UPackage* Package = Blueprint->GetPackage();
			ResetLoaders(Package);
			Package->ClearDirtyFlag();
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Package->MarkAsGarbage();
		}
	}

	TSharedPtr<FJsonObject> FindRecord(const TSharedPtr<FJsonObject>& Data, const TCHAR* Array, const FString& WidgetName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(Array, Records))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Records)
		{
			const TSharedPtr<FJsonObject> Record = Value->AsObject();
			if (Record.IsValid() && Record->GetStringField(TEXT("widget_name")) == WidgetName)
			{
				return Record;
			}
		}
		return nullptr;
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGCommonUIInfoTest,
	"Cortex.UMG.CommonUIInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGCommonUIInfoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UClass* ActivatableClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonActivatableWidget"));
	UClass* ButtonBaseClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonButtonBase"));
	UClass* ButtonStyleClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonButtonStyle"));
	UClass* TextBlockClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonTextBlock"));
	UClass* BorderClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonBorder"));
	if (!ActivatableClass || !ButtonBaseClass || !ButtonStyleClass || !TextBlockClass || !BorderClass)
	{
		AddInfo(TEXT("Skipping CommonUI info test: CommonUI classes are not loaded (plugin disabled)"));
		return true;
	}

	TArray<UBlueprint*> Created;
	auto RetireAll = [&Created]()
	{
		for (UBlueprint* Blueprint : Created)
		{
			RetireCommonUIBlueprint(Blueprint);
		}
	};

	// Style Blueprint (CommonButtonStyle descendant).
	UBlueprint* StyleBlueprint = CreateCommonUIBlueprint(
		ButtonStyleClass, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CortexCommonStyle_"));
	if (!TestNotNull(TEXT("style Blueprint created"), StyleBlueprint))
	{
		return false;
	}
	Created.Add(StyleBlueprint);
	FKismetEditorUtilities::CompileBlueprint(StyleBlueprint);
	UClass* StyleGeneratedClass = StyleBlueprint->GeneratedClass;
	if (!TestNotNull(TEXT("style generated class"), StyleGeneratedClass))
	{
		RetireAll();
		return false;
	}

	// CommonButtonBase-derived Widget Blueprint whose CDO points at the style Blueprint class.
	UWidgetBlueprint* ButtonBlueprint = CreateCommonUIWidgetBlueprint(ButtonBaseClass, TEXT("CortexCommonButton_"));
	if (!TestNotNull(TEXT("button Blueprint created"), ButtonBlueprint))
	{
		RetireAll();
		return false;
	}
	Created.Add(ButtonBlueprint);
	FKismetEditorUtilities::CompileBlueprint(ButtonBlueprint);
	UClass* ButtonGeneratedClass = ButtonBlueprint->GeneratedClass;
	if (!TestNotNull(TEXT("button generated class"), ButtonGeneratedClass))
	{
		RetireAll();
		return false;
	}
	// Held strongly: Blueprint compiles run GC, and the table is only referenced from raw property writes later.
	TStrongObjectPtr<UDataTable> ActionTableHolder(NewObject<UDataTable>(GetTransientPackage(),
		FName(*FString::Printf(TEXT("CortexCommonActions_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))));
	UDataTable* ActionTable = ActionTableHolder.Get();
	auto ApplyButtonValues = [StyleBlueprint, ActionTable](UObject* Target) -> bool
	{
		FClassProperty* StyleProperty = CastField<FClassProperty>(Target->GetClass()->FindPropertyByName(TEXT("Style")));
		FStructProperty* TriggeringProperty =
			CastField<FStructProperty>(Target->GetClass()->FindPropertyByName(TEXT("TriggeringInputAction")));
		if (!StyleProperty || !TriggeringProperty || TriggeringProperty->Struct != FDataTableRowHandle::StaticStruct())
		{
			return false;
		}
		StyleProperty->SetObjectPropertyValue_InContainer(Target, StyleBlueprint->GeneratedClass);
		FDataTableRowHandle* Handle = TriggeringProperty->ContainerPtrToValuePtr<FDataTableRowHandle>(Target);
		Handle->DataTable = ActionTable;
		Handle->RowName = TEXT("Confirm");
		return true;
	};

	// CommonActivatableWidget-derived screen with two buttons, a text block, a border and two overrides.
	UWidgetBlueprint* Screen = CreateCommonUIWidgetBlueprint(ActivatableClass, TEXT("CortexCommonScreen_"));
	if (!TestNotNull(TEXT("screen Blueprint created"), Screen))
	{
		RetireAll();
		return false;
	}
	Created.Add(Screen);

	UCanvasPanel* Root = Cast<UCanvasPanel>(Screen->WidgetTree->RootWidget);
	Root->AddChild(Screen->WidgetTree->ConstructWidget<UUserWidget>(ButtonGeneratedClass, TEXT("Confirm")));
	Root->AddChild(Screen->WidgetTree->ConstructWidget<UUserWidget>(ButtonGeneratedClass, TEXT("Cancel")));
	Root->AddChild(Screen->WidgetTree->ConstructWidget<UWidget>(TextBlockClass, TEXT("Caption")));
	Root->AddChild(Screen->WidgetTree->ConstructWidget<UWidget>(BorderClass, TEXT("Frame")));

	// BP_OnHandleBackAction / BP_GetDesiredInputConfig have return values, so they are function overrides;
	// BP_OnDeactivated is an event override.
	for (const TCHAR* FunctionName : {TEXT("BP_OnHandleBackAction"), TEXT("BP_GetDesiredInputConfig")})
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Screen, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph(Screen, Graph, /*bIsUserCreated=*/false, ActivatableClass);
	}
	UEdGraph* EventGraph = Screen->UbergraphPages.Num() > 0 ? Screen->UbergraphPages[0] : nullptr;
	if (!EventGraph)
	{
		EventGraph = FBlueprintEditorUtils::CreateNewGraph(
			Screen, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Screen, EventGraph);
	}
	UK2Node_Event* DeactivatedEvent = NewObject<UK2Node_Event>(EventGraph);
	DeactivatedEvent->EventReference.SetExternalMember(TEXT("BP_OnDeactivated"), ActivatableClass);
	DeactivatedEvent->bOverrideFunction = true;
	EventGraph->AddNode(DeactivatedEvent, false, false);
	DeactivatedEvent->CreateNewGuid();
	DeactivatedEvent->PostPlacedNewNode();
	DeactivatedEvent->AllocateDefaultPins();

	FKismetEditorUtilities::CompileBlueprint(Screen);
	UObject* ScreenCDO = Screen->GeneratedClass ? Screen->GeneratedClass->GetDefaultObject() : nullptr;
	FBoolProperty* BackHandlerProperty = Screen->GeneratedClass
		? CastField<FBoolProperty>(Screen->GeneratedClass->FindPropertyByName(TEXT("bIsBackHandler"))) : nullptr;
	if (!TestTrue(TEXT("screen CDO exposes bIsBackHandler"), ScreenCDO && BackHandlerProperty))
	{
		RetireAll();
		return false;
	}
	BackHandlerProperty->SetPropertyValue_InContainer(ScreenCDO, true);

	// Applied after every compile: compiling the screen recompiles its dependencies and resets their defaults.
	bool bButtonValuesApplied = true;
	TArray<UObject*> ButtonTargets;
	ButtonTargets.Add(ButtonBlueprint->GeneratedClass->GetDefaultObject());
	ButtonTargets.Add(Screen->WidgetTree->FindWidget(TEXT("Confirm")));
	ButtonTargets.Add(Screen->WidgetTree->FindWidget(TEXT("Cancel")));
	for (UObject* Target : ButtonTargets)
	{
		bButtonValuesApplied &= Target && ApplyButtonValues(Target);
	}
	if (!TestTrue(TEXT("button values applied to the button CDO and both instances"), bButtonValuesApplied))
	{
		RetireAll();
		return false;
	}

	UWidgetBlueprint* Plain = CreateCommonUIWidgetBlueprint(UUserWidget::StaticClass(), TEXT("CortexCommonPlain_"));
	if (!TestNotNull(TEXT("plain Blueprint created"), Plain))
	{
		RetireAll();
		return false;
	}
	Created.Add(Plain);
	FKismetEditorUtilities::CompileBlueprint(Plain);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"), MakeShared<FCortexUMGCommandHandler>());

	// Activatable screen.
	TSharedPtr<FJsonObject> ScreenParams = MakeShared<FJsonObject>();
	ScreenParams->SetStringField(TEXT("asset_path"), Screen->GetPathName());
	const FCortexCommandResult ScreenResult = Router.Execute(TEXT("umg.get_commonui_info"), ScreenParams);
	if (TestTrue(TEXT("screen info succeeds"), ScreenResult.bSuccess && ScreenResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject>& Data = ScreenResult.Data;
		TestTrue(TEXT("commonui_available"), Data->GetBoolField(TEXT("commonui_available")));
		TestEqual(TEXT("asset_kind is widget"), Data->GetStringField(TEXT("asset_kind")), FString(TEXT("widget")));
		TestTrue(TEXT("commonui_bases lists CommonActivatableWidget"),
			StringArray(Data, TEXT("commonui_bases")).Contains(ActivatableClass->GetPathName()));

		const TSharedPtr<FJsonObject>* Activatable = nullptr;
		if (TestTrue(TEXT("activatable object"), Data->TryGetObjectField(TEXT("activatable"), Activatable)))
		{
			bool bBackHandler = false;
			TestTrue(TEXT("activatable carries bIsBackHandler"),
				(*Activatable)->TryGetBoolField(TEXT("bIsBackHandler"), bBackHandler));
			TestTrue(TEXT("bIsBackHandler reads the CDO value"), bBackHandler);
		}

		const TArray<FString> Overrides = StringArray(Data, TEXT("overrides"));
		TestTrue(TEXT("BP_OnHandleBackAction override listed"), Overrides.Contains(TEXT("BP_OnHandleBackAction")));
		TestTrue(TEXT("BP_GetDesiredInputConfig override listed"), Overrides.Contains(TEXT("BP_GetDesiredInputConfig")));
		TestTrue(TEXT("BP_OnDeactivated event override listed"), Overrides.Contains(TEXT("BP_OnDeactivated")));
		TestFalse(TEXT("unimplemented BP_OnActivated absent"), Overrides.Contains(TEXT("BP_OnActivated")));
		TestFalse(TEXT("unimplemented BP_GetDesiredFocusTarget absent"), Overrides.Contains(TEXT("BP_GetDesiredFocusTarget")));

		const TSharedPtr<FJsonObject> Confirm = FindRecord(Data, TEXT("buttons"), TEXT("Confirm"));
		const TSharedPtr<FJsonObject> Cancel = FindRecord(Data, TEXT("buttons"), TEXT("Cancel"));
		if (TestTrue(TEXT("both buttons reported"), Confirm.IsValid() && Cancel.IsValid()))
		{
			TestEqual(TEXT("button style is the style Blueprint class"),
				Confirm->GetStringField(TEXT("style")), StyleGeneratedClass->GetPathName());
			TestEqual(TEXT("button style asset is the style Blueprint"),
				Confirm->GetStringField(TEXT("style_asset")), StyleBlueprint->GetPathName());
			const TSharedPtr<FJsonObject>* Action = nullptr;
			if (TestTrue(TEXT("triggering_input_action object"),
				Confirm->TryGetObjectField(TEXT("triggering_input_action"), Action)))
			{
				TestEqual(TEXT("action table"), (*Action)->GetStringField(TEXT("table")), ActionTable->GetPathName());
				TestEqual(TEXT("action row"), (*Action)->GetStringField(TEXT("row")), FString(TEXT("Confirm")));
			}
		}

		const TArray<FString> StyleAssets = StringArray(Data, TEXT("style_assets"));
		TestEqual(TEXT("style_assets lists the style Blueprint exactly once"),
			StyleAssets.FilterByPredicate([&](const FString& Path) { return Path == StyleBlueprint->GetPathName(); }).Num(), 1);
		TestEqual(TEXT("style_assets holds only the shared style"), StyleAssets.Num(), 1);

		const TSharedPtr<FJsonObject> Caption = FindRecord(Data, TEXT("text_widgets"), TEXT("Caption"));
		TestTrue(TEXT("text widget reported"), Caption.IsValid());
		const TSharedPtr<FJsonObject> Frame = FindRecord(Data, TEXT("border_widgets"), TEXT("Frame"));
		TestTrue(TEXT("border widget reported"), Frame.IsValid());
	}

	// A Blueprint parented to a CommonUI button is itself the button: its CDO is read.
	TSharedPtr<FJsonObject> ButtonParams = MakeShared<FJsonObject>();
	ButtonParams->SetStringField(TEXT("asset_path"), ButtonBlueprint->GetPathName());
	const FCortexCommandResult ButtonResult = Router.Execute(TEXT("umg.get_commonui_info"), ButtonParams);
	if (TestTrue(TEXT("button Blueprint info succeeds"), ButtonResult.bSuccess && ButtonResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> Self = FindRecord(ButtonResult.Data, TEXT("buttons"), ButtonBlueprint->GetName());
		if (TestTrue(TEXT("button Blueprint reports itself"), Self.IsValid()))
		{
			TestTrue(TEXT("self record flagged"), Self->GetBoolField(TEXT("is_self")));
			TestEqual(TEXT("self style read from the CDO"),
				Self->GetStringField(TEXT("style")), StyleBlueprint->GeneratedClass->GetPathName());
		}
	}

	// Style Blueprint asset.
	TSharedPtr<FJsonObject> StyleParams = MakeShared<FJsonObject>();
	StyleParams->SetStringField(TEXT("asset_path"), StyleBlueprint->GetPathName());
	const FCortexCommandResult StyleResult = Router.Execute(TEXT("umg.get_commonui_info"), StyleParams);
	if (TestTrue(TEXT("style info succeeds"), StyleResult.bSuccess && StyleResult.Data.IsValid()))
	{
		TestEqual(TEXT("asset_kind is style"), StyleResult.Data->GetStringField(TEXT("asset_kind")), FString(TEXT("style")));
		TestEqual(TEXT("style_kind is button"), StyleResult.Data->GetStringField(TEXT("style_kind")), FString(TEXT("button")));
	}

	// Plain UserWidget Blueprint: empty CommonUI fields, no error.
	TSharedPtr<FJsonObject> PlainParams = MakeShared<FJsonObject>();
	PlainParams->SetStringField(TEXT("asset_path"), Plain->GetPathName());
	const FCortexCommandResult PlainResult = Router.Execute(TEXT("umg.get_commonui_info"), PlainParams);
	if (TestTrue(TEXT("plain info succeeds"), PlainResult.bSuccess && PlainResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject>& Data = PlainResult.Data;
		TestEqual(TEXT("plain asset_kind is widget"), Data->GetStringField(TEXT("asset_kind")), FString(TEXT("widget")));
		TestEqual(TEXT("plain has no CommonUI bases"), StringArray(Data, TEXT("commonui_bases")).Num(), 0);
		TestEqual(TEXT("plain has no overrides"), StringArray(Data, TEXT("overrides")).Num(), 0);
		TestEqual(TEXT("plain has no style assets"), StringArray(Data, TEXT("style_assets")).Num(), 0);
		TestEqual(TEXT("plain has no buttons"), Data->GetArrayField(TEXT("buttons")).Num(), 0);
		const TSharedPtr<FJsonObject>* Activatable = nullptr;
		if (TestTrue(TEXT("plain activatable object present"), Data->TryGetObjectField(TEXT("activatable"), Activatable)))
		{
			TestEqual(TEXT("plain activatable is empty"), (*Activatable)->Values.Num(), 0);
		}
	}

	// Unknown asset.
	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexCommonUI_DoesNotExist/WBP_Missing"));
	const FCortexCommandResult Missing = Router.Execute(TEXT("umg.get_commonui_info"), MissingParams);
	TestFalse(TEXT("unknown asset fails"), Missing.bSuccess);
	TestEqual(TEXT("unknown asset error code"), Missing.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	RetireAll();
	return true;
}
