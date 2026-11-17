#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionColorRamp.h"
#include "Curves/CurveLinearColor.h"
#include "Misc/ScopeExit.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

namespace
{
/** In-memory material with a Constant wired into BaseColor and into a Multiply's A input. Nothing touches disk. */
struct FUndoFixture
{
	UPackage* Package;
	UMaterial* Material;
	UMaterialExpressionConstant* Constant;
	UMaterialExpressionMultiply* Multiply;
	UMaterialInstanceConstant* Instance;
	FCortexMaterialCommandHandler Handler;

	FUndoFixture()
	{
		const FString Name = TEXT("M_Undo_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Package = CreatePackage(*(TEXT("/Game/Temp/CortexMaterialUndo/") + Name));
		Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		Material->bEnableNewHLSLGenerator = false;
		// No RF_Transactional, like expressions that add_node created before this fix.
		Constant = NewObject<UMaterialExpressionConstant>(Material);
		Constant->R = 0.25f;
		Multiply = NewObject<UMaterialExpressionMultiply>(Material);
		Multiply->A.Connect(0, Constant);
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		Data->ExpressionCollection.Expressions.Add(Constant);
		Data->ExpressionCollection.Expressions.Add(Multiply);
		Data->BaseColor.Connect(0, Constant);

		const FString InstanceName = TEXT("MI_Undo_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		UPackage* InstancePackage = CreatePackage(*(TEXT("/Game/Temp/CortexMaterialUndo/") + InstanceName));
		// Same flags as create_instance, which does not mark the instance transactional.
		Instance = NewObject<UMaterialInstanceConstant>(InstancePackage, *InstanceName, RF_Public | RF_Standalone);
		Instance->Parent = Material;
		Package->SetDirtyFlag(false);
		InstancePackage->SetDirtyFlag(false);
	}

	~FUndoFixture()
	{
		UPackage* InstancePackage = Instance->GetPackage();
		Instance->MarkAsGarbage();
		InstancePackage->SetDirtyFlag(false);
		InstancePackage->MarkAsGarbage();
		Constant->MarkAsGarbage();
		Multiply->MarkAsGarbage();
		Material->GetEditorOnlyData()->MarkAsGarbage();
		Material->MarkAsGarbage();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
	}

	TSharedPtr<FJsonObject> MaterialParams() const
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Material->GetPathName());
		return Result;
	}

	TSharedPtr<FJsonObject> InstanceParams() const
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Instance->GetPathName());
		return Result;
	}

	TArray<TObjectPtr<UMaterialExpression>>& Expressions() const
	{
		return Material->GetEditorOnlyData()->ExpressionCollection.Expressions;
	}

	FCortexCommandResult SetScalar(const FString& Name, double Value)
	{
		auto Request = InstanceParams();
		Request->SetStringField(TEXT("parameter_name"), Name);
		Request->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		Request->SetNumberField(TEXT("value"), Value);
		return Handler.Execute(TEXT("set_parameter"), Request);
	}
};

/** The command's own transaction is the one Undo would revert next. Guards against undoing an unrelated step. */
bool IsNextUndo(const TCHAR* Title)
{
	return GEditor->Trans->GetUndoContext(true).Title.ToString().StartsWith(Title);
}

