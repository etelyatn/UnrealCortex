#pragma once

#include "Misc/PackageName.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/ProgressBar.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/StrongObjectPtr.h"

struct FCortexUMGPropertyBindingFixture
{
	TStrongObjectPtr<UWidgetBlueprint> Blueprint;
	FCortexCommandRouter Router;
	UProgressBar* Progress = nullptr;
	UProgressBar* Retained = nullptr;
	UWidgetAnimation* Animation = nullptr;

	explicit FCortexUMGPropertyBindingFixture(const FString& PackageName = FString())
	{
		UPackage* Package = CreatePackage(*(PackageName.IsEmpty() ? TEXT("/Temp/CortexPropertyBinding_") +
																		FGuid::NewGuid().ToString(EGuidFormats::Digits)
																  : PackageName));
		const FName Name = FName(*FPackageName::GetLongPackageAssetName(Package->GetName()));
		Blueprint.Reset(CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), Package, Name, BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None)));
		Blueprint->SetFlags(RF_Transactional);
		UCanvasPanel* Root =
			Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		Blueprint->WidgetTree->RootWidget = Root;
		Progress =
			Blueprint->WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("ProgressDisplay"));
		Retained =
			Blueprint->WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("RetainedDisplay"));
		Root->AddChild(Progress);
		Root->AddChild(Retained);
		FEdGraphPinType FloatType;
		FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
		FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("ElapsedValue"), FloatType);
		FEdGraphPinType ArrayType = FloatType;
		ArrayType.ContainerType = EPinContainerType::Array;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("Samples"), ArrayType);
		UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint.Get(), TEXT("GetElapsedPercent"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint.Get(), FunctionGraph, true, nullptr);
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Entry->AddExtraFlags(FUNC_BlueprintPure | FUNC_Const);
				UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
				UEdGraphPin* Return = Result->CreateUserDefinedPin(TEXT("ReturnValue"), FloatType, EGPD_Input);
				Return->DefaultValue = TEXT("0.35");
				GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
					Entry->FindPinChecked(UEdGraphSchema_K2::PN_Then),
					Result->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
				break;
			}
		}
		Progress->SetPercent(0.25f);
		Progress->SetFillColorAndOpacity(FLinearColor(0.2f, 0.4f, 0.6f, 1.0f));
		Animation = NewObject<UWidgetAnimation>(Blueprint.Get(), TEXT("RetainedAnimation"), RF_Transactional);
		Animation->MovieScene = NewObject<UMovieScene>(Animation, TEXT("RetainedAnimation"), RF_Transactional);
		Animation->MovieScene->SetPlaybackRange(0, 24);
		FWidgetAnimationBinding AnimationBinding;
		AnimationBinding.WidgetName = TEXT("RetainedDisplay");
		AnimationBinding.AnimationGuid =
			Animation->MovieScene->AddPossessable(Retained->GetName(), Retained->GetClass());
		Animation->AnimationBindings.Add(AnimationBinding);
		Blueprint->Animations.Add(Animation);
		Blueprint->WidgetVariableNameToGuidMap.Add(Animation->GetFName(), FGuid::NewGuid());
		FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
		Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"), MakeShared<FCortexUMGCommandHandler>());
	}

	~FCortexUMGPropertyBindingFixture()
	{
		if (Blueprint.IsValid())
		{
			ResetLoaders(Blueprint->GetPackage());
			Blueprint->GetPackage()->ClearDirtyFlag();
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Blueprint->GetPackage()->MarkAsGarbage();
		}
	}

	TSharedPtr<FJsonObject> ReadParams(bool bWidget = false) const
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetBoolField(TEXT("include_property_bindings"), true);
		if (bWidget)
		{
			Params->SetStringField(TEXT("widget_name"), TEXT("ProgressDisplay"));
		}
		return Params;
	}

	TSharedPtr<FJsonObject> State() const
	{
		const FCortexCommandResult Read =
			const_cast<FCortexCommandRouter&>(Router).Execute(TEXT("umg.get_tree"), ReadParams());
		const TSharedPtr<FJsonObject>* StateObject = nullptr;
		return Read.bSuccess && Read.Data.IsValid() &&
					   Read.Data->TryGetObjectField(TEXT("property_binding_state"), StateObject)
				   ? *StateObject
				   : nullptr;
	}

	TSharedPtr<FJsonObject> WriteParams(const TSharedPtr<FJsonValue>& Binding = MakeShared<FJsonValueNull>()) const
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("widget_name"), TEXT("ProgressDisplay"));
		Params->SetStringField(TEXT("property_name"), TEXT("Percent"));
		Params->SetField(TEXT("binding"), Binding);
		const TSharedPtr<FJsonObject> Current = State();
		if (Current.IsValid())
		{
			Params->SetObjectField(TEXT("expected_fingerprint"), Current->GetObjectField(TEXT("fingerprint")));
		}
		return Params;
	}

	static TSharedPtr<FJsonValue> Property(const TCHAR* Member = TEXT("RenderOpacity"))
	{
		TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("kind"), TEXT("property"));
		Binding->SetArrayField(TEXT("source_path"), {MakeShared<FJsonValueString>(Member)});
		return MakeShared<FJsonValueObject>(Binding);
	}

	static TSharedPtr<FJsonValue> Function(const TCHAR* Name = TEXT("GetRenderOpacity"))
	{
		TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("kind"), TEXT("function"));
		Binding->SetStringField(TEXT("function_name"), Name);
		return MakeShared<FJsonValueObject>(Binding);
	}

	void Seed()
	{
		FDelegateEditorBinding Binding;
		Binding.ObjectName = TEXT("ProgressDisplay");
		Binding.PropertyName = TEXT("Percent");
		Binding.SourceProperty = TEXT("ElapsedValue");
		Binding.MemberGuid = FGuid::NewGuid();
		Binding.FunctionName = TEXT("MissingSourceGetter");
		Blueprint->Bindings.Add(Binding);
		Binding.ObjectName = TEXT("RetainedDisplay");
		Blueprint->Bindings.Add(Binding);
		Blueprint->GetPackage()->ClearDirtyFlag();
	}
};
