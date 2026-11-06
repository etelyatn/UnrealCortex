#include "Misc/AutomationTest.h"

#include "CortexBPAdoptTestTypes.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Operations/CortexBPSaveGameGuard.h"
#include "UObject/UnrealType.h"

namespace
{
	FBPVariableDescription MakeVariable(const TCHAR* Name, const FName PinCategory, bool bSaveGame)
	{
		FBPVariableDescription Variable;
		Variable.VarName = Name;
		Variable.VarType.PinCategory = PinCategory;
		Variable.PropertyFlags = CPF_Edit | CPF_BlueprintVisible | (bSaveGame ? CPF_SaveGame : 0);
		return Variable;
	}

	const FProperty* Native(const TCHAR* Name)
	{
		return ACortexBPAdoptParent::StaticClass()->FindPropertyByName(Name);
	}

	FString Joined(const TArray<FString>& Strings)
	{
		return FString::Join(Strings, TEXT(","));
	}

	TArray<FString> Kinds(const TArray<FCortexBPSaveGameGuard::FFinding>& Findings)
	{
		TArray<FString> Result;
		for (const FCortexBPSaveGameGuard::FFinding& Finding : Findings)
		{
			Result.Add(Finding.Kind);
		}
		Result.Sort();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSaveGameGuardTest,
	"Cortex.Blueprint.SaveGame.Guard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSaveGameGuardTest::RunTest(const FString& Parameters)
{
	using FFinding = FCortexBPSaveGameGuard::FFinding;

	struct FCase
	{
		const TCHAR* Label;
		FBPVariableDescription Variable;
		const FProperty* NativeProperty;
		TArray<FString> ExpectedKinds;
	};

	const FName Boolean = UEdGraphSchema_K2::PC_Boolean;
	const FName String = UEdGraphSchema_K2::PC_String;
	const FName Int = UEdGraphSchema_K2::PC_Int;

	for (const TCHAR* Name : { TEXT("bPowerOn"), TEXT("bIsOpen"), TEXT("Speed"), TEXT("SavedName"), TEXT("TypeMismatch"), TEXT("bNotSave") })
	{
		if (!TestNotNull(FString::Printf(TEXT("Native fixture property %s"), Name), Native(Name)))
		{
			return false;
		}
	}

	const TArray<FCase> Cases = {
		{TEXT("same name, same type, SaveGame kept"), MakeVariable(TEXT("bPowerOn"), Boolean, true), Native(TEXT("bPowerOn")), {}},
		{TEXT("exact SaveGame string"), MakeVariable(TEXT("SavedName"), String, true), Native(TEXT("SavedName")), {}},
		{TEXT("native without SaveGame"), MakeVariable(TEXT("bNotSave"), Boolean, true), Native(TEXT("bNotSave")), {TEXT("save_game_flag_missing")}},
		{TEXT("different name"), MakeVariable(TEXT("bRenamedPower"), Boolean, true), Native(TEXT("bPowerOn")), {TEXT("save_game_name_changed")}},
		{TEXT("different type"), MakeVariable(TEXT("SavedName"), Int, true), Native(TEXT("SavedName")), {TEXT("save_game_type_changed")}},
		{TEXT("plain removal"), MakeVariable(TEXT("bGone"), Boolean, true), nullptr, {TEXT("save_game_removed")}},
		{TEXT("rename and flag missing raise both"), MakeVariable(TEXT("bOpen"), Boolean, true), Native(TEXT("bIsOpen")),
			{TEXT("save_game_flag_missing"), TEXT("save_game_name_changed")}},
		{TEXT("non-SaveGame removal never raises"), MakeVariable(TEXT("bPlain"), Boolean, false), nullptr, {}},
		{TEXT("non-SaveGame type change never raises"), MakeVariable(TEXT("TypeMismatch"), String, false), Native(TEXT("TypeMismatch")), {}},
		{TEXT("non-SaveGame rename never raises"), MakeVariable(TEXT("bOpen"), Boolean, false), Native(TEXT("bIsOpen")), {}},
	};

	for (const FCase& Case : Cases)
	{
		TArray<FFinding> Findings;
		FCortexBPSaveGameGuard::Evaluate(Case.Variable, Case.NativeProperty, Findings);
		TestEqual(FString::Printf(TEXT("%s: finding kinds"), Case.Label), Joined(Kinds(Findings)), Joined(Case.ExpectedKinds));
		for (const FFinding& Finding : Findings)
		{
			TestEqual(FString::Printf(TEXT("%s: variable"), Case.Label), Finding.Variable, Case.Variable.VarName.ToString());
			TestEqual(FString::Printf(TEXT("%s: key"), Case.Label), Finding.AcknowledgmentKey(), FString(TEXT("save_game:")) + Case.Variable.VarName.ToString());
			TestEqual(FString::Printf(TEXT("%s: native property"), Case.Label),
				Finding.NativeProperty, Case.NativeProperty ? Case.NativeProperty->GetName() : FString());
		}
	}

	// Acknowledgment keys: sorted, unique, exact.
	TArray<FFinding> Findings;
	FCortexBPSaveGameGuard::Evaluate(MakeVariable(TEXT("bZeta"), Boolean, true), nullptr, Findings);
	FCortexBPSaveGameGuard::Evaluate(MakeVariable(TEXT("bAlpha"), Boolean, true), nullptr, Findings);
	FCortexBPSaveGameGuard::Evaluate(MakeVariable(TEXT("bOpen"), Boolean, true), Native(TEXT("bIsOpen")), Findings); // two findings, one key
	const TArray<FString> Required = FCortexBPSaveGameGuard::RequiredAcknowledgment(Findings);
	const TArray<FString> ExpectedRequired = {TEXT("save_game:bAlpha"), TEXT("save_game:bOpen"), TEXT("save_game:bZeta")};
	TestEqual(TEXT("Required keys are sorted and unique"), Joined(Required), Joined(ExpectedRequired));

	FCortexCommandResult Error;
	TestTrue(TEXT("Exact acknowledgment passes"), FCortexBPSaveGameGuard::Check(Findings, ExpectedRequired, Error));
	TestTrue(TEXT("Order does not matter"),
		FCortexBPSaveGameGuard::Check(Findings, {TEXT("save_game:bZeta"), TEXT("save_game:bAlpha"), TEXT("save_game:bOpen")}, Error));

	TestFalse(TEXT("Missing acknowledgment refuses"), FCortexBPSaveGameGuard::Check(Findings, {}, Error));
	TestEqual(TEXT("Refusal code"), Error.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	const TArray<TSharedPtr<FJsonValue>>* RequiredJson = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* FindingsJson = nullptr;
	if (TestTrue(TEXT("Refusal details"), Error.ErrorDetails.IsValid()))
	{
		if (TestTrue(TEXT("required_acknowledgment"), Error.ErrorDetails->TryGetArrayField(TEXT("required_acknowledgment"), RequiredJson)))
		{
			TArray<FString> Reported;
			for (const TSharedPtr<FJsonValue>& Value : *RequiredJson)
			{
				Reported.Add(Value->AsString());
			}
			TestEqual(TEXT("required_acknowledgment is the sorted set"), Joined(Reported), Joined(ExpectedRequired));
		}
		if (TestTrue(TEXT("findings"), Error.ErrorDetails->TryGetArrayField(TEXT("findings"), FindingsJson)))
		{
			TestEqual(TEXT("Every finding is listed"), FindingsJson->Num(), Findings.Num());
			const TSharedPtr<FJsonObject> First = (*FindingsJson)[0]->AsObject();
			TestTrue(TEXT("finding carries variable and kind"), First.IsValid() && First->HasField(TEXT("variable")) && First->HasField(TEXT("kind")));
		}
	}

	TestFalse(TEXT("Partial acknowledgment refuses"),
		FCortexBPSaveGameGuard::Check(Findings, {TEXT("save_game:bAlpha"), TEXT("save_game:bZeta")}, Error));
	TestFalse(TEXT("Extra key refuses"),
		FCortexBPSaveGameGuard::Check(Findings, {TEXT("save_game:bAlpha"), TEXT("save_game:bOpen"), TEXT("save_game:bZeta"), TEXT("save_game:bExtra")}, Error));
	TestFalse(TEXT("Stale key refuses"),
		FCortexBPSaveGameGuard::Check(Findings, {TEXT("save_game:bAlpha"), TEXT("save_game:bOpen"), TEXT("save_game:bStale")}, Error));
	TestEqual(TEXT("Stale refusal code"), Error.ErrorCode, CortexErrorCodes::PotentialDataLoss);

	// rename_member input.
	{
		TArray<FFinding> Renames;
		FCortexBPSaveGameGuard::EvaluateRename(MakeVariable(TEXT("bOld"), Boolean, true), TEXT("bNew"), Renames);
		TestEqual(TEXT("SaveGame rename raises one finding"), Renames.Num(), 1);
		if (Renames.Num() == 1)
		{
			TestEqual(TEXT("Rename kind"), Renames[0].Kind, FString(TEXT("save_game_name_changed")));
			TestEqual(TEXT("Rename key is the old name"), Renames[0].AcknowledgmentKey(), FString(TEXT("save_game:bOld")));
			TestEqual(TEXT("Rename target"), Renames[0].NativeProperty, FString(TEXT("bNew")));
		}
		TArray<FFinding> NoRaise;
		FCortexBPSaveGameGuard::EvaluateRename(MakeVariable(TEXT("bPlain"), Boolean, false), TEXT("bOther"), NoRaise);
		FCortexBPSaveGameGuard::EvaluateRename(MakeVariable(TEXT("bSame"), Boolean, true), TEXT("bSame"), NoRaise);
		TestEqual(TEXT("Non-SaveGame or same-name rename raises nothing"), NoRaise.Num(), 0);
	}

	// No findings: nothing to acknowledge, and nothing stale may be carried.
	const TArray<FFinding> None;
	TestTrue(TEXT("No findings, no acknowledgment"), FCortexBPSaveGameGuard::Check(None, {}, Error));
	TestFalse(TEXT("No findings, stale acknowledgment refuses"), FCortexBPSaveGameGuard::Check(None, {TEXT("save_game:bOld")}, Error));
	TestEqual(TEXT("Stale-on-empty code"), Error.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	return true;
}