bool HasScalarOverride(const UMaterialInstanceConstant* Instance, FName Name)
{
	return Instance->ScalarParameterValues.ContainsByPredicate([Name](const FScalarParameterValue& Value)
	{
		return Value.ParameterInfo.Name == Name;
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialUndoAddNodeTest, "Cortex.Material.Undo.AddNode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialUndoAddNodeTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	const int32 Before = Fixture.Expressions().Num();
	auto Request = Fixture.MaterialParams();
	Request->SetStringField(TEXT("expression_class"), TEXT("Constant"));
	TestTrue(TEXT("add_node succeeds"), Fixture.Handler.Execute(TEXT("add_node"), Request).bSuccess);
	TestEqual(TEXT("add_node adds one expression"), Fixture.Expressions().Num(), Before + 1);
	if (!TestTrue(TEXT("add_node is the next undo step"), IsNextUndo(TEXT("Cortex: Add Material Expression Node"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo add_node"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo removes the added expression"), Fixture.Expressions().Num(), Before);
	TestTrue(TEXT("Redo add_node"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restores the added expression"), Fixture.Expressions().Num(), Before + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialUndoRemoveNodeTest, "Cortex.Material.Undo.RemoveNode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialUndoRemoveNodeTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	UMaterialEditorOnlyData* Data = Fixture.Material->GetEditorOnlyData();
	auto Request = Fixture.MaterialParams();
	Request->SetStringField(TEXT("node_id"), Fixture.Constant->GetName());
	TestTrue(TEXT("remove_node succeeds"), Fixture.Handler.Execute(TEXT("remove_node"), Request).bSuccess);
	TestFalse(TEXT("Removed expression leaves the collection"), Fixture.Expressions().Contains(Fixture.Constant));
	TestNull(TEXT("Root input no longer references the removed expression"), Data->BaseColor.Expression);
	TestNull(TEXT("Expression input no longer references the removed expression"), Fixture.Multiply->A.Expression);
	if (!TestTrue(TEXT("remove_node is the next undo step"), IsNextUndo(TEXT("Cortex: Remove Material Expression Node"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo remove_node"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores the expression"), Fixture.Expressions().Contains(Fixture.Constant));
	TestTrue(TEXT("Undo restores the root link"), Data->BaseColor.Expression == Fixture.Constant);
	TestTrue(TEXT("Undo restores the expression link"), Fixture.Multiply->A.Expression == Fixture.Constant);
	TestTrue(TEXT("Redo remove_node"), GEditor->RedoTransaction());
	TestFalse(TEXT("Redo removes the expression again"), Fixture.Expressions().Contains(Fixture.Constant));
	TestNull(TEXT("Redo clears the root link"), Data->BaseColor.Expression);
	TestNull(TEXT("Redo clears the expression link"), Fixture.Multiply->A.Expression);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialUndoSetNodePropertyTest, "Cortex.Material.Undo.SetNodeProperty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialUndoSetNodePropertyTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	auto Request = Fixture.MaterialParams();
	Request->SetStringField(TEXT("node_id"), Fixture.Constant->GetName());
	Request->SetStringField(TEXT("property_name"), TEXT("R"));

	// A value the serializer cannot write is refused without touching the expression or the undo history.
	Request->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	TestFalse(TEXT("Wrong-typed value is refused"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	TestEqual(TEXT("Refusal keeps the value"), Fixture.Constant->R, 0.25f);
	TestEqual(TEXT("Refusal records no transaction"), GEditor->Trans->GetQueueLength(), Transactions);

	Request->SetNumberField(TEXT("value"), 0.5);
	TestTrue(TEXT("set_node_property succeeds"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	TestEqual(TEXT("Value written"), Fixture.Constant->R, 0.5f);
	if (!TestTrue(TEXT("set_node_property is the next undo step"), IsNextUndo(TEXT("Cortex: Set Node Property"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo set_node_property"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo restores the value"), Fixture.Constant->R, 0.25f);
	TestTrue(TEXT("Redo set_node_property"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo reapplies the value"), Fixture.Constant->R, 0.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialUndoSetNodePropertyInstancedTest, "Cortex.Material.Undo.SetNodePropertyInstanced", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialUndoSetNodePropertyInstancedTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	UMaterialExpressionColorRamp* Ramp = NewObject<UMaterialExpressionColorRamp>(Fixture.Material);
	if (Ramp->ColorCurve == nullptr)
	{
		Ramp->ColorCurve = NewObject<UCurveLinearColor>(Ramp);
	}
	UCurveLinearColor* Original = Ramp->ColorCurve;
	Fixture.Expressions().Add(Ramp);
	ON_SCOPE_EXIT
	{
		Fixture.Expressions().Remove(Ramp);
		Ramp->MarkAsGarbage();
	};

	auto Request = Fixture.MaterialParams();
	Request->SetStringField(TEXT("node_id"), Ramp->GetName());
	Request->SetStringField(TEXT("property_name"), TEXT("ColorCurve"));
	auto Replacement = MakeShared<FJsonObject>();
	Replacement->SetStringField(TEXT("_class"), TEXT("CurveLinearColor"));
	Request->SetObjectField(TEXT("value"), Replacement);

	// Replacing an instanced sub-object retires the old one; Undo must bring it back alive.
	TestTrue(TEXT("Replacing the instanced curve succeeds"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	UCurveLinearColor* Replaced = Ramp->ColorCurve;
	TestTrue(TEXT("A new curve replaced the original"), Replaced != nullptr && Replaced != Original);
	if (!TestTrue(TEXT("Replacement is the next undo step"), IsNextUndo(TEXT("Cortex: Set Node Property"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo replacement"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores the original curve"), Ramp->ColorCurve == Original);
	TestTrue(TEXT("Undo revives the original curve"), IsValid(Original));
	TestTrue(TEXT("Redo replacement"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo restores the replacement"), Ramp->ColorCurve == Replaced && IsValid(Replaced));
	TestFalse(TEXT("Redo retires the original curve again"), IsValid(Original));

	// Clearing the instanced sub-object is undoable the same way.
	TestTrue(TEXT("Undo replacement again"), GEditor->UndoTransaction());
	Request->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
	TestTrue(TEXT("Clearing the instanced curve succeeds"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	TestNull(TEXT("Curve cleared"), Ramp->ColorCurve.Get());
	TestTrue(TEXT("Undo clear"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores the cleared curve"), Ramp->ColorCurve == Original && IsValid(Original));

	// An independent curve asset assigned by path is referenced, not owned: clearing must never retire it.
	const FString ExternalName = TEXT("Curve_Undo_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* ExternalPackage = CreatePackage(*(TEXT("/Game/Temp/CortexMaterialUndo/") + ExternalName));
	UCurveLinearColor* External = NewObject<UCurveLinearColor>(ExternalPackage, *ExternalName, RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		External->ClearFlags(RF_Public | RF_Standalone);
		External->MarkAsGarbage();
		ExternalPackage->SetDirtyFlag(false);
		ExternalPackage->MarkAsGarbage();
	};
	Request->SetStringField(TEXT("value"), External->GetPathName());
	TestTrue(TEXT("Assigning the external curve succeeds"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	TestTrue(TEXT("External curve assigned"), Ramp->ColorCurve == External);
	Request->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
	TestTrue(TEXT("Clearing the external curve succeeds"), Fixture.Handler.Execute(TEXT("set_node_property"), Request).bSuccess);
	TestTrue(TEXT("Clearing keeps the external curve alive"), IsValid(External));
	TestTrue(TEXT("Undo clear of the external curve"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores the external curve"), Ramp->ColorCurve == External && IsValid(External));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialUndoParametersTest, "Cortex.Material.Undo.Parameters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialUndoParametersTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	const FName Roughness(TEXT("Roughness"));
	const FName Metallic(TEXT("Metallic"));

	TestTrue(TEXT("set_parameter succeeds"), Fixture.SetScalar(Roughness.ToString(), 0.7).bSuccess);
	TestTrue(TEXT("set_parameter adds an override"), HasScalarOverride(Fixture.Instance, Roughness));
	if (!TestTrue(TEXT("set_parameter is the next undo step"), IsNextUndo(TEXT("Cortex: Set Material Parameter"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo set_parameter"), GEditor->UndoTransaction());
	TestFalse(TEXT("Undo removes the override"), HasScalarOverride(Fixture.Instance, Roughness));
	TestTrue(TEXT("Redo set_parameter"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo restores the override"), HasScalarOverride(Fixture.Instance, Roughness));

	auto Reset = Fixture.InstanceParams();
	Reset->SetStringField(TEXT("parameter_name"), Roughness.ToString());
	TestTrue(TEXT("reset_parameter succeeds"), Fixture.Handler.Execute(TEXT("reset_parameter"), Reset).bSuccess);
	TestFalse(TEXT("reset_parameter removes the override"), HasScalarOverride(Fixture.Instance, Roughness));
	if (!TestTrue(TEXT("reset_parameter is the next undo step"), IsNextUndo(TEXT("Cortex: Reset Material Parameter"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo reset_parameter"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores the reset override"), HasScalarOverride(Fixture.Instance, Roughness));

	// set_parameters is a single undo step covering every parameter it set.
	auto Batch = Fixture.InstanceParams();
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FName& Name : {Metallic, FName(TEXT("Specular"))})
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("parameter_name"), Name.ToString());
		Entry->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		Entry->SetNumberField(TEXT("value"), 0.3);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Batch->SetArrayField(TEXT("parameters"), Entries);
	TestTrue(TEXT("set_parameters succeeds"), Fixture.Handler.Execute(TEXT("set_parameters"), Batch).bSuccess);
	if (!TestTrue(TEXT("set_parameters is the next undo step"), IsNextUndo(TEXT("Cortex: Set Material Parameters"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo set_parameters"), GEditor->UndoTransaction());
	TestFalse(TEXT("Undo removes the first override"), HasScalarOverride(Fixture.Instance, Metallic));
	TestFalse(TEXT("Undo removes the second override"), HasScalarOverride(Fixture.Instance, TEXT("Specular")));
	TestTrue(TEXT("Undo keeps the earlier override"), HasScalarOverride(Fixture.Instance, Roughness));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialSetParametersAllFailedTest, "Cortex.Material.Param.SetParametersAllFailed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialSetParametersAllFailedTest::RunTest(const FString& Parameters)
{
	FUndoFixture Fixture;
	auto Request = Fixture.InstanceParams();
	auto Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("parameter_name"), TEXT("Roughness"));
	Entry->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	Entry->SetStringField(TEXT("value"), TEXT("not a number"));
	Request->SetArrayField(TEXT("parameters"), {MakeShared<FJsonValueObject>(Entry)});
	const int32 Transactions = GEditor->Trans->GetQueueLength();
	const FCortexCommandResult Result = Fixture.Handler.Execute(TEXT("set_parameters"), Request);
	TestFalse(TEXT("No parameter set means failure"), Result.bSuccess);
	TestEqual(TEXT("Failure carries the entry error code"), Result.ErrorCode, CortexErrorCodes::InvalidParameter);
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	TestTrue(TEXT("Failure details list each entry error"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->TryGetArrayField(TEXT("errors"), Errors) && Errors->Num() == 1);
	TestEqual(TEXT("Failure records no transaction"), GEditor->Trans->GetQueueLength(), Transactions);

	Request->SetArrayField(TEXT("parameters"), TArray<TSharedPtr<FJsonValue>>());
	TestFalse(TEXT("Empty parameters array is refused"), Fixture.Handler.Execute(TEXT("set_parameters"), Request).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMaterialCreateSaveFailureTest, "Cortex.Material.Asset.CreateSaveFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexMaterialCreateSaveFailureTest::RunTest(const FString& Parameters)
{
	const FString Dir = TEXT("/Game/Temp/CortexMaterialSaveFailure_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FUndoFixture Fixture;
	IFileManager& FileManager = IFileManager::Get();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TArray<UObject*> Created;
	TArray<FString> Blockers;

	// A read-only directory at the package filename is not a package, so the name is free, but SavePackage
	// refuses to replace it. The engine logs each refusal as one error plus one warning; that is the failure under test.
	AddExpectedError(TEXT("as it is read only"), EAutomationExpectedErrorFlags::Contains, 3);
	AddExpectedMessage(TEXT("Error saving"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	auto ExpectSaveFailure = [&](const TCHAR* Command, const FString& Name, TSharedPtr<FJsonObject> Request)
	{
		const FString PackageName = Dir / Name;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		TestTrue(TEXT("Blocking directory created"), FileManager.MakeDirectory(*Filename, true));
		TestTrue(TEXT("Blocking directory made read-only"), PlatformFile.SetReadOnly(*Filename, true));
		Blockers.Add(Filename);
		Request->SetStringField(TEXT("asset_path"), Dir);
		Request->SetStringField(TEXT("name"), Name);
		const FCortexCommandResult Result = Fixture.Handler.Execute(Command, Request);
		TestFalse(FString::Printf(TEXT("%s reports the failed save"), Command), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s uses SAVE_FAILED"), Command), Result.ErrorCode, CortexErrorCodes::SaveFailed);
		if (UObject* Asset = FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)))
		{
			Created.Add(Asset);
		}
	};

	ExpectSaveFailure(TEXT("create_material"), TEXT("M_SaveFailure"), MakeShared<FJsonObject>());
	auto InstanceRequest = MakeShared<FJsonObject>();
	InstanceRequest->SetStringField(TEXT("parent_material"), Fixture.Material->GetPathName());
	ExpectSaveFailure(TEXT("create_instance"), TEXT("MI_SaveFailure"), InstanceRequest);
	ExpectSaveFailure(TEXT("create_collection"), TEXT("MPC_SaveFailure"), MakeShared<FJsonObject>());

	for (UObject* Asset : Created)
	{
		UPackage* Package = Asset->GetPackage();
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
	}
	for (const FString& Blocker : Blockers)
	{
		PlatformFile.SetReadOnly(*Blocker, false);
	}
	FileManager.DeleteDirectory(*FPackageName::LongPackageNameToFilename(Dir, TEXT("")), false, true);
	// The refused save leaves its temp output in Saved/.
	for (const TCHAR* Name : {TEXT("M_SaveFailure"), TEXT("MI_SaveFailure"), TEXT("MPC_SaveFailure")})
	{
		TArray<FString> TempFiles;
		FileManager.FindFiles(TempFiles, *(FPaths::ProjectSavedDir() / FString(Name) + TEXT("*.tmp")), true, false);
		for (const FString& TempFile : TempFiles)
		{
			FileManager.Delete(*(FPaths::ProjectSavedDir() / TempFile));
		}
	}
	return true;
}
