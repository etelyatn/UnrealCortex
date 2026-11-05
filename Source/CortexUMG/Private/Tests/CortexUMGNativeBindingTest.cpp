// umg.check_native_bindings classifies every BindWidget / BindWidgetAnim member of a native widget
// base against a Widget Blueprint's designer tree and animations, using the engine's own predicates.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "CortexUMGBindWidgetTestTypes.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieScene.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	TSharedPtr<FJsonObject> FindBinding(const FCortexCommandResult& Result, const FString& Kind, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("bindings"), Bindings))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Bindings)
		{
			const TSharedPtr<FJsonObject> Binding = Value->AsObject();
			FString BindingKind;
			FString BindingName;
			if (Binding.IsValid() && Binding->TryGetStringField(TEXT("kind"), BindingKind) &&
				Binding->TryGetStringField(TEXT("name"), BindingName) && BindingKind == Kind && BindingName == Name)
			{
				return Binding;
			}
		}
		return nullptr;
	}

	FString JoinedBindingKeys(const FCortexCommandResult& Result)
	{
		FString Keys;
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("bindings"), Bindings))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Bindings)
			{
				const TSharedPtr<FJsonObject> Binding = Value->AsObject();
				Keys += Binding->GetStringField(TEXT("kind")) + TEXT(":") + Binding->GetStringField(TEXT("name")) +
					TEXT("=") + Binding->GetStringField(TEXT("status")) + TEXT(";");
			}
		}
		return Keys;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGCheckNativeBindingsTest,
	"Cortex.UMG.CheckNativeBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGCheckNativeBindingsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UPackage* Package = CreatePackage(*(TEXT("/Temp/CortexUMGNativeBinding_") +
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UWidgetBlueprint> Blueprint(NewObject<UWidgetBlueprint>(
		Package, TEXT("WBP_CortexNativeBinding"), RF_Public | RF_Standalone | RF_Transactional));
	Blueprint->ParentClass = UUserWidget::StaticClass();
	Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint.Get(), TEXT("WidgetTree"));
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	Root->AddChild(Blueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("MatchedImage")));
	Root->AddChild(Blueprint->WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("WrongType")));
	Root->AddChild(Blueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("MissingImage")));

	UWidgetAnimation* FadeIn = NewObject<UWidgetAnimation>(Blueprint.Get(), TEXT("FadeIn"), RF_Transactional);
	FadeIn->MovieScene = NewObject<UMovieScene>(FadeIn, TEXT("FadeIn"), RF_Transactional);
	Blueprint->Animations.Add(FadeIn);

	const FString AssetPath = Blueprint->GetPathName();
	const FString NativeClassPath = UCortexUMGBindWidgetTestBase::StaticClass()->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("native_class"), NativeClassPath);
	const FCortexCommandResult Result = Router.Execute(TEXT("umg.check_native_bindings"), Params);
	if (TestTrue(TEXT("check_native_bindings succeeds"), Result.bSuccess && Result.Data.IsValid()))
	{
		auto ExpectStatus = [&](const FString& Kind, const FString& Name, const FString& Status)
			-> TSharedPtr<FJsonObject>
		{
			const TSharedPtr<FJsonObject> Binding = FindBinding(Result, Kind, Name);
			if (!TestTrue(FString::Printf(TEXT("%s %s is listed"), *Kind, *Name), Binding.IsValid()))
			{
				return nullptr;
			}
			TestEqual(FString::Printf(TEXT("%s %s status"), *Kind, *Name),
				Binding->GetStringField(TEXT("status")), Status);
			return Binding;
		};

		ExpectStatus(TEXT("widget"), TEXT("MatchedImage"), TEXT("matched"));

		if (const TSharedPtr<FJsonObject> Wrong = ExpectStatus(TEXT("widget"), TEXT("WrongType"), TEXT("type_mismatch")))
		{
			TestEqual(TEXT("WrongType expected_class"), Wrong->GetStringField(TEXT("expected_class")), FString(TEXT("Overlay")));
			TestEqual(TEXT("WrongType actual_class"), Wrong->GetStringField(TEXT("actual_class")), FString(TEXT("HorizontalBox")));
			TestFalse(TEXT("WrongType is required"), Wrong->GetBoolField(TEXT("optional")));
		}

		if (const TSharedPtr<FJsonObject> Missing = ExpectStatus(TEXT("widget"), TEXT("MissingImag"), TEXT("missing")))
		{
			TestEqual(TEXT("MissingImag name_suggestion"),
				Missing->GetStringField(TEXT("name_suggestion")), FString(TEXT("MissingImage")));
			TestEqual(TEXT("MissingImag expected_class"),
				Missing->GetStringField(TEXT("expected_class")), FString(TEXT("Image")));
		}

		if (const TSharedPtr<FJsonObject> Optional =
				ExpectStatus(TEXT("widget"), TEXT("OptionalImage"), TEXT("optional_missing")))
		{
			TestTrue(TEXT("OptionalImage is optional"), Optional->GetBoolField(TEXT("optional")));
		}

		ExpectStatus(TEXT("animation"), TEXT("FadeIn"), TEXT("matched"));
		ExpectStatus(TEXT("animation"), TEXT("NotTransientAnim"), TEXT("not_transient"));

		TestTrue(TEXT("has_errors"), Result.Data->GetBoolField(TEXT("has_errors")));
		const TSharedPtr<FJsonObject> Counts = Result.Data->GetObjectField(TEXT("counts"));
		TestEqual(TEXT("matched count"), static_cast<int32>(Counts->GetNumberField(TEXT("matched"))), 2);
		TestEqual(TEXT("type_mismatch count"), static_cast<int32>(Counts->GetNumberField(TEXT("type_mismatch"))), 1);
		TestEqual(TEXT("missing count"), static_cast<int32>(Counts->GetNumberField(TEXT("missing"))), 1);
		TestEqual(TEXT("optional_missing count"), static_cast<int32>(Counts->GetNumberField(TEXT("optional_missing"))), 1);
		TestEqual(TEXT("not_transient count"), static_cast<int32>(Counts->GetNumberField(TEXT("not_transient"))), 1);

		// Sorted by kind, then name; the same call yields the same order.
		TestEqual(TEXT("bindings are sorted by kind then name"), JoinedBindingKeys(Result),
			FString(TEXT("animation:FadeIn=matched;animation:NotTransientAnim=not_transient;")
				TEXT("widget:MatchedImage=matched;widget:MissingImag=missing;")
				TEXT("widget:OptionalImage=optional_missing;widget:WrongType=type_mismatch;")));
	}

	// Without native_class the Blueprint's own parent class is checked.
	Blueprint->ParentClass = UCortexUMGBindWidgetTestBase::StaticClass();
	TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
	DefaultParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult DefaultResult = Router.Execute(TEXT("umg.check_native_bindings"), DefaultParams);
	TestTrue(TEXT("default parent check succeeds"), DefaultResult.bSuccess);
	TestEqual(TEXT("default check matches the explicit native_class check"),
		JoinedBindingKeys(DefaultResult), JoinedBindingKeys(Result));

	// A parent with no BindWidget members reports an empty, error-free result.
	Blueprint->ParentClass = UUserWidget::StaticClass();
	const FCortexCommandResult PlainResult = Router.Execute(TEXT("umg.check_native_bindings"), DefaultParams);
	if (TestTrue(TEXT("plain parent check succeeds"), PlainResult.bSuccess && PlainResult.Data.IsValid()))
	{
		TestFalse(TEXT("plain parent has no errors"), PlainResult.Data->GetBoolField(TEXT("has_errors")));
		TestEqual(TEXT("plain parent has no bindings"), JoinedBindingKeys(PlainResult), FString());
	}

	TSharedPtr<FJsonObject> UnknownParams = MakeShared<FJsonObject>();
	UnknownParams->SetStringField(TEXT("asset_path"), AssetPath);
	UnknownParams->SetStringField(TEXT("native_class"), TEXT("/Script/CortexUMG.CortexUMGNoSuchBindWidgetBase"));
	const FCortexCommandResult Unknown = Router.Execute(TEXT("umg.check_native_bindings"), UnknownParams);
	TestFalse(TEXT("unknown native_class fails"), Unknown.bSuccess);
	TestEqual(TEXT("unknown native_class error code"), Unknown.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> NotWidgetParams = MakeShared<FJsonObject>();
	NotWidgetParams->SetStringField(TEXT("asset_path"), AssetPath);
	NotWidgetParams->SetStringField(TEXT("native_class"), TEXT("/Script/Engine.Actor"));
	const FCortexCommandResult NotWidget = Router.Execute(TEXT("umg.check_native_bindings"), NotWidgetParams);
	TestFalse(TEXT("non-UserWidget native_class fails"), NotWidget.bSuccess);
	TestEqual(TEXT("non-UserWidget native_class error code"), NotWidget.ErrorCode, CortexErrorCodes::InvalidField);

	ResetLoaders(Package);
	Package->ClearDirtyFlag();
	Blueprint->ClearFlags(RF_Standalone);
	Blueprint->MarkAsGarbage();
	Package->MarkAsGarbage();
	return true;
}
