#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "HAL/IConsoleManager.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

namespace
{
struct FInputStateFixture
{
	UPackage* Package;
	UMaterial* Material;
	UMaterialExpressionConstant* Scalar;
	UMaterialExpressionVectorParameter* Vector;
	FCortexMaterialCommandHandler Handler;

	FInputStateFixture()
	{
		const FString Name = TEXT("M_InputState_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Package = CreatePackage(*(TEXT("/Game/Temp/CortexInputState/") + Name));
		Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		Material->bEnableNewHLSLGenerator = false;
		Scalar = NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
		Scalar->R = 0.25f;
		Vector = NewObject<UMaterialExpressionVectorParameter>(Material, NAME_None, RF_Transactional);
		Vector->ParameterName = TEXT("Channels");
		Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Scalar);
		Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Vector);
		Package->SetDirtyFlag(false);
	}

	~FInputStateFixture()
	{
		Scalar->MarkAsGarbage();
		Vector->MarkAsGarbage();
		Material->GetEditorOnlyData()->MarkAsGarbage();
		Material->MarkAsGarbage();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
	}

	TSharedPtr<FJsonObject> Params() const
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Material->GetPathName());
		return Result;
	}

	FCortexCommandResult Connect(const FString& Input, UMaterialExpression* Source, double Output = 0)
	{
		auto Request = Params();
		Request->SetStringField(TEXT("source_node"), Source->GetName());
		Request->SetNumberField(TEXT("source_output"), Output);
		Request->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
		Request->SetStringField(TEXT("target_input"), Input);
		return Handler.Execute(TEXT("connect"), Request);
	}

	FCortexCommandResult Patch(const FString& Input, const FString& Field, TSharedPtr<FJsonValue> Value)
	{
		auto Request = Params();
		Request->SetStringField(TEXT("input"), Input);
		if (!Field.IsEmpty())
		{
			Request->SetField(Field, Value);
		}
		return Handler.Execute(TEXT("set_material_input"), Request);
	}
};

TSharedPtr<FJsonObject> Components(std::initializer_list<TPair<const TCHAR*, double>> Values)
{
	auto Result = MakeShared<FJsonObject>();
	for (const auto& Value : Values)
	{
		Result->SetNumberField(Value.Key, Value.Value);
	}
	return Result;
}

bool HasWarning(const TSharedPtr<FJsonObject>& Data, const FString& Code)
{
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("warnings"), Warnings))
	{
		return false;
	}
	for (const auto& Warning : *Warnings)
	{
		FString Actual;
		if (Warning->AsObject()->TryGetStringField(TEXT("code"), Actual) && Actual == Code)
		{
			return true;
		}
	}
	return false;
}

bool HasWarningForInput(const TSharedPtr<FJsonObject>& Data, const FString& Code, const FString& Input)
{
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("warnings"), Warnings))
	{
		return false;
	}
	for (const auto& Warning : *Warnings)
	{
		const TSharedPtr<FJsonObject> Entry = Warning->AsObject();
		FString ActualCode;
		FString ActualInput;
		if (Entry->TryGetStringField(TEXT("code"), ActualCode) && ActualCode == Code
			&& Entry->TryGetStringField(TEXT("input"), ActualInput) && ActualInput == Input)
		{
			return true;
		}
	}
	return false;
}

/** Scoped console-variable override used only to make translator selection deterministic inside one test.
 *  The override is applied at ECVF_SetByConsole, the strongest SetBy priority, so it wins regardless of the
 *  caller's baseline level. On every exit path the caller's state is returned exactly: non-array priorities
 *  replace a single value per level, so if the baseline already owned the Console level its original value is
 *  written back at that level, and otherwise only the Console level introduced here is unset, which reveals the
 *  untouched baseline value and priority. No tag is used and no config is written. */
class FScopedConsoleVariableOverride
{
public:
	FScopedConsoleVariableOverride(IConsoleVariable* InVariable, int32 InValue)
		: Variable(InVariable)
		, BaselineValue(InVariable ? InVariable->GetInt() : 0)
		, bBaselineOwnsConsole(InVariable && static_cast<int32>(InVariable->GetFlags() & ECVF_SetByMask) == static_cast<int32>(ECVF_SetByConsole))
	{
		if (Variable)
		{
			Variable->Set(InValue, ECVF_SetByConsole);
		}
	}

	~FScopedConsoleVariableOverride()
	{
		if (!Variable)
		{
			return;
		}
		if (bBaselineOwnsConsole)
		{
			Variable->Set(BaselineValue, ECVF_SetByConsole);
		}
		else
		{
			Variable->Unset(ECVF_SetByConsole);
		}
	}

