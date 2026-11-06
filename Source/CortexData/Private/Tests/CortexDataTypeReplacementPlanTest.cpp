// #122 data.plan_type_replacement: the CoreRedirects gate (what the generated redirect text must achieve), then the plan itself.

#include "Misc/AutomationTest.h"

#include "CortexDataTypeReplacementTestSupport.h"
#include "CortexDataTypeReplacementTestTypes.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_SwitchEnum.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CortexIssue122DataTest
{
	namespace Gate
	{
		const TCHAR* const Native = TEXT("/Script/CortexData.CortexIssue122Row");

		/** What a reload of the fixture's DataTable and Blueprint produced. */
		struct FReload
		{
			UDataTable* Table = nullptr;
			UBlueprint* Blueprint = nullptr;
			TMap<FName, FRowValues> Rows;
			FRowValues Cdo;
			UScriptStruct* VariableType = nullptr;
		};

		TArray<FCoreRedirect> StructRedirects(const FString& UserPath, const TArray<FString>& GuidNames, const bool bObject, const bool bStruct, const bool bProperty)
		{
			TArray<FCoreRedirect> Redirects;
			if (bObject)
			{
				Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Object, UserPath, Native, TEXT("/Script/CoreUObject.ScriptStruct")));
			}
			if (bStruct)
			{
				Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Struct, UserPath, Native));
			}
			if (bProperty)
			{
				const TCHAR* const NativeMembers[] = { TEXT("Count"), TEXT("Label"), TEXT("bFlag") };
				for (int32 Index = 0; Index < GuidNames.Num(); ++Index)
				{
					Redirects.Add(MakeRedirect(ECoreRedirectFlags::Type_Property, FString::Printf(TEXT("%s.%s"), Native, *GuidNames[Index]), NativeMembers[Index]));
				}
			}
			return Redirects;
		}

		FReload Reload(const FString& TablePath, const FString& BlueprintPath)
		{
			FReload Out;
			Out.Table = FFixture::Reload<UDataTable>(TablePath);
			Out.Blueprint = FFixture::Reload<UBlueprint>(BlueprintPath);
			if (Out.Table != nullptr && Out.Table->GetRowStruct() != nullptr)
			{
				Out.Rows = ReadRows(Out.Table, Out.Table->GetRowStruct() == FCortexIssue122Row::StaticStruct() ? NativeMembers() : FRowMembers());
			}
			if (Out.Blueprint != nullptr)
			{
				if (Out.Blueprint->NewVariables.Num() > 0)
				{
					Out.VariableType = Cast<UScriptStruct>(Out.Blueprint->NewVariables[0].VarType.PinSubCategoryObject.Get());
				}
				if (const FStructProperty* Payload = Out.Blueprint->GeneratedClass ? FindFProperty<FStructProperty>(Out.Blueprint->GeneratedClass, TEXT("Payload")) : nullptr)
				{
					const uint8* Memory = Payload->ContainerPtrToValuePtr<uint8>(Out.Blueprint->GeneratedClass->GetDefaultObject());
					Out.Cdo = ReadRow(Payload->Struct, Memory, Payload->Struct == FCortexIssue122Row::StaticStruct() ? NativeMembers() : FRowMembers());
				}
			}
			return Out;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122RedirectLoadsNativeStructTest,
	"Cortex.Data.TypeReplacement.RedirectLoadsNativeStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122RedirectLoadsNativeStructTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;

	FFixture Fixture;
	UUserDefinedStruct* Uds = Fixture.MakeRowStruct(TEXT("UDS_Row"));
	if (!TestNotNull(TEXT("UDS"), Uds))
	{
		return false;
	}
	const FString UserPath = Uds->GetPathName();
	TArray<FString> GuidNames;
	for (const FStructVariableDescription& Member : FStructureEditorUtils::GetVarDesc(Uds))
	{
		GuidNames.Add(Member.VarName.ToString());
	}
	if (!TestEqual(TEXT("Three members"), GuidNames.Num(), 3))
	{
		return false;
	}

	const FRowValues Row1{ TEXT("1"), TEXT("one"), TEXT("True") };
	const FRowValues Row2{ TEXT("2"), TEXT("two"), TEXT("False") };
	const FRowValues Row3{ TEXT("3"), TEXT("three"), TEXT("True") };
	UDataTable* Table = Fixture.MakeTable(TEXT("DT_Rows"), Uds);
	if (!TestNotNull(TEXT("Table"), Table)
		|| !TestTrue(TEXT("Rows added"), FFixture::AddRow(Table, TEXT("R1"), FRowMembers(), Row1) && FFixture::AddRow(Table, TEXT("R2"), FRowMembers(), Row2) && FFixture::AddRow(Table, TEXT("R3"), FRowMembers(), Row3)))
	{
		return false;
	}

	FStructOnScope Payload(Uds);
	const FRowValues PayloadValues{ TEXT("5"), TEXT("hello"), TEXT("True") };
	SetMember(Uds, Payload.GetStructMemory(), TEXT("Count"), PayloadValues.Count);
	SetMember(Uds, Payload.GetStructMemory(), TEXT("Label"), PayloadValues.Label);
	SetMember(Uds, Payload.GetStructMemory(), TEXT("Flag"), PayloadValues.Flag);
	FString PayloadText;
	Uds->ExportText(PayloadText, Payload.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
	UBlueprint* Blueprint = Fixture.MakeBlueprintWithVariable(TEXT("BP_Holder"), TEXT("Payload"), StructPin(Uds), PayloadText);
	if (!TestNotNull(TEXT("Blueprint"), Blueprint)
		|| !TestTrue(TEXT("Saved"), Fixture.SaveAsset(Uds) && Fixture.SaveAsset(Table) && Fixture.SaveAsset(Blueprint)))
	{
		return false;
	}
	const FString TablePath = Table->GetPathName();
	const FString BlueprintPath = Blueprint->GetPathName();
	const TMap<FName, FRowValues> Baseline = ReadRows(Table, FRowMembers());

	// 0. Control: no redirects, a reload keeps the user struct.
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Table, Blueprint })))
	{
		return false;
	}
	{
		const Gate::FReload Control = Gate::Reload(TablePath, BlueprintPath);
		if (TestNotNull(TEXT("Control table"), Control.Table) && TestNotNull(TEXT("Control blueprint"), Control.Blueprint))
		{
			TestTrue(TEXT("Control: the DataTable still uses the user struct"), Control.Table->GetRowStruct() == Uds);
			TestEqual(TEXT("Control: the CDO default"), Control.Cdo.Count, PayloadValues.Count);
			if (!FFixture::Unload({ Control.Table, Control.Blueprint }))
			{
				return false;
			}
		}
	}

	// 1. Object (+OverrideClassName) + Struct + Property: the whole set converts the import, the tagged default and the rows.
	{
		FRedirectScope Scope(Gate::StructRedirects(UserPath, GuidNames, true, true, true));
		if (!TestTrue(TEXT("Redirects registered"), Scope.bAdded))
		{
			return false;
		}
		const Gate::FReload Full = Gate::Reload(TablePath, BlueprintPath);
		if (TestNotNull(TEXT("Full: table"), Full.Table) && TestNotNull(TEXT("Full: blueprint"), Full.Blueprint))
		{
			TestTrue(TEXT("Full: the DataTable's RowStruct is the native struct"), Full.Table->GetRowStruct() == FCortexIssue122Row::StaticStruct());
			TestEqual(TEXT("Full: three rows"), Full.Rows.Num(), 3);
			for (const TPair<FName, FRowValues>& Pair : Baseline)
			{
				const FRowValues* Reloaded = Full.Rows.Find(Pair.Key);
				if (TestNotNull(*FString::Printf(TEXT("Full: row %s"), *Pair.Key.ToString()), Reloaded))
				{
					TestTrue(*FString::Printf(TEXT("Full: row %s keeps its values"), *Pair.Key.ToString()), *Reloaded == Pair.Value);
				}
			}
			TestTrue(TEXT("Full: the variable type is the native struct"), Full.VariableType == FCortexIssue122Row::StaticStruct());
			FStructOnScope Imported(FCortexIssue122Row::StaticStruct());
			TestNotNull(
				TEXT("Full: the authored default text (GUID member names) imports into the native struct (property redirects)"),
				FCortexIssue122Row::StaticStruct()->ImportText(*PayloadText, Imported.GetStructMemory(), nullptr, PPF_None, GWarn, TEXT("Payload")));
			TestTrue(TEXT("Full: ... with every value"), ReadRow(FCortexIssue122Row::StaticStruct(), Imported.GetStructMemory(), NativeMembers()) == PayloadValues);
			TestTrue(TEXT("Full: the CDO tagged default survived"), Full.Cdo == PayloadValues);
			if (!FFixture::Unload({ Full.Table, Full.Blueprint }))
			{
				return false;
			}
		}
	}

	// 2. StructRedirects alone do not redirect a UDS import (it is a Type_Object import).
	{
		FRedirectScope Scope(Gate::StructRedirects(UserPath, GuidNames, false, true, true));
		const Gate::FReload StructOnly = Gate::Reload(TablePath, BlueprintPath);
		if (TestNotNull(TEXT("Struct only: table"), StructOnly.Table))
		{
			TestTrue(TEXT("Struct only: the import is NOT redirected, the DataTable keeps the user struct"), StructOnly.Table->GetRowStruct() == Uds);
			if (!FFixture::Unload({ StructOnly.Table, StructOnly.Blueprint }))
			{
				return false;
			}
		}
	}

	// 3. Object + Property without the Struct redirect: the DataTable rows survive, the Blueprint CDO's tagged struct default does not.
	{
		// The engine reports the dropped tagged default as a warning; it is the observable consequence this case pins.
		AddExpectedMessagePlain(TEXT("has a struct type mismatch"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		FRedirectScope Scope(Gate::StructRedirects(UserPath, GuidNames, true, false, true));
		const Gate::FReload NoStruct = Gate::Reload(TablePath, BlueprintPath);
		if (TestNotNull(TEXT("No struct redirect: table"), NoStruct.Table) && TestNotNull(TEXT("No struct redirect: blueprint"), NoStruct.Blueprint))
		{
			TestTrue(TEXT("No struct redirect: the DataTable still converts"), NoStruct.Table->GetRowStruct() == FCortexIssue122Row::StaticStruct());
			for (const TPair<FName, FRowValues>& Pair : Baseline)
			{
				const FRowValues* Reloaded = NoStruct.Rows.Find(Pair.Key);
				if (TestNotNull(*FString::Printf(TEXT("No struct redirect: row %s"), *Pair.Key.ToString()), Reloaded))
				{
					TestTrue(*FString::Printf(TEXT("No struct redirect: row %s survives"), *Pair.Key.ToString()), *Reloaded == Pair.Value);
				}
			}
			TestFalse(TEXT("No struct redirect: the CDO tagged default is lost"), NoStruct.Cdo == PayloadValues);
			if (!FFixture::Unload({ NoStruct.Table, NoStruct.Blueprint }))
			{
				return false;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122RedirectLoadsNativeEnumTest,
	"Cortex.Data.TypeReplacement.RedirectLoadsNativeEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122RedirectLoadsNativeEnumTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;

	FFixture Fixture;
	UUserDefinedEnum* Ude = Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
	if (!TestNotNull(TEXT("UDE"), Ude) || !TestEqual(TEXT("Three values and the MAX entry"), Ude->NumEnums(), 4))
	{
		return false;
	}
	const FString UserPath = Ude->GetPathName();
	UEnum* Native = StaticEnum<ECortexIssue122Mode>();
	TMap<FString, FString> ValueChanges;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ValueChanges.Add(Ude->GetNameStringByIndex(Index), Native->GetNameStringByIndex(Index));
	}

	// The variable defaults to the second value; a switch node on the enum has its second case pin linked.
	UBlueprint* Blueprint = Fixture.MakeBlueprintWithVariable(TEXT("BP_Holder"), TEXT("Mode"), EnumPin(Ude), Ude->GetNameStringByIndex(1));
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	FGraphNodeCreator<UK2Node_SwitchEnum> SwitchCreator(*Graph);
	UK2Node_SwitchEnum* Switch = SwitchCreator.CreateNode();
	Switch->Enum = Ude;
	SwitchCreator.Finalize();
	FGraphNodeCreator<UK2Node_ExecutionSequence> SequenceCreator(*Graph);
	UK2Node_ExecutionSequence* Sequence = SequenceCreator.CreateNode();
	SequenceCreator.Finalize();
	UEdGraphPin* CasePin = Switch->FindPin(FName(*Ude->GetNameStringByIndex(1)), EGPD_Output);
	if (!TestNotNull(TEXT("Case pin"), CasePin))
	{
		return false;
	}
	GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(CasePin, Sequence->GetExecPin());
	const FGuid SequenceGuid = Sequence->NodeGuid;
	if (!TestEqual(TEXT("Case pin linked"), CasePin->LinkedTo.Num(), 1)
		|| !TestTrue(TEXT("Saved"), Fixture.SaveAsset(Ude) && Fixture.SaveAsset(Blueprint)))
	{
		return false;
	}
	const FString BlueprintPath = Blueprint->GetPathName();
	if (!TestTrue(TEXT("Unloaded"), FFixture::Unload({ Blueprint })))
	{
		return false;
	}

	{
		FRedirectScope Scope({ MakeEnumRedirect(UserPath, Native->GetPathName(), ValueChanges) });
		if (!TestTrue(TEXT("Redirects registered"), Scope.bAdded))
		{
			return false;
		}
		UBlueprint* Reloaded = FFixture::Reload<UBlueprint>(BlueprintPath);
		if (TestNotNull(TEXT("Reloaded blueprint"), Reloaded))
		{
			TestTrue(TEXT("The variable type is the native enum"), Reloaded->NewVariables.Num() == 1 && Reloaded->NewVariables[0].VarType.PinSubCategoryObject.Get() == Native);
			const FProperty* Mode = Reloaded->GeneratedClass ? Reloaded->GeneratedClass->FindPropertyByName(TEXT("Mode")) : nullptr;
			if (TestNotNull(TEXT("Mode property"), Mode))
			{
				const FNumericProperty* Numeric = CastField<FNumericProperty>(Mode);
				const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Mode);
				const int64 Value = Numeric ? Numeric->GetSignedIntPropertyValue(Mode->ContainerPtrToValuePtr<void>(Reloaded->GeneratedClass->GetDefaultObject()))
					: EnumProperty ? EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Mode->ContainerPtrToValuePtr<void>(Reloaded->GeneratedClass->GetDefaultObject())) : -1;
				TestEqual(TEXT("The CDO default maps NewEnumerator1 to the native B"), Value, static_cast<int64>(ECortexIssue122Mode::B));
			}
			UK2Node_SwitchEnum* ReloadedSwitch = nullptr;
			for (UEdGraphNode* Node : FBlueprintEditorUtils::FindEventGraph(Reloaded)->Nodes)
			{
				if (UK2Node_SwitchEnum* Candidate = Cast<UK2Node_SwitchEnum>(Node))
				{
					ReloadedSwitch = Candidate;
				}
			}
			if (TestNotNull(TEXT("Reloaded switch"), ReloadedSwitch))
			{
				TestTrue(TEXT("The switch enum is the native enum"), ReloadedSwitch->Enum == Native);
				ReloadedSwitch->ReconstructNode();
				const UEdGraphPin* NativeCase = ReloadedSwitch->FindPin(FName(*Native->GetNameStringByIndex(1)), EGPD_Output);
				if (TestNotNull(TEXT("The native case pin exists after reconstruct"), NativeCase)
					&& TestEqual(TEXT("The case pin keeps its link after reconstruct"), NativeCase->LinkedTo.Num(), 1))
				{
					TestEqual(TEXT("... to the same node"), NativeCase->LinkedTo[0]->GetOwningNode()->NodeGuid, SequenceGuid);
				}
			}
			if (!FFixture::Unload({ Reloaded }))
			{
				return false;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122PlanStructTest,
	"Cortex.Data.TypeReplacement.PlanStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122PlanStructTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;

	FFixture Fixture;
	UUserDefinedStruct* Uds = Fixture.MakeRowStruct(TEXT("UDS_Row"));
	if (!TestNotNull(TEXT("UDS"), Uds))
	{
		return false;
	}
	UUserDefinedStruct* Holder = Fixture.MakeStruct(TEXT("UDS_HoldsRow"), { { TEXT("Row"), StructPin(Uds) } });
	UDataTable* Table = Fixture.MakeTable(TEXT("DT_Rows"), Uds);
	UObject* DataAsset = Fixture.MakeDataAssetReferencing(TEXT("DA_Ref"), Uds);
	UWorld* World = Fixture.MakeWorldReferencing(TEXT("L_Ref"), Uds);
	FStructOnScope Payload(Uds);
	SetMember(Uds, Payload.GetStructMemory(), TEXT("Count"), TEXT("5"));
	FString PayloadText;
	Uds->ExportText(PayloadText, Payload.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
	UBlueprint* Blueprint = Fixture.MakeBlueprintWithVariable(TEXT("BP_Holder"), TEXT("Payload"), StructPin(Uds), PayloadText);
	if (!TestNotNull(TEXT("Holder struct"), Holder) || !TestNotNull(TEXT("Table"), Table) || !TestNotNull(TEXT("Data asset"), DataAsset)
		|| !TestNotNull(TEXT("World"), World) || !TestNotNull(TEXT("Blueprint"), Blueprint)
		|| !TestTrue(TEXT("Rows added"), FFixture::AddRow(Table, TEXT("R1"), FRowMembers(), { TEXT("1"), TEXT("one"), TEXT("True") }) && FFixture::AddRow(Table, TEXT("R2"), FRowMembers(), { TEXT("2"), TEXT("two"), TEXT("False") }))
		|| !TestTrue(TEXT("Saved"), Fixture.SaveAsset(Uds) && Fixture.SaveAsset(Holder) && Fixture.SaveAsset(Table) && Fixture.SaveAsset(DataAsset) && Fixture.SaveAsset(Blueprint)
			&& Fixture.SaveAsset(World, *FPackageName::GetMapPackageExtension())))
	{
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString UserPath = Uds->GetPathName();
	const FString NativePath = FCortexIssue122Row::StaticStruct()->GetPathName();
	const FString CountName = UserName(Uds, TEXT("Count"));
	const FString LabelName = UserName(Uds, TEXT("Label"));
	const FString FlagName = UserName(Uds, TEXT("Flag"));
	const FString IniBefore = ConfigFileHash(TEXT("Config/DefaultEngine.ini"));

	// The plan: exact redirect text, mapping (Flag -> bFlag), classified referencers, baseline files, plan file.
	TSharedPtr<FJsonObject> Params = PlanParams(UserPath, NativePath);
	SetStringMap(Params, TEXT("member_map"), { { TEXT("Flag"), TEXT("bFlag") } });
	Params->SetStringField(TEXT("baseline_dir"), Fixture.ScratchPath(TEXT("baseline")));
	Params->SetStringField(TEXT("out_path"), Fixture.ScratchPath(TEXT("plan.json")));
	const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), Params);
	if (!TestTrue(TEXT("plan succeeds"), Plan.bSuccess) || !Plan.Data.IsValid())
	{
		AddError(Plan.ErrorMessage);
		return false;
	}

	const TArray<FString> Expected = {
		FString::Printf(TEXT("+ObjectRedirects=(OldName=\"%s\",NewName=\"%s\",OverrideClassName=\"/Script/CoreUObject.ScriptStruct\")"), *UserPath, *NativePath),
		FString::Printf(TEXT("+StructRedirects=(OldName=\"%s\",NewName=\"%s\")"), *UserPath, *NativePath),
		FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.%s\",NewName=\"Count\")"), *NativePath, *CountName),
		FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.%s\",NewName=\"Label\")"), *NativePath, *LabelName),
		FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.%s\",NewName=\"bFlag\")"), *NativePath, *FlagName),
	};
	const TArray<FString> Lines = Strings(Plan.Data, TEXT("redirects"));
	TestTrue(TEXT("The redirect lines are exactly Object (with OverrideClassName), Struct and one Property per member"), Lines == Expected);
	const TSharedPtr<FJsonObject> MemberMap = Plan.Data->GetObjectField(TEXT("member_map"));
	TestEqual(TEXT("member_map maps the GUID-suffixed Flag to bFlag"), MemberMap->GetStringField(FlagName), FString(TEXT("bFlag")));
	TestEqual(TEXT("... and Count to Count"), MemberMap->GetStringField(CountName), FString(TEXT("Count")));
	TestTrue(TEXT("The GUID-suffixed name is not the friendly name"), FlagName != TEXT("Flag") && FlagName.StartsWith(TEXT("Flag_")));
	TestEqual(TEXT("No native-only members"), Strings(Plan.Data, TEXT("native_only")).Num(), 0);

	// The generated text goes through the engine's own ini parser and measurably changes a reload.
	{
		FRedirectScope Scope(
			{ MakeRedirect(ECoreRedirectFlags::Type_Object, UserPath, NativePath, TEXT("/Script/CoreUObject.ScriptStruct")), MakeRedirect(ECoreRedirectFlags::Type_Struct, UserPath, NativePath),
				MakeRedirect(ECoreRedirectFlags::Type_Property, NativePath + TEXT(".") + CountName, TEXT("Count")), MakeRedirect(ECoreRedirectFlags::Type_Property, NativePath + TEXT(".") + LabelName, TEXT("Label")),
				MakeRedirect(ECoreRedirectFlags::Type_Property, NativePath + TEXT(".") + FlagName, TEXT("bFlag")) },
			/*bAlreadyRegistered=*/true);
		TestTrue(TEXT("The ini reader accepts the generated lines"), RegisterLinesThroughIni(Lines));
		const FCoreRedirectObjectName Old(UserPath);
		TestEqual(TEXT("Object redirect probe"), FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Object, Old).ToString(), NativePath);
		TestEqual(TEXT("Struct redirect probe"), FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Struct, Old).ToString(), NativePath);
		FCoreRedirectObjectName Redirected;
		const FCoreRedirect* ValueRedirect = nullptr;
		FCoreRedirects::RedirectNameAndValues(ECoreRedirectFlags::Type_Object, Old, Redirected, &ValueRedirect);
		if (TestNotNull(TEXT("The object redirect carries its OverrideClassName"), ValueRedirect))
		{
			TestEqual(TEXT("OverrideClassName"), ValueRedirect->OverrideClassName.ToString(), FString(TEXT("/Script/CoreUObject.ScriptStruct")));
		}
		TestEqual(TEXT("Count redirect probe"), FProperty::FindRedirectedPropertyName(FCortexIssue122Row::StaticStruct(), FName(*CountName)), FName(TEXT("Count")));
		TestEqual(TEXT("Label redirect probe"), FProperty::FindRedirectedPropertyName(FCortexIssue122Row::StaticStruct(), FName(*LabelName)), FName(TEXT("Label")));
		TestEqual(TEXT("Flag redirect probe"), FProperty::FindRedirectedPropertyName(FCortexIssue122Row::StaticStruct(), FName(*FlagName)), FName(TEXT("bFlag")));

		// With the redirects present the same plan is refused: running it again would duplicate them.
		const FCortexCommandResult Present = Run(TEXT("plan_type_replacement"), PlanParams(UserPath, NativePath));
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("A plan over an existing redirect is refused"), Present.bSuccess);
		TestTrue(TEXT("... with redirect_present"), ProblemCodes(Present, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("redirect_present"), UserPath));
	}
	TestEqual(TEXT("The test redirects are gone again"), FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Object, FCoreRedirectObjectName(UserPath)).ToString(), UserPath);

	// Affected assets are classified from tags without loading.
	TMap<FString, FString> KindOf;
	for (const TSharedPtr<FJsonObject>& Asset : Objects(Plan.Data, TEXT("affected_assets")))
	{
		KindOf.Add(Asset->GetStringField(TEXT("path")), Asset->GetStringField(TEXT("kind")));
		if (Asset->GetStringField(TEXT("kind")) == TEXT("datatable"))
		{
			TestTrue(TEXT("The DataTable's RowStructure tag names the user struct"), Asset->GetBoolField(TEXT("row_struct")));
		}
	}
	TestEqual(TEXT("Blueprint"), KindOf.FindRef(Blueprint->GetPathName()), FString(TEXT("blueprint")));
	TestEqual(TEXT("DataTable"), KindOf.FindRef(Table->GetPathName()), FString(TEXT("datatable")));
	TestEqual(TEXT("DataAsset"), KindOf.FindRef(DataAsset->GetPathName()), FString(TEXT("data_asset")));
	TestEqual(TEXT("Nested struct"), KindOf.FindRef(Holder->GetPathName()), FString(TEXT("struct")));
	TestEqual(TEXT("World"), KindOf.FindRef(World->GetPathName()), FString(TEXT("world")));
	TestEqual(TEXT("Five affected assets"), static_cast<int32>(Plan.Data->GetNumberField(TEXT("affected_count"))), 5);

	// The baseline was exported with the exporters before any redirect existed, so it carries the raw GUID-suffixed names.
	TArray<FString> BaselineKinds;
	for (const TSharedPtr<FJsonObject>& Entry : Objects(Plan.Data, TEXT("baseline")))
	{
		BaselineKinds.Add(Entry->GetStringField(TEXT("kind")));
		TestTrue(TEXT("The baseline file exists"), IFileManager::Get().FileExists(*FFixture::Absolute(Entry->GetStringField(TEXT("file")))));
		if (Entry->GetStringField(TEXT("kind")) == TEXT("datatable"))
		{
			TestEqual(TEXT("The DataTable baseline belongs to the table"), Entry->GetStringField(TEXT("asset_path")), Table->GetPathName());
			TestTrue(TEXT("The DataTable baseline holds the raw user-struct property names"), ReadFile(Entry->GetStringField(TEXT("file"))).Contains(CountName));
		}
	}
	BaselineKinds.Sort();
	TestTrue(TEXT("A baseline for the DataTable and the DataAsset"), BaselineKinds == TArray<FString>({ TEXT("data_asset"), TEXT("datatable") }));
	TestTrue(TEXT("The plan file exists"), IFileManager::Get().FileExists(*FFixture::Absolute(Plan.Data->GetStringField(TEXT("plan_path")))));
	TSharedPtr<FJsonObject> FromFile;
	TestTrue(TEXT("The plan file parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadFile(Plan.Data->GetStringField(TEXT("plan_path")))), FromFile) && FromFile.IsValid());
	if (FromFile.IsValid())
	{
		TestEqual(TEXT("The plan file carries the plan hash"), FromFile->GetStringField(TEXT("plan_hash")), Plan.Data->GetStringField(TEXT("plan_hash")));
		TestTrue(TEXT("... and the redirect lines"), Strings(FromFile, TEXT("redirects")) == Lines);
	}

	// Identical inputs give an identical plan hash; a different mapping gives another.
	const FCortexCommandResult Again = Run(TEXT("plan_type_replacement"), Params);
	if (TestTrue(TEXT("The same request succeeds again"), Again.bSuccess))
	{
		TestEqual(TEXT("Identical inputs give an identical plan_hash"), Again.Data->GetStringField(TEXT("plan_hash")), Plan.Data->GetStringField(TEXT("plan_hash")));
	}
	TestEqual(TEXT("plan_hash is an MD5 hex digest"), Plan.Data->GetStringField(TEXT("plan_hash")).Len(), 32);
	TestEqual(TEXT("No ini file was touched"), ConfigFileHash(TEXT("Config/DefaultEngine.ini")), IniBefore);

	// Refusals list every problem. An unmatched member, a type-incompatible member and a mapping entry nobody uses in one refusal.
	{
		UUserDefinedStruct* Bad = Fixture.MakeStruct(TEXT("UDS_Bad"), { { TEXT("Count"), IntPin() }, { TEXT("Missing"), IntPin() }, { TEXT("Label"), BoolPin() } });
		TSharedPtr<FJsonObject> BadParams = PlanParams(Bad->GetPathName(), NativePath);
		SetStringMap(BadParams, TEXT("member_map"), { { TEXT("Nobody"), TEXT("Count") } });
		BadParams->SetStringField(TEXT("out_path"), Fixture.ScratchPath(TEXT("bad.plan.json")));
		const FCortexCommandResult Refused = Run(TEXT("plan_type_replacement"), BadParams);
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("A bad struct is refused"), Refused.bSuccess);
		TestEqual(TEXT("... as INVALID_OPERATION"), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("Problems are listed"), ProblemCodes(Refused, Codes, Subjects));
		TestTrue(TEXT("unmatched member"), HasProblem(Codes, Subjects, TEXT("unmatched_member"), TEXT("Missing")));
		TestTrue(TEXT("type-incompatible member"), HasProblem(Codes, Subjects, TEXT("type_incompatible"), TEXT("Label")));
		TestTrue(TEXT("unknown member_map entry"), HasProblem(Codes, Subjects, TEXT("unknown_member_map_entry"), TEXT("Nobody")));
		TestEqual(TEXT("Exactly those three problems"), Codes.Num(), 3);
		TestFalse(TEXT("A refused plan writes no plan file"), IFileManager::Get().FileExists(*FFixture::Absolute(BadParams->GetStringField(TEXT("out_path")))));

		// An int member against a float property.
		const FCortexCommandResult Float = Run(TEXT("plan_type_replacement"), PlanParams(UserPath, FCortexIssue122FloatRow::StaticStruct()->GetPathName()));
		Codes.Reset();
		Subjects.Reset();
		TestFalse(TEXT("int vs float is refused"), Float.bSuccess);
		TestTrue(TEXT("... as type_incompatible on Count"), ProblemCodes(Float, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("type_incompatible"), TEXT("Count")));
	}

	// A native struct that is not a table row while a DataTable uses the user struct.
	{
		TSharedPtr<FJsonObject> PlainParams = PlanParams(UserPath, FCortexIssue122PlainStruct::StaticStruct()->GetPathName());
		SetStringMap(PlainParams, TEXT("member_map"), { { TEXT("Flag"), TEXT("bFlag") } });
		const FCortexCommandResult Plain = Run(TEXT("plan_type_replacement"), PlainParams);
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("A non-table-row native struct is refused while a DataTable uses the user struct"), Plain.bSuccess);
		TestTrue(TEXT("... as native_not_table_row on the table"), ProblemCodes(Plain, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("native_not_table_row"), Table->GetPathName()));
	}

	// A member typed with another user-defined struct needs that struct replaced first; once it is redirected it is native.
	{
		UUserDefinedStruct* InnerUds = Fixture.MakeStruct(TEXT("UDS_InnerValue"), { { TEXT("Value"), IntPin() } });
		UUserDefinedStruct* Outer = Fixture.MakeStruct(TEXT("UDS_Outer"), { { TEXT("Count"), IntPin() }, { TEXT("Inner"), StructPin(InnerUds) } });
		const FString WrapperPath = FCortexIssue122WrapperRow::StaticStruct()->GetPathName();
		const FCortexCommandResult Nested = Run(TEXT("plan_type_replacement"), PlanParams(Outer->GetPathName(), WrapperPath));
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("A nested user-defined struct is refused"), Nested.bSuccess);
		TestTrue(TEXT("... as nested_user_type on Inner"), ProblemCodes(Nested, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("nested_user_type"), TEXT("Inner")));
		// Only a COMPLETE inner contract (Object+class override, Struct, one Property per GUID member) is accepted; a name-only redirect would drop the nested value.
		const FString InnerPath = InnerUds->GetPathName();
		const FString InnerNative = FCortexIssue122Inner::StaticStruct()->GetPathName();
		const TArray<TPair<FString, FString>> InnerMembers = { { UserName(InnerUds, TEXT("Value")), TEXT("Value") } };
		const auto PlanOuter = [&]() -> FCortexCommandResult
		{
			TSharedPtr<FJsonObject> OuterParams = PlanParams(Outer->GetPathName(), WrapperPath);
			OuterParams->SetStringField(TEXT("out_path"), Fixture.ScratchPath(TEXT("outer.plan.json")));
			return Run(TEXT("plan_type_replacement"), OuterParams);
		};
		const auto ExpectNestedRefusal = [&](const TCHAR* Label, const FCortexCommandResult& Result)
		{
			TArray<FString> NestedCodes;
			TArray<FString> NestedSubjects;
			TestFalse(*FString::Printf(TEXT("%s: refused"), Label), Result.bSuccess);
			TestTrue(*FString::Printf(TEXT("%s: as nested_user_type on Inner"), Label), ProblemCodes(Result, NestedCodes, NestedSubjects) && HasProblem(NestedCodes, NestedSubjects, TEXT("nested_user_type"), TEXT("Inner")));
		};
		{
			FRedirectScope ObjectOnly({ MakeRedirect(ECoreRedirectFlags::Type_Object, InnerPath, InnerNative, TEXT("/Script/CoreUObject.ScriptStruct")) });
			ExpectNestedRefusal(TEXT("Object redirect only"), PlanOuter());
		}
		{
			FRedirectScope NameOnly({ MakeRedirect(ECoreRedirectFlags::Type_Object, InnerPath, InnerNative) });
			ExpectNestedRefusal(TEXT("Object redirect without the class override"), PlanOuter());
		}
		{
			TArray<FCoreRedirect> NoStruct;
			for (const FCoreRedirect& Redirect : MakeStructRedirects(InnerPath, InnerNative, InnerMembers))
			{
				if (Redirect.RedirectFlags != ECoreRedirectFlags::Type_Struct)
				{
					NoStruct.Add(Redirect);
				}
			}
			FRedirectScope Scope(NoStruct);
			ExpectNestedRefusal(TEXT("Missing StructRedirects of the inner type"), PlanOuter());
		}
		{
			TArray<FCoreRedirect> NoMembers;
			for (const FCoreRedirect& Redirect : MakeStructRedirects(InnerPath, InnerNative, InnerMembers))
			{
				if (Redirect.RedirectFlags != ECoreRedirectFlags::Type_Property)
				{
					NoMembers.Add(Redirect);
				}
			}
			FRedirectScope Scope(NoMembers);
			ExpectNestedRefusal(TEXT("Missing GUID-member PropertyRedirects of the inner type"), PlanOuter());
		}
		{
			FRedirectScope Full(MakeStructRedirects(InnerPath, InnerNative, InnerMembers));
			const FCortexCommandResult Native = PlanOuter();
			if (TestTrue(TEXT("The plan succeeds with the complete inner contract active"), Native.bSuccess))
			{
				const TArray<TSharedPtr<FJsonObject>> Prerequisites = Objects(Native.Data, TEXT("prerequisites"));
				if (TestEqual(TEXT("... and records it as a prerequisite"), Prerequisites.Num(), 1))
				{
					TestEqual(TEXT("prerequisite user type"), Prerequisites[0]->GetStringField(TEXT("user_type")), InnerPath);
					TestEqual(TEXT("prerequisite native type"), Prerequisites[0]->GetStringField(TEXT("native_type")), InnerNative);
					TestEqual(TEXT("prerequisite GUID member"), Objects(Prerequisites[0], TEXT("members"))[0]->GetStringField(TEXT("user_name")), InnerMembers[0].Key);
				}
			}
			else
			{
				AddError(Native.ErrorMessage);
			}
		}
	}

	// Type and path problems.
	{
		const FCortexCommandResult Hidden = Run(TEXT("plan_type_replacement"), PlanParams(UserPath, FCortexIssue122HiddenRow::StaticStruct()->GetPathName()));
		TestFalse(TEXT("A native struct that is not BlueprintType is refused"), Hidden.bSuccess);
		TestEqual(TEXT("A missing native type is INVALID_FIELD"), Run(TEXT("plan_type_replacement"), PlanParams(UserPath, TEXT("/Script/CortexData.NoSuchStruct"))).ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("A missing user type is ASSET_NOT_FOUND"), Run(TEXT("plan_type_replacement"), PlanParams(TEXT("/Game/Temp/NoSuchUds"), NativePath)).ErrorCode, CortexErrorCodes::AssetNotFound);
		TestEqual(TEXT("A user type that is a DataTable is INVALID_FIELD"), Run(TEXT("plan_type_replacement"), PlanParams(Table->GetPathName(), NativePath)).ErrorCode, CortexErrorCodes::InvalidField);
		TSharedPtr<FJsonObject> WrongMap = PlanParams(UserPath, NativePath);
		SetStringMap(WrongMap, TEXT("value_map"), { { TEXT("A"), TEXT("B") } });
		TestEqual(TEXT("value_map on a struct is INVALID_FIELD"), Run(TEXT("plan_type_replacement"), WrongMap).ErrorCode, CortexErrorCodes::InvalidField);
		TSharedPtr<FJsonObject> Undeclared = PlanParams(UserPath, NativePath);
		Undeclared->SetStringField(TEXT("surprise"), TEXT("x"));
		TestFalse(TEXT("An undeclared parameter is refused"), Run(TEXT("plan_type_replacement"), Undeclared).bSuccess);
	}

	// The default plan file lands under Saved/CortexTypeReplacement.
	{
		TSharedPtr<FJsonObject> NoOut = PlanParams(UserPath, NativePath);
		SetStringMap(NoOut, TEXT("member_map"), { { TEXT("Flag"), TEXT("bFlag") } });
		const FCortexCommandResult Default = Run(TEXT("plan_type_replacement"), NoOut);
		if (TestTrue(TEXT("A request without out_path succeeds"), Default.bSuccess))
		{
			const FString DefaultPath = Default.Data->GetStringField(TEXT("plan_path"));
			TestEqual(TEXT("The default plan path"), DefaultPath, FString(TEXT("Saved/CortexTypeReplacement/UDS_Row.plan.json")));
			TestTrue(TEXT("... exists"), IFileManager::Get().FileExists(*FFixture::Absolute(DefaultPath)));
			IFileManager::Get().Delete(*FFixture::Absolute(DefaultPath));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexIssue122PlanEnumTest,
	"Cortex.Data.TypeReplacement.PlanEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexIssue122PlanEnumTest::RunTest(const FString& Parameters)
{
	using namespace CortexIssue122DataTest;

	FFixture Fixture;
	UUserDefinedEnum* Ude = Fixture.MakeEnum(TEXT("UDE_Mode"), { TEXT("A"), TEXT("B"), TEXT("C") });
	if (!TestNotNull(TEXT("UDE"), Ude))
	{
		return false;
	}
	const FString UserPath = Ude->GetPathName();
	const FString NativePath = StaticEnum<ECortexIssue122Mode>()->GetPathName();
	const FString Token0 = Ude->GetNameStringByIndex(0);
	const FString Token1 = Ude->GetNameStringByIndex(1);
	const FString Token2 = Ude->GetNameStringByIndex(2);

	// Display names match the native enumerators by default; the _MAX entry is excluded.
	const FCortexCommandResult Plan = Run(TEXT("plan_type_replacement"), PlanParams(UserPath, NativePath));
	if (!TestTrue(TEXT("plan succeeds"), Plan.bSuccess) || !Plan.Data.IsValid())
	{
		AddError(Plan.ErrorMessage);
		return false;
	}
	const FString ExpectedLine = FString::Printf(
		TEXT("+EnumRedirects=(OldName=\"%s\",NewName=\"%s\",OverrideClassName=\"/Script/CoreUObject.Enum\",ValueChanges=((\"%s\",\"A\"),(\"%s\",\"B\"),(\"%s\",\"C\")))"),
		*UserPath, *NativePath, *Token0, *Token1, *Token2);
	const TArray<FString> Lines = Strings(Plan.Data, TEXT("redirects"));
	TestTrue(TEXT("One EnumRedirects line with OverrideClassName and the three ValueChanges (no _MAX)"), Lines == TArray<FString>({ ExpectedLine }));
	TestEqual(TEXT("value_map"), Plan.Data->GetObjectField(TEXT("value_map"))->GetStringField(Token1), FString(TEXT("B")));
	TestEqual(TEXT("Three values"), Objects(Plan.Data, TEXT("values")).Num(), 3);
	TestEqual(TEXT("kind"), Plan.Data->GetStringField(TEXT("kind")), FString(TEXT("enum")));
	IFileManager::Get().Delete(*FFixture::Absolute(Plan.Data->GetStringField(TEXT("plan_path"))));

	// The text parses through the engine's ini reader and registers the type redirect and the value changes.
	{
		UEnum* Native = StaticEnum<ECortexIssue122Mode>();
		TMap<FString, FString> Changes;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Changes.Add(Ude->GetNameStringByIndex(Index), Native->GetNameStringByIndex(Index));
		}
		FRedirectScope Scope({ MakeEnumRedirect(UserPath, NativePath, Changes) }, /*bAlreadyRegistered=*/true);
		TestTrue(TEXT("The ini reader accepts the generated line"), RegisterLinesThroughIni(Lines));
		const FCoreRedirectObjectName Old(UserPath);
		FCoreRedirectObjectName Redirected;
		const FCoreRedirect* ValueRedirect = nullptr;
		FCoreRedirects::RedirectNameAndValues(ECoreRedirectFlags::Type_Enum, Old, Redirected, &ValueRedirect);
		TestEqual(TEXT("Enum redirect probe"), Redirected.ToString(), NativePath);
		if (TestNotNull(TEXT("Value redirect"), ValueRedirect))
		{
			TestEqual(TEXT("OverrideClassName"), ValueRedirect->OverrideClassName.ToString(), FString(TEXT("/Script/CoreUObject.Enum")));
		}
		const TMap<FString, FString>* Values = FCoreRedirects::GetValueRedirects(ECoreRedirectFlags::Type_Enum, FCoreRedirectObjectName(Native));
		if (TestNotNull(TEXT("The native enum has value redirects"), Values))
		{
			TestEqual(TEXT("ValueChanges maps the old token to the native enumerator"), Values->FindRef(Token1), FString(TEXT("B")));
			TestEqual(TEXT("... for every value"), Values->Num(), 3);
		}
		// The measurable effect: the engine resolves the old token on the native enum.
		TestEqual(TEXT("The native enum resolves the old token through the redirect"), Native->GetIndexByNameString(Token1), 1);
	}

	// An explicit value_map by token or by display name.
	{
		UUserDefinedEnum* Odd = Fixture.MakeEnum(TEXT("UDE_Odd"), { TEXT("Alpha"), TEXT("Bravo"), TEXT("Charlie") });
		const FCortexCommandResult Unmapped = Run(TEXT("plan_type_replacement"), PlanParams(Odd->GetPathName(), NativePath));
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("Display names that match nothing are refused"), Unmapped.bSuccess);
		TestTrue(TEXT("... as unmatched_value for each value"), ProblemCodes(Unmapped, Codes, Subjects) && Codes.Num() == 3 && HasProblem(Codes, Subjects, TEXT("unmatched_value"), Odd->GetNameStringByIndex(0)));

		TSharedPtr<FJsonObject> Mapped = PlanParams(Odd->GetPathName(), NativePath);
		SetStringMap(Mapped, TEXT("value_map"), { { TEXT("Alpha"), TEXT("A") }, { Odd->GetNameStringByIndex(1), TEXT("B") }, { TEXT("Charlie"), TEXT("C") } });
		const FCortexCommandResult Result = Run(TEXT("plan_type_replacement"), Mapped);
		if (TestTrue(TEXT("An explicit value_map by display name and by token succeeds"), Result.bSuccess))
		{
			TestEqual(TEXT("Bravo -> B"), Result.Data->GetObjectField(TEXT("value_map"))->GetStringField(Odd->GetNameStringByIndex(1)), FString(TEXT("B")));
			IFileManager::Get().Delete(*FFixture::Absolute(Result.Data->GetStringField(TEXT("plan_path"))));
		}
		else
		{
			AddError(Result.ErrorMessage);
		}

		SetStringMap(Mapped, TEXT("value_map"), { { TEXT("Alpha"), TEXT("A") }, { TEXT("Bravo"), TEXT("B") }, { TEXT("Charlie"), TEXT("C") }, { TEXT("Nobody"), TEXT("A") } });
		const FCortexCommandResult Unknown = Run(TEXT("plan_type_replacement"), Mapped);
		Codes.Reset();
		Subjects.Reset();
		TestFalse(TEXT("A value_map entry nobody uses is refused"), Unknown.bSuccess);
		TestTrue(TEXT("... as unknown_value_map_entry"), ProblemCodes(Unknown, Codes, Subjects) && HasProblem(Codes, Subjects, TEXT("unknown_value_map_entry"), TEXT("Nobody")));
	}

	// Numbers must be equal: the stored value would change meaning.
	{
		const FCortexCommandResult Shifted = Run(TEXT("plan_type_replacement"), PlanParams(UserPath, StaticEnum<ECortexIssue122Shifted>()->GetPathName()));
		TArray<FString> Codes;
		TArray<FString> Subjects;
		TestFalse(TEXT("A native enum with other numeric values is refused"), Shifted.bSuccess);
		TestTrue(TEXT("... as value_mismatch for each value"), ProblemCodes(Shifted, Codes, Subjects) && Codes.Num() == 3 && HasProblem(Codes, Subjects, TEXT("value_mismatch"), Token0) && HasProblem(Codes, Subjects, TEXT("value_mismatch"), Token2));
	}

	// Enum-only and struct-only parameters are kept apart.
	{
		TSharedPtr<FJsonObject> WrongMap = PlanParams(UserPath, NativePath);
		SetStringMap(WrongMap, TEXT("member_map"), { { TEXT("A"), TEXT("B") } });
		TestEqual(TEXT("member_map on an enum is INVALID_FIELD"), Run(TEXT("plan_type_replacement"), WrongMap).ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("A struct as the native type of an enum is INVALID_FIELD"), Run(TEXT("plan_type_replacement"), PlanParams(UserPath, FCortexIssue122Row::StaticStruct()->GetPathName())).ErrorCode, CortexErrorCodes::InvalidField);
	}
	return true;
}