	FScopedConsoleVariableOverride(const FScopedConsoleVariableOverride&) = delete;
	FScopedConsoleVariableOverride& operator=(const FScopedConsoleVariableOverride&) = delete;

private:
	IConsoleVariable* Variable = nullptr;
	int32 BaselineValue = 0;
	bool bBaselineOwnsConsole = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputRewireTest, "Cortex.Material.InputState.Rewire", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputRewireTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto& Input = Fixture.Material->GetEditorOnlyData()->OpacityMask;
	Input.UseConstant = true;
	Input.Constant = 1.0f;
	TestTrue(TEXT("Connect succeeds"), Fixture.Connect(TEXT("OpacityMask"), Fixture.Scalar).bSuccess);
	TestTrue(TEXT("Expression identity retained"), Input.Expression == Fixture.Scalar);
	TestFalse(TEXT("Connected expression overrides old inline constant"), !!Input.UseConstant);
	TestEqual(TEXT("Stored constant preserved"), Input.Constant, 1.0f);
	TestTrue(TEXT("Green channel connect succeeds"), Fixture.Connect(TEXT("OpacityMask"), Fixture.Vector, 2).bSuccess);
	TestEqual(TEXT("Green output index"), Input.OutputIndex, 2);
	TestEqual(TEXT("Channel mask enabled"), Input.Mask, 1);
	TestEqual(TEXT("Red excluded"), Input.MaskR, 0);
	TestEqual(TEXT("Green included"), Input.MaskG, 1);
	TestEqual(TEXT("Blue excluded"), Input.MaskB, 0);
	TestEqual(TEXT("Alpha excluded"), Input.MaskA, 0);
	TestTrue(TEXT("Scalar reconnect succeeds"), Fixture.Connect(TEXT("OpacityMask"), Fixture.Scalar).bSuccess);
	TestEqual(TEXT("Scalar reconnect resets stale channel mask"), Input.Mask, 0);
	auto Disconnect = Fixture.Params();
	Disconnect->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
	Disconnect->SetStringField(TEXT("target_input"), TEXT("OpacityMask"));
	TestTrue(TEXT("Disconnect succeeds"), Fixture.Handler.Execute(TEXT("disconnect"), Disconnect).bSuccess);
	TestNull(TEXT("Disconnected expression"), Input.Expression);
	TestTrue(TEXT("Disconnect selects retained inline constant"), !!Input.UseConstant);
	TestEqual(TEXT("Disconnect preserves constant"), Input.Constant, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputTypedPatchTest, "Cortex.Material.InputState.TypedPatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputTypedPatchTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto* Data = Fixture.Material->GetEditorOnlyData();
	TestTrue(TEXT("Scalar patch"), Fixture.Patch(TEXT("OpacityMask"), TEXT("constant"), MakeShared<FJsonValueNumber>(0.125)).bSuccess);
	TestEqual(TEXT("Scalar native value"), Data->OpacityMask.Constant, 0.125f);
	TestTrue(TEXT("Color patch"), Fixture.Patch(TEXT("BaseColor"), TEXT("constant"), MakeShared<FJsonValueObject>(Components({{TEXT("r"), 0.25}, {TEXT("g"), 0.5}, {TEXT("b"), 0.75}, {TEXT("a"), 1}}))).bSuccess);
	TestEqual(TEXT("Color native value"), Data->BaseColor.Constant, FLinearColor(0.25f, 0.5f, 0.75f, 1));
	TestTrue(TEXT("Vector patch"), Fixture.Patch(TEXT("Normal"), TEXT("constant"), MakeShared<FJsonValueObject>(Components({{TEXT("x"), 0}, {TEXT("y"), 0.5}, {TEXT("z"), 1}}))).bSuccess);
	TestEqual(TEXT("Vector native value"), Data->Normal.Constant, FVector3f(0, 0.5f, 1));
	TestTrue(TEXT("UV alias patch"), Fixture.Patch(TEXT("CustomizedUV0"), TEXT("constant"), MakeShared<FJsonValueObject>(Components({{TEXT("x"), 0.25}, {TEXT("y"), 0.75}}))).bSuccess);
	TestEqual(TEXT("UV native x"), Data->CustomizedUVs[0].Constant.X, 0.25f);
	TestEqual(TEXT("UV native y"), Data->CustomizedUVs[0].Constant.Y, 0.75f);
	TestTrue(TEXT("CustomData0 maps to clear coat"), Fixture.Patch(TEXT("CustomData0"), TEXT("constant"), MakeShared<FJsonValueNumber>(0.5)).bSuccess);
	TestEqual(TEXT("ClearCoat native value"), Data->ClearCoat.Constant, 0.5f);
	auto Request = Fixture.Params();
	Request->SetBoolField(TEXT("include_input_state"), true);
	auto Read = Fixture.Handler.Execute(TEXT("get_material"), Request);
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (TestTrue(TEXT("Read exposes disconnected root inputs"), Read.bSuccess && Read.Data->TryGetArrayField(TEXT("root_inputs"), Inputs)))
	{
		bool bFound = false;
		bool bFoundNonConstant = false;
		for (const auto& Value : *Inputs)
		{
			const auto State = Value->AsObject();
			if (State->GetStringField(TEXT("input")) == TEXT("OpacityMask"))
			{
				bFound = true;
				TestFalse(TEXT("Read marks disconnected input"), State->GetBoolField(TEXT("connected")));
				TestEqual(TEXT("Read scalar constant"), State->GetNumberField(TEXT("constant")), 0.125);
			}
			if (State->GetStringField(TEXT("input")) == TEXT("MaterialAttributes"))
			{
				bFoundNonConstant = true;
				TestFalse(TEXT("Attributes have no inline constants"), State->GetBoolField(TEXT("supports_constant")));
				TestTrue(TEXT("No invented constant field"), State->TryGetField(TEXT("constant"))->IsNull());
				TestTrue(TEXT("No invented inline flag"), State->TryGetField(TEXT("use_constant"))->IsNull());
			}
		}
		TestTrue(TEXT("OpacityMask included"), bFound);
		TestTrue(TEXT("Nonconstant input included"), bFoundNonConstant);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputRefusalTest, "Cortex.Material.InputState.RefusalPreservesState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputRefusalTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto& Input = Fixture.Material->GetEditorOnlyData()->OpacityMask;
	Input.Constant = 0.75f;
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	const TPair<FString, TSharedPtr<FJsonValue>> Cases[] = {
		{TEXT("mask"), MakeShared<FJsonValueNumber>(2)},
		{TEXT("mask_r"), MakeShared<FJsonValueNumber>(0.5)},
		{TEXT("mask"), MakeShared<FJsonValueString>(TEXT("1"))},
		{TEXT("use_constant"), MakeShared<FJsonValueString>(TEXT("false"))},
		{TEXT("constant"), MakeShared<FJsonValueNull>()},
		{TEXT("constant"), MakeShared<FJsonValueString>(TEXT("bad"))},
		{TEXT("constant"), MakeShared<FJsonValueNumber>(1.e100)},
		{TEXT("output_index"), MakeShared<FJsonValueNumber>(0.5)},
		{TEXT("output_index"), MakeShared<FJsonValueNumber>(999)},
		{TEXT("unknown"), MakeShared<FJsonValueNumber>(0)},
		{TEXT(""), MakeShared<FJsonValueNull>()}
	};
	for (const auto& Case : Cases)
	{
		TestFalse(*FString::Printf(TEXT("Invalid patch field %s refuses"), *Case.Key), Fixture.Patch(TEXT("OpacityMask"), Case.Key, Case.Value).bSuccess);
		TestEqual(TEXT("Refusal preserves constant"), Input.Constant, 0.75f);
		TestNull(TEXT("Refusal preserves expression"), Input.Expression);
		TestFalse(TEXT("Refusal keeps package clean"), Fixture.Package->IsDirty());
		TestEqual(TEXT("Refusal creates no transaction"), GEditor->Trans->GetQueueLength(), Transactions);
	}
	TestFalse(TEXT("Incomplete color refuses"), Fixture.Patch(TEXT("BaseColor"), TEXT("constant"), MakeShared<FJsonValueObject>(Components({{TEXT("r"), 0.5}}))).bSuccess);
	TestFalse(TEXT("Nonconstant input refuses constant"), Fixture.Patch(TEXT("MaterialAttributes"), TEXT("use_constant"), MakeShared<FJsonValueBoolean>(true)).bSuccess);
	// Output indices are also bounded by the connected expression's real outputs, not only by its presence.
	Input.Expression = Fixture.Scalar;
	Input.OutputIndex = 0;
	const int32 ConnectedTransactions = GEditor->Trans->GetQueueLength();
	const TSharedPtr<FJsonValue> BadIndices[] = {
		MakeShared<FJsonValueNumber>(999),
		MakeShared<FJsonValueNumber>(-1),
		MakeShared<FJsonValueNumber>(0.5)
	};
	for (const auto& BadIndex : BadIndices)
	{
		TestFalse(TEXT("Connected out-of-range output index refuses"), Fixture.Patch(TEXT("OpacityMask"), TEXT("output_index"), BadIndex).bSuccess);
		TestTrue(TEXT("Index refusal preserves the connection"), Input.Expression == Fixture.Scalar);
		TestEqual(TEXT("Index refusal preserves the output index"), Input.OutputIndex, 0);
		TestFalse(TEXT("Index refusal keeps package clean"), Fixture.Package->IsDirty());
		TestEqual(TEXT("Index refusal creates no transaction"), GEditor->Trans->GetQueueLength(), ConnectedTransactions);
	}
	Input.Expression = nullptr;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputDisabledPolicyTest, "Cortex.Material.InputState.DisabledInlinePolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputDisabledPolicyTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	const TCHAR* Names[] = {TEXT("WorldPositionOffset"), TEXT("Anisotropy"), TEXT("PixelDepthOffset"), TEXT("Displacement")};
	for (const TCHAR* Name : Names)
	{
		TestFalse(TEXT("Engine-disabled inline true refuses"), Fixture.Patch(Name, TEXT("use_constant"), MakeShared<FJsonValueBoolean>(true)).bSuccess);
		TestTrue(TEXT("Engine-disabled inline false is writable"), Fixture.Patch(Name, TEXT("use_constant"), MakeShared<FJsonValueBoolean>(false)).bSuccess);
		auto Request = Fixture.Params();
		Request->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
		Request->SetStringField(TEXT("target_input"), Name);
		TestTrue(TEXT("Disabled-inline disconnect succeeds"), Fixture.Handler.Execute(TEXT("disconnect"), Request).bSuccess);
	}
	TestFalse(TEXT("WPO inline stays disabled"), !!Fixture.Material->GetEditorOnlyData()->WorldPositionOffset.UseConstant);
	TestFalse(TEXT("Anisotropy inline stays disabled"), !!Fixture.Material->GetEditorOnlyData()->Anisotropy.UseConstant);
	TestFalse(TEXT("PDO inline stays disabled"), !!Fixture.Material->GetEditorOnlyData()->PixelDepthOffset.UseConstant);
	TestFalse(TEXT("Displacement inline stays disabled"), !!Fixture.Material->GetEditorOnlyData()->Displacement.UseConstant);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputDiagnosticsTest, "Cortex.Material.InputState.Diagnostics", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputDiagnosticsTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	Fixture.Material->BlendMode = BLEND_Masked;
	Fixture.Material->bCanMaskedBeAssumedOpaque = true;
	auto& Input = Fixture.Material->GetEditorOnlyData()->OpacityMask;
	Input.Expression = Fixture.Scalar;
	Input.UseConstant = true;
	Input.Constant = 1;
	auto Read = Fixture.Handler.Execute(TEXT("get_material"), Fixture.Params());
	FString Effective;
	TestTrue(TEXT("Effective blend field returned"), Read.bSuccess && Read.Data->TryGetStringField(TEXT("effective_blend_mode"), Effective));
	TestEqual(TEXT("Legacy flag overrides effective mode"), Effective, FString(TEXT("Opaque")));
	TestTrue(TEXT("Blend mismatch warning"), HasWarning(Read.Data, TEXT("BLEND_MODE_MISMATCH")));
	TestTrue(TEXT("Ignored expression warning"), HasWarning(Read.Data, TEXT("CONNECTED_EXPRESSION_IGNORED")));
	// The accepted repair path is an explicit authoring write, not a direct field edit.
	auto Repair = Fixture.Params();
	Repair->SetStringField(TEXT("property_name"), TEXT("bCanMaskedBeAssumedOpaque"));
	Repair->SetBoolField(TEXT("value"), false);
	TestTrue(TEXT("Explicit opaque-override repair succeeds"), Fixture.Handler.Execute(TEXT("set_material_property"), Repair).bSuccess);
	TestFalse(TEXT("Repair cleared native override"), !!Fixture.Material->bCanMaskedBeAssumedOpaque);
	TestTrue(TEXT("Repair dirties package for explicit save"), Fixture.Package->IsDirty());
	Input.UseConstant = false;
	Read = Fixture.Handler.Execute(TEXT("get_material"), Fixture.Params());
	FString RepairedAuthored;
	FString RepairedEffective;
	TestTrue(TEXT("Authored blend field returned after repair"),
		Read.bSuccess && Read.Data->TryGetStringField(TEXT("blend_mode"), RepairedAuthored));
	TestEqual(TEXT("Authored blend mode stays Masked"), RepairedAuthored, FString(TEXT("Masked")));
	TestTrue(TEXT("Effective blend field returned after repair"),
		Read.Data.IsValid() && Read.Data->TryGetStringField(TEXT("effective_blend_mode"), RepairedEffective));
	TestEqual(TEXT("Repair restores effective Masked"), RepairedEffective, FString(TEXT("Masked")));
	TestFalse(TEXT("Repair removes blend warning"), HasWarning(Read.Data, TEXT("BLEND_MODE_MISMATCH")));
	TestFalse(TEXT("Repair removes ignored expression warning"), HasWarning(Read.Data, TEXT("CONNECTED_EXPRESSION_IGNORED")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputDiagnosticsNegativeTest, "Cortex.Material.InputState.DiagnosticsNegatives", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputDiagnosticsNegativeTest::RunTest(const FString& Parameters)
{
	// Inline-disabled inputs never compile their inline constant, so a connected expression is not ignored.
	FInputStateFixture Disabled;
	auto* DisabledData = Disabled.Material->GetEditorOnlyData();
	DisabledData->WorldPositionOffset.Expression = Disabled.Scalar;
	DisabledData->WorldPositionOffset.UseConstant = true;
	auto Read = Disabled.Handler.Execute(TEXT("get_material"), Disabled.Params());
	TestTrue(TEXT("Read succeeds with inline-disabled state"), Read.bSuccess && Read.Data.IsValid());
	TestFalse(TEXT("Disabled-inline input does not warn"),
		HasWarningForInput(Read.Data, TEXT("CONNECTED_EXPRESSION_IGNORED"), TEXT("WorldPositionOffset")));
	TestFalse(TEXT("Disabled-inline state emits no ignored-expression warning"),
		HasWarning(Read.Data, TEXT("CONNECTED_EXPRESSION_IGNORED")));
	TestTrue(TEXT("Read does not repair authored disabled-inline state"), !!DisabledData->WorldPositionOffset.UseConstant);

	// The new translator prioritizes a valid connection, so the warning must be scoped to translator selection
	// rather than to the authoring condition. Both branches are explicit so the result cannot depend on the
	// caller's baseline value, and the guard restores the original value and priority on every exit path.
	FInputStateFixture Fixture;
	Fixture.Material->bEnableNewHLSLGenerator = true;
	auto& Input = Fixture.Material->GetEditorOnlyData()->OpacityMask;
	Input.Expression = Fixture.Scalar;
	Input.UseConstant = true;
	IConsoleVariable* Translator = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Material.Translator.EnableNew"));
	if (!TestNotNull(TEXT("Translator selection cvar is registered"), Translator))
	{
		return false;
	}
	const int32 BaselineValue = Translator->GetInt();
	const int32 BaselineSetBy = static_cast<int32>(Translator->GetFlags() & ECVF_SetByMask);
	{
		// Explicit new-translator branch: selection is actually in effect, and no ignored-expression warning is emitted.
		FScopedConsoleVariableOverride NewTranslator(Translator, 1);
		TestEqual(TEXT("Translator cvar readback reflects the scoped selection"), Translator->GetInt(), 1);
		if (TestTrue(TEXT("New translator is actually selected"), Fixture.Material->IsUsingNewHLSLGenerator()))
		{
			auto Selected = Fixture.Handler.Execute(TEXT("get_material"), Fixture.Params());
			TestTrue(TEXT("Selected new translator read succeeds"), Selected.bSuccess && Selected.Data.IsValid());
			TestFalse(TEXT("New translator does not falsely warn"),
				HasWarningForInput(Selected.Data, TEXT("CONNECTED_EXPRESSION_IGNORED"), TEXT("OpacityMask")));
		}
	}
	TestEqual(TEXT("Override restores the original selection value"), Translator->GetInt(), BaselineValue);
	TestEqual(TEXT("Override restores the original priority level"),
		static_cast<int32>(Translator->GetFlags() & ECVF_SetByMask), BaselineSetBy);
	{
		// Explicit legacy branch: same authored state, legacy translator selected, warning must be present.
		FScopedConsoleVariableOverride LegacyTranslator(Translator, 0);
		TestEqual(TEXT("Translator cvar readback reflects the legacy selection"), Translator->GetInt(), 0);
		if (TestFalse(TEXT("Legacy translator is actually selected"), Fixture.Material->IsUsingNewHLSLGenerator()))
		{
			auto Legacy = Fixture.Handler.Execute(TEXT("get_material"), Fixture.Params());
			TestTrue(TEXT("Legacy translator read succeeds"), Legacy.bSuccess && Legacy.Data.IsValid());
			TestTrue(TEXT("Legacy translator warns for the identical authored state"),
				HasWarningForInput(Legacy.Data, TEXT("CONNECTED_EXPRESSION_IGNORED"), TEXT("OpacityMask")));
		}
	}
	TestEqual(TEXT("Second override restores the original selection value"), Translator->GetInt(), BaselineValue);
	TestEqual(TEXT("Second override restores the original priority level"),
		static_cast<int32>(Translator->GetFlags() & ECVF_SetByMask), BaselineSetBy);
	TestTrue(TEXT("Diagnostics never repair authored state"),
		!!Input.UseConstant && Input.Expression == Fixture.Scalar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputBlendNamingTest, "Cortex.Material.InputState.BlendNaming", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputBlendNamingTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	Fixture.Material->bCanMaskedBeAssumedOpaque = false;
	const auto ReadStatus = [&](const TCHAR* ModeName, FString& OutAuthored, FString& OutEffective)
	{
		auto Read = Fixture.Handler.Execute(TEXT("get_material"), Fixture.Params());
		if (!TestTrue(*FString::Printf(TEXT("%s read succeeds"), ModeName), Read.bSuccess && Read.Data.IsValid()))
		{
			return;
		}
		TestTrue(*FString::Printf(TEXT("%s authored blend mode reported"), ModeName),
			Read.Data->TryGetStringField(TEXT("blend_mode"), OutAuthored));
		TestTrue(*FString::Printf(TEXT("%s effective blend mode reported"), ModeName),
			Read.Data->TryGetStringField(TEXT("effective_blend_mode"), OutEffective));
	};
	const TPair<EBlendMode, const TCHAR*> Established[] = {
		{BLEND_Opaque, TEXT("Opaque")},
		{BLEND_Masked, TEXT("Masked")},
		{BLEND_Translucent, TEXT("Translucent")},
		{BLEND_Additive, TEXT("Additive")},
		{BLEND_Modulate, TEXT("Modulate")}
	};
	for (const auto& Case : Established)
	{
		Fixture.Material->BlendMode = Case.Key;
		FString Authored;
		FString Effective;
		ReadStatus(Case.Value, Authored, Effective);
		TestEqual(*FString::Printf(TEXT("Established authored name for %s"), Case.Value), Authored, FString(Case.Value));
		TestEqual(*FString::Printf(TEXT("Established effective name for %s"), Case.Value), Effective, FString(Case.Value));
	}
	const TPair<EBlendMode, const TCHAR*> Complete[] = {
		{BLEND_AlphaComposite, TEXT("AlphaComposite")},
		{BLEND_AlphaHoldout, TEXT("AlphaHoldout")}
	};
	for (const auto& Case : Complete)
	{
		Fixture.Material->BlendMode = Case.Key;
		FString Authored;
		FString Effective;
		ReadStatus(Case.Value, Authored, Effective);
		TestEqual(*FString::Printf(TEXT("Authored name uses the complete enum for %s"), Case.Value), Authored, FString(Case.Value));
		TestEqual(*FString::Printf(TEXT("Effective name uses the complete enum for %s"), Case.Value), Effective, FString(Case.Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputNestedPropertyTest, "Cortex.Material.InputState.NestedProperties", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputNestedPropertyTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto Set = [&](const FString& Path, TSharedPtr<FJsonValue> Value)
	{
		auto Request = Fixture.Params();
		Request->SetStringField(TEXT("property_name"), Path);
		Request->SetField(TEXT("value"), Value);
		return Fixture.Handler.Execute(TEXT("set_material_property"), Request);
	};
	TestTrue(TEXT("Nested inline flag writable"), Set(TEXT("EditorOnlyData.OpacityMask.UseConstant"), MakeShared<FJsonValueBoolean>(false)).bSuccess);
	TestTrue(TEXT("Nested scalar zero writable"), Set(TEXT("EditorOnlyData.OpacityMask.Constant"), MakeShared<FJsonValueNumber>(0)).bSuccess);
	TestEqual(TEXT("Nested zero reached native field"), Fixture.Material->GetEditorOnlyData()->OpacityMask.Constant, 0.f);
	TestTrue(TEXT("Nested color member writable"), Set(TEXT("EditorOnlyData.BaseColor.Constant.R"), MakeShared<FJsonValueNumber>(0.375)).bSuccess);
	TestEqual(TEXT("Nested color member reached native field"), Fixture.Material->GetEditorOnlyData()->BaseColor.Constant.R, 0.375f);
	auto Request = Fixture.Params();
	TArray<TSharedPtr<FJsonValue>> Paths = {
		MakeShared<FJsonValueString>(TEXT("EditorOnlyData.OpacityMask.UseConstant")),
		MakeShared<FJsonValueString>(TEXT("EditorOnlyData.OpacityMask.Constant")),
		MakeShared<FJsonValueString>(TEXT("EditorOnlyData.BaseColor.Constant.R")),
		MakeShared<FJsonValueString>(TEXT("bCanMaskedBeAssumedOpaque"))
	};
	Request->SetArrayField(TEXT("properties"), Paths);
	auto Read = Fixture.Handler.Execute(TEXT("get_material"), Request);
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (TestTrue(TEXT("Selected property projection returned"), Read.bSuccess && Read.Data->TryGetObjectField(TEXT("properties"), Values)))
	{
		TestFalse(TEXT("Selected false retained"), (*Values)->GetBoolField(TEXT("EditorOnlyData.OpacityMask.UseConstant")));
		TestEqual(TEXT("Selected zero retained"), (*Values)->GetNumberField(TEXT("EditorOnlyData.OpacityMask.Constant")), 0.);
		TestEqual(TEXT("Selected nested component"), (*Values)->GetNumberField(TEXT("EditorOnlyData.BaseColor.Constant.R")), 0.375);
		TestEqual(TEXT("No unselected properties"), (*Values)->Values.Num(), 4);
	}
	Paths.Add(MakeShared<FJsonValueString>(TEXT("EditorOnlyData.DoesNotExist")));
	Request->SetArrayField(TEXT("properties"), Paths);
	TestFalse(TEXT("Invalid selector refuses instead of partial projection"), Fixture.Handler.Execute(TEXT("get_material"), Request).bSuccess);
	Paths.Last() = MakeShared<FJsonValueNumber>(1);
	Request->SetArrayField(TEXT("properties"), Paths);
	TestFalse(TEXT("Non-string selector refuses instead of partial projection"), Fixture.Handler.Execute(TEXT("get_material"), Request).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputNestedRefusalTest, "Cortex.Material.InputState.NestedRefusalPreservesState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputNestedRefusalTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto* Data = Fixture.Material->GetEditorOnlyData();
	Data->OpacityMask.Constant = 0.75f;
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	const TPair<FString, TSharedPtr<FJsonValue>> Cases[] = {
		{TEXT("EditorOnlyData.OpacityMask.Constant"), MakeShared<FJsonValueNumber>(1.e100)},
		{TEXT("EditorOnlyData.OpacityMask.MaskR"), MakeShared<FJsonValueNumber>(0.5)},
		{TEXT("EditorOnlyData.OpacityMask.UseConstant"), MakeShared<FJsonValueString>(TEXT("false"))},
		{TEXT("EditorOnlyData.OpacityMask.Expression"), MakeShared<FJsonValueString>(Fixture.Vector->GetPathName())},
		{TEXT("EditorOnlyData.OpacityMask.InputName"), MakeShared<FJsonValueString>(TEXT("R"))},
		{TEXT("EditorOnlyData.OpacityMask"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())},
		{TEXT("EditorOnlyData.WorldPositionOffset.UseConstant"), MakeShared<FJsonValueBoolean>(true)},
		{TEXT("EditorOnlyData.BaseColor.Constant.R"), MakeShared<FJsonValueNumber>(1.e100)},
		{TEXT("EditorOnlyData.NoSuchMember"), MakeShared<FJsonValueNumber>(0)},
		{TEXT("PhysMaterial.Friction"), MakeShared<FJsonValueNumber>(0.5)},
		{TEXT("TwoSided"), MakeShared<FJsonValueString>(TEXT("bad"))}
	};
	for (const auto& Case : Cases)
	{
		auto Request = Fixture.Params();
		Request->SetStringField(TEXT("property_name"), Case.Key);
		Request->SetField(TEXT("value"), Case.Value);
		TestFalse(*FString::Printf(TEXT("Invalid property %s refuses"), *Case.Key), Fixture.Handler.Execute(TEXT("set_material_property"), Request).bSuccess);
		TestEqual(TEXT("Failed conversion preserves scalar"), Data->OpacityMask.Constant, 0.75f);
		TestNull(TEXT("No expression replacement"), Data->OpacityMask.Expression);
		TestFalse(TEXT("No disabled constant enabled"), !!Data->WorldPositionOffset.UseConstant);
		TestFalse(TEXT("No dirty state from refusal"), Fixture.Package->IsDirty());
		TestEqual(TEXT("No transaction from refusal"), GEditor->Trans->GetQueueLength(), Transactions);
	}
	// Referenced external assets are not writable subobjects of this material. The external object is
	// instantiated through the reflected PhysMaterial class so this test needs no physics module symbols.
	FObjectProperty* PhysProperty = FindFProperty<FObjectProperty>(UMaterial::StaticClass(), TEXT("PhysMaterial"));
	UClass* ExternalClass = PhysProperty ? PhysProperty->PropertyClass.Get() : nullptr;
	if (TestNotNull(TEXT("PhysMaterial FObjectProperty is reflected"), PhysProperty)
		&& TestNotNull(TEXT("PhysMaterial class is reflected"), ExternalClass))
	{
		void* PhysAddress = PhysProperty->ContainerPtrToValuePtr<void>(Fixture.Material);
		// Instantiated through the property's own class, so no compile-time physics module symbol is needed.
		UObject* External = NewObject<UObject>(GetTransientPackage(), ExternalClass, NAME_None, RF_Transient);
		FNumericProperty* Friction = CastField<FNumericProperty>(ExternalClass->FindPropertyByName(TEXT("Friction")));
		if (TestNotNull(TEXT("External owner class exposes reflected Friction"), Friction))
		{
			void* FrictionAddress = Friction->ContainerPtrToValuePtr<void>(External);
			Friction->SetFloatingPointPropertyValue(FrictionAddress, 0.5);
			PhysProperty->SetObjectPropertyValue(PhysAddress, External);
			auto ExternalRequest = Fixture.Params();
			ExternalRequest->SetStringField(TEXT("property_name"), TEXT("PhysMaterial.Friction"));
			ExternalRequest->SetNumberField(TEXT("value"), 0.25);
			TestFalse(TEXT("External owner hop refuses"), Fixture.Handler.Execute(TEXT("set_material_property"), ExternalRequest).bSuccess);
			TestEqual(TEXT("External object unchanged"), Friction->GetFloatingPointPropertyValue(FrictionAddress), 0.5);
			TestTrue(TEXT("Material reference preserved"), PhysProperty->GetObjectPropertyValue(PhysAddress) == External);
			TestFalse(TEXT("No dirty state from external refusal"), Fixture.Package->IsDirty());
			TestEqual(TEXT("No transaction from external refusal"), GEditor->Trans->GetQueueLength(), Transactions);
			PhysProperty->SetObjectPropertyValue(PhysAddress, nullptr);
		}
		External->MarkAsGarbage();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputUndoTest, "Cortex.Material.InputState.UndoRedo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputUndoTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto& Input = Fixture.Material->GetEditorOnlyData()->OpacityMask;
	Input.UseConstant = true;
	Input.Constant = 0.75f;
	TestTrue(TEXT("Undo fixture connect"), Fixture.Connect(TEXT("OpacityMask"), Fixture.Scalar).bSuccess);
	TestTrue(TEXT("Connect transaction undo"), GEditor->UndoTransaction());
	TestNull(TEXT("Undo restored disconnected state"), Input.Expression);
	TestTrue(TEXT("Undo restored inline selection"), !!Input.UseConstant);
	TestTrue(TEXT("Connect transaction redo"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo restored expression identity"), Input.Expression == Fixture.Scalar);
	TestFalse(TEXT("Redo restored expression selection"), !!Input.UseConstant);
	auto Request = Fixture.Params();
	Request->SetStringField(TEXT("property_name"), TEXT("EditorOnlyData.OpacityMask.Constant"));
	Request->SetNumberField(TEXT("value"), 0.125);
	TestTrue(TEXT("Nested transaction write"), Fixture.Handler.Execute(TEXT("set_material_property"), Request).bSuccess);
	TestTrue(TEXT("Nested write undo"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo restored native constant"), Input.Constant, 0.75f);
	TestTrue(TEXT("Nested write redo"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restored native constant"), Input.Constant, 0.125f);
	TestTrue(TEXT("Nested write dirties package for explicit save"), Fixture.Package->IsDirty());
	// A typed root-state patch is one undoable step spanning the material and its editor data.
	auto PatchRequest = Fixture.Params();
	PatchRequest->SetStringField(TEXT("input"), TEXT("OpacityMask"));
	PatchRequest->SetNumberField(TEXT("constant"), 0.5);
	PatchRequest->SetBoolField(TEXT("use_constant"), true);
	PatchRequest->SetNumberField(TEXT("mask"), 1);
	TestTrue(TEXT("Typed state patch succeeds"), Fixture.Handler.Execute(TEXT("set_material_input"), PatchRequest).bSuccess);
	TestEqual(TEXT("Typed patch native constant"), Input.Constant, 0.5f);
	TestTrue(TEXT("Typed patch native inline selection"), !!Input.UseConstant);
	TestEqual(TEXT("Typed patch native mask"), Input.Mask, 1);
	TestTrue(TEXT("Typed patch undo"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo restored typed constant"), Input.Constant, 0.125f);
	TestFalse(TEXT("Undo restored typed inline selection"), !!Input.UseConstant);
	TestEqual(TEXT("Undo restored typed mask"), Input.Mask, 0);
	TestTrue(TEXT("Typed patch redo"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restored typed constant"), Input.Constant, 0.5f);
	TestTrue(TEXT("Redo restored typed inline selection"), !!Input.UseConstant);
	TestEqual(TEXT("Redo restored typed mask"), Input.Mask, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAlternateRootRouteTest, "Cortex.Material.InputState.AlternateRootRoute", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputAlternateRootRouteTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto* Data = Fixture.Material->GetEditorOnlyData();
	Fixture.Scalar->Material = Fixture.Material;
	Data->ExpressionCollection.ExpressionExecBegin = Fixture.Scalar;
	Data->OpacityMask.Constant = 0.75f;
	Data->OpacityMask.MaskR = 1;
	Data->WorldPositionOffset.Constant = FVector3f(0.25f, 0.5f, 0.75f);
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	// Alias route: prefix + <input leaf> addresses the input struct; the whole editor data object is the route
	// itself, so it must NOT carry the leaf separator (Route ends with '.', DataRoute does not).
	const FString DataRoute = TEXT("EditorOnlyData.ExpressionCollection.ExpressionExecBegin.Material.EditorOnlyData");
	const FString Route = DataRoute + TEXT(".");
	auto Set = [&](const FString& Path, TSharedPtr<FJsonValue> Value)
	{
		auto Request = Fixture.Params();
		Request->SetStringField(TEXT("property_name"), Path);
		Request->SetField(TEXT("value"), Value);
		return Fixture.Handler.Execute(TEXT("set_material_property"), Request);
	};
	TestFalse(TEXT("Alternate route refuses disabled inline constant"),
		Set(Route + TEXT("WorldPositionOffset.UseConstant"), MakeShared<FJsonValueBoolean>(true)).bSuccess);
	TestFalse(TEXT("Alternate route refuses expression replacement"),
		Set(Route + TEXT("OpacityMask.Expression"), MakeShared<FJsonValueString>(Fixture.Vector->GetPathName())).bSuccess);
	TestFalse(TEXT("Alternate route refuses whole root replacement"),
		Set(Route + TEXT("OpacityMask"), MakeShared<FJsonValueObject>(Components({{TEXT("UseConstant"), 1.0}}))).bSuccess);
	TestFalse(TEXT("Alternate route refuses fractional mask"),
		Set(Route + TEXT("OpacityMask.MaskR"), MakeShared<FJsonValueNumber>(0.5)).bSuccess);
	TestFalse(TEXT("Alternate route refuses non-boolean inline flag"),
		Set(Route + TEXT("OpacityMask.UseConstant"), MakeShared<FJsonValueNumber>(1)).bSuccess);
	// The resolver already refuses the direct whole-editor-data form (segment count < 2 after the prefix). The
	// same replacement must not be accepted through an owned alias route: the same-material reference is used so
	// an accepted write is observable (success, dirty, undo) without swapping in a foreign editor data object.
	TestFalse(TEXT("Direct whole editor-data form is refused"),
		Set(TEXT("EditorOnlyData"), MakeShared<FJsonValueString>(Data->GetPathName())).bSuccess);
	auto WholeData = Set(DataRoute, MakeShared<FJsonValueString>(Data->GetPathName()));
	TestFalse(TEXT("Alternate route refuses whole editor-data replacement"), WholeData.bSuccess);
	if (WholeData.bSuccess)
	{
		// Pre-guard behaviour is a same-object no-op; consume its transaction so later tests see a clean queue.
		GEditor->UndoTransaction();
	}
	TestTrue(TEXT("Whole editor-data reference preserved"), Fixture.Material->GetEditorOnlyData() == Data);
	TestFalse(TEXT("Disabled inline constant stays disabled"), !!Data->WorldPositionOffset.UseConstant);
	TestEqual(TEXT("Refusal preserves disabled inline constant"), Data->WorldPositionOffset.Constant, FVector3f(0.25f, 0.5f, 0.75f));
	TestNull(TEXT("Refusal preserves expression"), Data->OpacityMask.Expression);
	TestFalse(TEXT("Refusal preserves inline selection"), !!Data->OpacityMask.UseConstant);
	TestEqual(TEXT("Refusal preserves mask"), Data->OpacityMask.MaskR, 1);
	TestFalse(TEXT("Refusal leaves package clean"), Fixture.Package->IsDirty());
	TestEqual(TEXT("Refusal creates no transaction"), GEditor->Trans->GetQueueLength(), Transactions);
	TestTrue(TEXT("Alternate route writes representable scalar leaf"),
		Set(Route + TEXT("OpacityMask.Constant"), MakeShared<FJsonValueNumber>(0.5)).bSuccess);
	TestEqual(TEXT("Alternate route reaches the same native storage as the direct form"),
		Data->OpacityMask.Constant, 0.5f);
	TestTrue(TEXT("Alternate route accepts explicit false for a disabled inline input"),
		Set(Route + TEXT("WorldPositionOffset.UseConstant"), MakeShared<FJsonValueBoolean>(false)).bSuccess);
	TestFalse(TEXT("Disabled inline constant accepts explicit false"), !!Data->WorldPositionOffset.UseConstant);
	TestTrue(TEXT("Alternate route dirties package for explicit save"), Fixture.Package->IsDirty());
	Data->ExpressionCollection.ExpressionExecBegin = nullptr;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputIntegerBoundsTest, "Cortex.Material.InputState.NestedIntegerBounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputIntegerBoundsTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto* Data = Fixture.Material->GetEditorOnlyData();
	Data->SubstrateVersion = 7;
	Data->SubstrateConversionVersion = -3;
	Fixture.Material->StencilRefValue = 12;
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	auto Set = [&](const TCHAR* Path, double Value)
	{
		auto Request = Fixture.Params();
		Request->SetStringField(TEXT("property_name"), Path);
		Request->SetNumberField(TEXT("value"), Value);
		return Fixture.Handler.Execute(TEXT("set_material_property"), Request);
	};
	TestFalse(TEXT("Int32 overflow refuses"), Set(TEXT("EditorOnlyData.SubstrateVersion"), 2147483648.0).bSuccess);
	TestFalse(TEXT("Int32 underflow refuses"), Set(TEXT("EditorOnlyData.SubstrateConversionVersion"), -2147483649.0).bSuccess);
	TestFalse(TEXT("Unsigned byte underflow refuses"), Set(TEXT("StencilRefValue"), -1.0).bSuccess);
	TestFalse(TEXT("Unsigned byte overflow refuses"), Set(TEXT("StencilRefValue"), 256.0).bSuccess);
	TestEqual(TEXT("Overflow leaves native value"), Data->SubstrateVersion, 7);
	TestEqual(TEXT("Underflow leaves native value"), Data->SubstrateConversionVersion, -3);
	TestEqual(TEXT("Byte refusal leaves native value"), static_cast<int32>(Fixture.Material->StencilRefValue), 12);
	TestFalse(TEXT("Overflow leaves package clean"), Fixture.Package->IsDirty());
	TestEqual(TEXT("Overflow creates no transaction"), GEditor->Trans->GetQueueLength(), Transactions);
	TestTrue(TEXT("Int32 maximum is representable"), Set(TEXT("EditorOnlyData.SubstrateVersion"), 2147483647.0).bSuccess);
	TestEqual(TEXT("Int32 maximum stored exactly"), Data->SubstrateVersion, MAX_int32);
	TestTrue(TEXT("Int32 minimum is representable"), Set(TEXT("EditorOnlyData.SubstrateConversionVersion"), -2147483648.0).bSuccess);
	TestEqual(TEXT("Int32 minimum stored exactly"), Data->SubstrateConversionVersion, MIN_int32);
	TestTrue(TEXT("Unsigned byte minimum is representable"), Set(TEXT("StencilRefValue"), 0.0).bSuccess);
	TestEqual(TEXT("Unsigned byte minimum stored exactly"), static_cast<int32>(Fixture.Material->StencilRefValue), 0);
	TestTrue(TEXT("Unsigned byte maximum is representable"), Set(TEXT("StencilRefValue"), 255.0).bSuccess);
	TestEqual(TEXT("Unsigned byte maximum stored exactly"), static_cast<int32>(Fixture.Material->StencilRefValue), 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputSelectedSupportTest, "Cortex.Material.InputState.SelectedReadSupport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexInputSelectedSupportTest::RunTest(const FString& Parameters)
{
	FInputStateFixture Fixture;
	auto Read = [&](std::initializer_list<const TCHAR*> Paths)
	{
		auto Request = Fixture.Params();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Path : Paths)
		{
			Values.Add(MakeShared<FJsonValueString>(Path));
		}
		Request->SetArrayField(TEXT("properties"), Values);
		return Fixture.Handler.Execute(TEXT("get_material"), Request);
	};
	auto NullReference = Read({TEXT("PhysMaterial")});
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (TestTrue(TEXT("Legitimate null object reference read succeeds"),
		NullReference.bSuccess && NullReference.Data.IsValid()
		&& NullReference.Data->TryGetObjectField(TEXT("properties"), Values)))
	{
		const TSharedPtr<FJsonValue> NullValue = (*Values)->TryGetField(TEXT("PhysMaterial"));
		TestTrue(TEXT("Null object reference stays a legitimate null"), NullValue.IsValid() && NullValue->IsNull());
	}
	auto Mixed = Read({TEXT("EditorOnlyData.OpacityMask.Constant"), TEXT("PreshaderGap")});
	TestFalse(TEXT("Mixed supported and unsupported selectors refuse"), Mixed.bSuccess);
	TestEqual(TEXT("Unsupported selector reports InvalidField"), Mixed.ErrorCode, CortexErrorCodes::InvalidField);
	TestFalse(TEXT("Refusal publishes no partial projection"),
		Mixed.Data.IsValid() && Mixed.Data->HasField(TEXT("properties")));
	auto Unsupported = Read({TEXT("NeuralProfileId")});
	TestFalse(TEXT("Unsupported int8 selector refuses"), Unsupported.bSuccess);
	auto Supported = Read({TEXT("EditorOnlyData.OpacityMask.Constant")});
	TestTrue(TEXT("Supported selector still reads"), Supported.bSuccess);
	return true;
}
