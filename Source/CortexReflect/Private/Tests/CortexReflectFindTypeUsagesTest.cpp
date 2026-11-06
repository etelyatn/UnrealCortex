// reflect.find_type_usages: the classified usage inventory of a user-defined struct / enum and of native types.

#include "Misc/AutomationTest.h"

#include "CortexReflectIssue122TestSupport.h"
#include "CortexReflectIssue122TestTypes.h"
#include "CortexTypes.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/SaveGame.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_SwitchEnum.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/UserDefinedStruct.h"

namespace CortexIssue122TypeUsagesTest
{
	using namespace CortexIssue122ReflectTest;

	TSharedPtr<FJsonObject> RequestParams(const FString& Type, const FString& Prefix)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("user_type"), Type);
		Result->SetArrayField(TEXT("path_prefixes"), JsonValues({Prefix}));
		return Result;
	}

	TArray<TSharedPtr<FJsonObject>> InCategory(const TSharedPtr<FJsonObject>& Data, const FString& Category)
	{
		return Objects(Data, TEXT("usages")).FilterByPredicate(
			[&Category](const TSharedPtr<FJsonObject>& Usage) { return Usage->GetStringField(TEXT("category")) == Category; });
	}

	TSharedPtr<FJsonObject> OfAsset(const TArray<TSharedPtr<FJsonObject>>& Usages, const FString& AssetPath)
	{
		for (const TSharedPtr<FJsonObject>& Usage : Usages)
		{
			if (Usage->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Usage;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> OfNode(const TArray<TSharedPtr<FJsonObject>>& Usages, const FGuid& NodeGuid)
	{
		for (const TSharedPtr<FJsonObject>& Usage : Usages)
		{
			if (Usage->HasField(TEXT("node_guid")) && Usage->GetStringField(TEXT("node_guid")) == NodeGuid.ToString())
			{
				return Usage;
			}
		}
		return nullptr;
	}

	FEdGraphPinType StructPin(UObject* Struct, const EPinContainerType Container = EPinContainerType::None)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = Struct;
		PinType.ContainerType = Container;
		return PinType;
	}

	FEdGraphPinType EnumPin(UObject* Enum)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		PinType.PinSubCategoryObject = Enum;
		return PinType;
	}

	UUserDefinedStruct* MakeStruct(FFixture& Fixture, const FString& Name, const FEdGraphPinType& ExtraMember)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Fixture.Dir, *Name));
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		if (Struct)
		{
			FStructureEditorUtils::AddVariable(Struct, ExtraMember);
			const TArray<FStructVariableDescription>& Members = FStructureEditorUtils::GetVarDesc(Struct);
			if (Members.Num() > 0)
			{
				FStructureEditorUtils::RenameVariable(Struct, Members[0].VarGuid, TEXT("Damage"));
			}
		}
		return Struct;
	}

	UUserDefinedEnum* MakeEnum(FFixture& Fixture, const FString& Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Fixture.Dir, *Name));
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional));
		if (Enum)
		{
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
		}
		return Enum;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindTypeUsagesTest,
	"Cortex.Reflect.FindTypeUsages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindTypeUsagesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122TypeUsagesTest;
	FFixture Fixture;

	FEdGraphPinType IntPin;
	IntPin.PinCategory = UEdGraphSchema_K2::PC_Int;
	UUserDefinedStruct* Hit = MakeStruct(Fixture, TEXT("UDS_Hit"), IntPin);
	UUserDefinedEnum* Mode = MakeEnum(Fixture, TEXT("UDE_Mode"));
	UUserDefinedEnum* Unused = MakeEnum(Fixture, TEXT("UDE_Unused"));
	if (!TestNotNull(TEXT("UDS"), Hit) || !TestNotNull(TEXT("UDE"), Mode) || !TestNotNull(TEXT("Unused UDE"), Unused))
	{
		return false;
	}
	UUserDefinedStruct* Wrapper = MakeStruct(Fixture, TEXT("UDS_Wrapper"), StructPin(Hit));
	if (!TestNotNull(TEXT("Wrapper UDS"), Wrapper))
	{
		return false;
	}
	for (UObject* Asset : {static_cast<UObject*>(Hit), static_cast<UObject*>(Mode), static_cast<UObject*>(Unused), static_cast<UObject*>(Wrapper)})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}

	// One Blueprint carries variables, a function with a parameter and a local, and the struct/enum nodes.
	UBlueprint* User = Fixture.MakeBlueprint(TEXT("BP_User"), AActor::StaticClass());
	UBlueprint* SaveGame = Fixture.MakeBlueprint(TEXT("BP_Save"), USaveGame::StaticClass());
	UBlueprint* DataAssetClass = Fixture.MakeBlueprint(TEXT("BP_DataAssetClass"), UDataAsset::StaticClass());
	UBlueprint* Unreadable = Fixture.MakeBlueprint(TEXT("BP_Unreadable"), AActor::StaticClass());
	if (!TestNotNull(TEXT("User"), User) || !TestNotNull(TEXT("SaveGame"), SaveGame) || !TestNotNull(TEXT("DataAsset class"), DataAssetClass)
		|| !TestNotNull(TEXT("Unreadable"), Unreadable))
	{
		return false;
	}
	TestTrue(TEXT("Struct variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Plain"), StructPin(Hit)));
	TestTrue(TEXT("Array variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Many"), StructPin(Hit, EPinContainerType::Array)));
	FEdGraphPinType MapPin = IntPin;
	MapPin.ContainerType = EPinContainerType::Map;
	MapPin.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Struct;
	MapPin.PinValueType.TerminalSubCategoryObject = Hit;
	TestTrue(TEXT("Map variable (struct values)"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("ByIndex"), MapPin));
	TestTrue(TEXT("Enum variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Current"), EnumPin(Mode)));
	TestTrue(TEXT("SaveGame variable"), FBlueprintEditorUtils::AddMemberVariable(SaveGame, TEXT("Saved"), StructPin(Hit)));
	TestTrue(TEXT("DataAsset class variable"), FBlueprintEditorUtils::AddMemberVariable(DataAssetClass, TEXT("Hit"), StructPin(Hit)));
	TestTrue(TEXT("Unreadable variable"), FBlueprintEditorUtils::AddMemberVariable(Unreadable, TEXT("Lost"), StructPin(Hit)));
	FKismetEditorUtilities::CompileBlueprint(DataAssetClass);

	UEdGraph* Function = FFixture::AddFunctionGraphTo(User, TEXT("Compute"), false);
	if (!TestNotNull(TEXT("Function graph"), Function))
	{
		return false;
	}
	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : Function->Nodes)
	{
		if (!Entry)
		{
			Entry = Cast<UK2Node_FunctionEntry>(Node);
		}
	}
	if (!TestNotNull(TEXT("Function entry"), Entry))
	{
		return false;
	}
	Entry->CreateUserDefinedPin(TEXT("InHit"), StructPin(Hit), EGPD_Output, false);
	TestTrue(TEXT("Local variable"), FBlueprintEditorUtils::AddLocalVariable(User, Function, TEXT("LocalHit"), StructPin(Hit)));

	UEdGraph* Graph = FFixture::EventGraph(User);
	FGraphNodeCreator<UK2Node_MakeStruct> MakeCreator(*Graph);
	UK2Node_MakeStruct* MakeNode = MakeCreator.CreateNode();
	MakeNode->StructType = Hit;
	MakeCreator.Finalize();
	FGraphNodeCreator<UK2Node_BreakStruct> BreakCreator(*Graph);
	UK2Node_BreakStruct* BreakNode = BreakCreator.CreateNode();
	BreakNode->StructType = Hit;
	BreakCreator.Finalize();
	FGraphNodeCreator<UK2Node_SetFieldsInStruct> SetCreator(*Graph);
	UK2Node_SetFieldsInStruct* SetNode = SetCreator.CreateNode();
	SetNode->StructType = Hit;
	SetCreator.Finalize();
	FGraphNodeCreator<UK2Node_SwitchEnum> SwitchCreator(*Graph);
	UK2Node_SwitchEnum* SwitchNode = SwitchCreator.CreateNode();
	SwitchNode->SetEnum(Mode);
	SwitchCreator.Finalize();
	FGraphNodeCreator<UK2Node_EnumLiteral> LiteralCreator(*Graph);
	UK2Node_EnumLiteral* LiteralNode = LiteralCreator.CreateNode();
	LiteralNode->Enum = Mode;
	LiteralCreator.Finalize();

	// A UDataAsset instance of a native class holding hard references, and an instance of the Blueprint DataAsset class.
	UCortexIssue122TypeRefAsset* NativeAsset = NewObject<UCortexIssue122TypeRefAsset>(
		CreatePackage(*FString::Printf(TEXT("%s/DA_Native"), *Fixture.Dir)), TEXT("DA_Native"), RF_Public | RF_Standalone);
	NativeAsset->StructRef = Hit;
	NativeAsset->EnumRef = Mode;
	UDataAsset* BlueprintAsset = NewObject<UDataAsset>(
		CreatePackage(*FString::Printf(TEXT("%s/DA_FromBlueprint"), *Fixture.Dir)), DataAssetClass->GeneratedClass, TEXT("DA_FromBlueprint"), RF_Public | RF_Standalone);
	UDataTable* Table = NewObject<UDataTable>(CreatePackage(*FString::Printf(TEXT("%s/DT_Hits"), *Fixture.Dir)), TEXT("DT_Hits"), RF_Public | RF_Standalone);
	Table->RowStruct = Hit;

	for (UObject* Asset : {static_cast<UObject*>(User), static_cast<UObject*>(SaveGame), static_cast<UObject*>(DataAssetClass), static_cast<UObject*>(Unreadable),
		static_cast<UObject*>(NativeAsset), static_cast<UObject*>(BlueprintAsset), static_cast<UObject*>(Table)})
	{
		TestTrue(*FString::Printf(TEXT("%s saved"), *Asset->GetName()), Fixture.SaveAsset(Asset));
	}
	Fixture.RegisterSavedFiles();

	// The table is found from its registry tag without being loaded; the unreadable Blueprint's package is corrupted.
	const FString TablePath = Table->GetPathName();
	const FString UnreadablePath = Unreadable->GetPathName();
	FString UnreadableFile;
	FPackageName::TryConvertLongPackageNameToFilename(Unreadable->GetOutermost()->GetName(), UnreadableFile, FPackageName::GetAssetPackageExtension());
	if (!TestTrue(TEXT("The table and the unreadable Blueprint unload"), FFixture::Unload({Table, Unreadable}))
		|| !TestNull(TEXT("... the table is not in memory"), FindObject<UDataTable>(nullptr, *TablePath)))
	{
		return false;
	}
	// Its footer is destroyed: the registry still reads the header and lists it, the loader refuses the package.
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("The unreadable Blueprint's file is read"), FFileHelper::LoadFileToArray(Bytes, *UnreadableFile)) || !TestTrue(TEXT("... and is long enough"), Bytes.Num() > 16))
	{
		return false;
	}
	for (int32 Index = Bytes.Num() - 4; Index < Bytes.Num(); ++Index)
	{
		Bytes[Index] = 0;
	}
	TestTrue(TEXT("The unreadable Blueprint's file is corrupted"), FFileHelper::SaveArrayToFile(Bytes, *UnreadableFile));
	AddExpectedErrorPlain(TEXT("BP_Unreadable"), EAutomationExpectedErrorFlags::Contains, -1);

	const FCortexCommandResult Result = Run(TEXT("find_type_usages"), RequestParams(Hit->GetPathName(), Fixture.Dir));
	if (!TestTrue(TEXT("find_type_usages succeeds"), Result.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Data = Result.Data;
	TestNull(TEXT("The table was not loaded to be classified"), FindObject<UDataTable>(nullptr, *TablePath));
	TestEqual(TEXT("kind"), Data->GetStringField(TEXT("kind")), FString(TEXT("struct")));
	TestFalse(TEXT("A user-defined struct is not native"), Data->GetBoolField(TEXT("native")));
	TestEqual(TEXT("type_path"), Data->GetStringField(TEXT("type_path")), Hit->GetPathName());

	// variable: plain, array, map value in the user Blueprint; the SaveGame and DataAsset class declare it too.
	const TArray<TSharedPtr<FJsonObject>> Variables = InCategory(Data, TEXT("variable"));
	TestEqual(TEXT("Five member variables"), Variables.Num(), 5);
	TMap<FString, TSharedPtr<FJsonObject>> ByName;
	for (const TSharedPtr<FJsonObject>& Variable : Variables)
	{
		if (Variable->GetStringField(TEXT("asset_path")) == User->GetPathName())
		{
			ByName.Add(Variable->GetStringField(TEXT("variable")), Variable);
		}
	}
	TestEqual(TEXT("Three variables in the user Blueprint"), ByName.Num(), 3);
	if (ByName.Contains(TEXT("Plain")) && ByName.Contains(TEXT("Many")) && ByName.Contains(TEXT("ByIndex")))
	{
		TestFalse(TEXT("Plain has no container"), ByName[TEXT("Plain")]->HasField(TEXT("container")));
		TestEqual(TEXT("Many is an array"), ByName[TEXT("Many")]->GetStringField(TEXT("container")), FString(TEXT("array")));
		TestEqual(TEXT("ByIndex is a map"), ByName[TEXT("ByIndex")]->GetStringField(TEXT("container")), FString(TEXT("map")));
		TestEqual(TEXT("... of the struct as value"), ByName[TEXT("ByIndex")]->GetStringField(TEXT("role")), FString(TEXT("map_value")));
	}
	TestNull(TEXT("The enum variable is not a use of the struct"), ByName.FindRef(TEXT("Current")).Get());

	// save_game and data_asset.
	const TArray<TSharedPtr<FJsonObject>> SaveGames = InCategory(Data, TEXT("save_game"));
	if (TestEqual(TEXT("One save_game"), SaveGames.Num(), 1))
	{
		TestEqual(TEXT("save_game asset"), SaveGames[0]->GetStringField(TEXT("asset_path")), SaveGame->GetPathName());
		TestEqual(TEXT("save_game variable"), SaveGames[0]->GetStringField(TEXT("variable")), FString(TEXT("Saved")));
	}
	const TArray<TSharedPtr<FJsonObject>> DataAssets = InCategory(Data, TEXT("data_asset"));
	TestEqual(TEXT("Two data assets"), DataAssets.Num(), 2);
	const TSharedPtr<FJsonObject> NativeUse = OfAsset(DataAssets, NativeAsset->GetPathName());
	if (TestNotNull(TEXT("The native-class instance is reported"), NativeUse.Get()))
	{
		TestFalse(TEXT("... without via_class"), NativeUse->HasField(TEXT("via_class")));
	}
	const TSharedPtr<FJsonObject> BlueprintUse = OfAsset(DataAssets, BlueprintAsset->GetPathName());
	if (TestNotNull(TEXT("The Blueprint-class instance is reported"), BlueprintUse.Get()))
	{
		TestEqual(TEXT("... via its Blueprint class"), BlueprintUse->GetStringField(TEXT("via_class")), DataAssetClass->GeneratedClass->GetClassPathName().ToString());
	}

	// datatable_row_struct and nested_struct.
	const TArray<TSharedPtr<FJsonObject>> Tables = InCategory(Data, TEXT("datatable_row_struct"));
	if (TestEqual(TEXT("One table"), Tables.Num(), 1))
	{
		TestEqual(TEXT("table asset"), Tables[0]->GetStringField(TEXT("asset_path")), TablePath);
	}
	const TArray<TSharedPtr<FJsonObject>> Nested = InCategory(Data, TEXT("nested_struct"));
	if (TestEqual(TEXT("One nested_struct"), Nested.Num(), 1))
	{
		TestEqual(TEXT("nested asset"), Nested[0]->GetStringField(TEXT("asset_path")), Wrapper->GetPathName());
	}

	// Function parameter and local variable carry the graph, node and pin/variable.
	const TArray<TSharedPtr<FJsonObject>> Parameters2 = InCategory(Data, TEXT("function_parameter"));
	if (TestEqual(TEXT("One function_parameter"), Parameters2.Num(), 1))
	{
		TestEqual(TEXT("parameter pin"), Parameters2[0]->GetStringField(TEXT("pin")), FString(TEXT("InHit")));
		TestEqual(TEXT("parameter node"), Parameters2[0]->GetStringField(TEXT("node_guid")), Entry->NodeGuid.ToString());
		TestEqual(TEXT("parameter graph"), Parameters2[0]->GetStringField(TEXT("graph_guid")), Function->GraphGuid.ToString());
		TestEqual(TEXT("parameter graph_name"), Parameters2[0]->GetStringField(TEXT("graph_name")), FString(TEXT("Compute")));
	}
	const TArray<TSharedPtr<FJsonObject>> Locals = InCategory(Data, TEXT("local_variable"));
	if (TestEqual(TEXT("One local_variable"), Locals.Num(), 1))
	{
		TestEqual(TEXT("local variable"), Locals[0]->GetStringField(TEXT("variable")), FString(TEXT("LocalHit")));
		TestEqual(TEXT("local node"), Locals[0]->GetStringField(TEXT("node_guid")), Entry->NodeGuid.ToString());
	}

	// Struct nodes, one entry each, by node (no per-pin duplicates).
	const struct { const TCHAR* Category; UEdGraphNode* Node; const TCHAR* NodeClass; } StructNodes[] = {
		{TEXT("make_struct"), MakeNode, TEXT("UK2Node_MakeStruct")},
		{TEXT("break_struct"), BreakNode, TEXT("UK2Node_BreakStruct")},
		{TEXT("set_fields"), SetNode, TEXT("UK2Node_SetFieldsInStruct")},
	};
	for (const auto& Expected : StructNodes)
	{
		const TArray<TSharedPtr<FJsonObject>> Found = InCategory(Data, Expected.Category);
		if (TestEqual(*FString::Printf(TEXT("One %s"), Expected.Category), Found.Num(), 1))
		{
			TestEqual(*FString::Printf(TEXT("%s node_guid"), Expected.Category), Found[0]->GetStringField(TEXT("node_guid")), Expected.Node->NodeGuid.ToString());
			TestEqual(*FString::Printf(TEXT("%s node_class"), Expected.Category), Found[0]->GetStringField(TEXT("node_class")), FString(Expected.NodeClass));
			TestEqual(*FString::Printf(TEXT("%s graph_guid"), Expected.Category), Found[0]->GetStringField(TEXT("graph_guid")), Graph->GraphGuid.ToString());
		}
	}
	TestEqual(TEXT("The struct nodes add no pin entries"), InCategory(Data, TEXT("pin")).Num(), 0);
	TestEqual(TEXT("No enum category is reported for a struct"), InCategory(Data, TEXT("switch_enum")).Num() + InCategory(Data, TEXT("enum_literal")).Num(), 0);
	TestEqual(TEXT("usage_count"), static_cast<int32>(Data->GetNumberField(TEXT("usage_count"))), Objects(Data, TEXT("usages")).Num());
	TestEqual(TEXT("usage_count adds up: 5 variables, 1 save_game, 2 data assets, 1 table, 1 nested, 1 parameter, 1 local, 3 struct nodes"),
		Objects(Data, TEXT("usages")).Num(), 15);

	// Unreadable assets are reported, never counted as free of the type.
	TestTrue(TEXT("The unreadable Blueprint is under load_failures"),
		Strings(Data, TEXT("load_failures")).ContainsByPredicate([&UnreadablePath](const FString& Failure) { return Failure.Contains(UnreadablePath); }));

	// The enum: variable, switch_enum, enum_literal and the native asset's reference; the struct's data is not a use.
	const FCortexCommandResult EnumResult = Run(TEXT("find_type_usages"), RequestParams(Mode->GetPathName(), Fixture.Dir));
	if (TestTrue(TEXT("find_type_usages on the enum succeeds"), EnumResult.bSuccess))
	{
		TestEqual(TEXT("enum kind"), EnumResult.Data->GetStringField(TEXT("kind")), FString(TEXT("enum")));
		const TArray<TSharedPtr<FJsonObject>> EnumVariables = InCategory(EnumResult.Data, TEXT("variable"));
		if (TestEqual(TEXT("One enum variable"), EnumVariables.Num(), 1))
		{
			TestEqual(TEXT("enum variable"), EnumVariables[0]->GetStringField(TEXT("variable")), FString(TEXT("Current")));
		}
		const TSharedPtr<FJsonObject> SwitchUse = OfNode(InCategory(EnumResult.Data, TEXT("switch_enum")), SwitchNode->NodeGuid);
		if (TestNotNull(TEXT("switch_enum reported"), SwitchUse.Get()))
		{
			TestEqual(TEXT("switch_enum node_class"), SwitchUse->GetStringField(TEXT("node_class")), FString(TEXT("UK2Node_SwitchEnum")));
		}
		TestNotNull(TEXT("enum_literal reported"), OfNode(InCategory(EnumResult.Data, TEXT("enum_literal")), LiteralNode->NodeGuid).Get());
		TestNotNull(TEXT("The native asset references the enum"), OfAsset(InCategory(EnumResult.Data, TEXT("data_asset")), NativeAsset->GetPathName()).Get());
		TestEqual(TEXT("No struct category for an enum"), InCategory(EnumResult.Data, TEXT("make_struct")).Num() + InCategory(EnumResult.Data, TEXT("datatable_row_struct")).Num(), 0);
	}

	// An unused type has no entries (and nothing is missing from load_failures for it).
	const FCortexCommandResult UnusedResult = Run(TEXT("find_type_usages"), RequestParams(Unused->GetPathName(), Fixture.Dir));
	if (TestTrue(TEXT("find_type_usages on an unused enum succeeds"), UnusedResult.bSuccess))
	{
		TestEqual(TEXT("An unused type has zero usages"), static_cast<int32>(UnusedResult.Data->GetNumberField(TEXT("usage_count"))), 0);
		TestEqual(TEXT("... and an empty list"), Objects(UnusedResult.Data, TEXT("usages")).Num(), 0);
	}

	// out_path: the file holds everything, the response the counts and the first 10 entries.
	TSharedPtr<FJsonObject> ToFile = RequestParams(Hit->GetPathName(), Fixture.Dir);
	const FString ReportPath = Fixture.ReportPath(TEXT("TypeUsages"));
	ToFile->SetStringField(TEXT("out_path"), ReportPath);
	const FCortexCommandResult FileResult = Run(TEXT("find_type_usages"), ToFile);
	if (TestTrue(TEXT("out_path request succeeds"), FileResult.bSuccess))
	{
		TestEqual(TEXT("The summary keeps the first 10 usages"), Objects(FileResult.Data, TEXT("usages")).Num(), 10);
		TestEqual(TEXT("... and totals all 15"), static_cast<int32>(FileResult.Data->GetNumberField(TEXT("usages_total"))), 15);
		FString Text;
		TSharedPtr<FJsonObject> Written;
		TestTrue(TEXT("The report file is written"), FFileHelper::LoadFileToString(Text, *ReportPath));
		TestTrue(TEXT("... and parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Written) && Written.IsValid());
		if (Written.IsValid())
		{
			TestEqual(TEXT("The file holds every usage"), Objects(Written, TEXT("usages")).Num(), 15);
		}
	}

	// Refusals.
	TestEqual(TEXT("An unknown type is refused"), Run(TEXT("find_type_usages"), RequestParams(TEXT("/Game/Temp/NoSuchType"), Fixture.Dir)).ErrorCode, CortexErrorCodes::AssetNotFound);
	TSharedPtr<FJsonObject> NoPrefixes = MakeShared<FJsonObject>();
	NoPrefixes->SetStringField(TEXT("user_type"), Hit->GetPathName());
	TestEqual(TEXT("path_prefixes is required"), Run(TEXT("find_type_usages"), NoPrefixes).ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindTypeUsagesNativeTest,
	"Cortex.Reflect.FindTypeUsagesNative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectFindTypeUsagesNativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122TypeUsagesTest;
	FFixture Fixture;

	UBlueprint* User = Fixture.MakeBlueprint(TEXT("BP_NativeUser"), AActor::StaticClass());
	UBlueprint* Other = Fixture.MakeBlueprint(TEXT("BP_NativeOther"), AActor::StaticClass());
	if (!TestNotNull(TEXT("User"), User) || !TestNotNull(TEXT("Other"), Other))
	{
		return false;
	}
	TestTrue(TEXT("Struct variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Hit"), StructPin(FCortexIssue122NativeHit::StaticStruct())));
	TestTrue(TEXT("Enum variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Mode"), EnumPin(StaticEnum<ECortexIssue122NativeMode>())));
	TestTrue(TEXT("User saved"), Fixture.SaveAsset(User));
	TestTrue(TEXT("Other saved"), Fixture.SaveAsset(Other));
	Fixture.RegisterSavedFiles();

	// Run against a native struct and enum: the residual-check use after a swap.
	const FCortexCommandResult StructResult = Run(TEXT("find_type_usages"), RequestParams(TEXT("/Script/CortexReflect.CortexIssue122NativeHit"), Fixture.Dir));
	if (TestTrue(TEXT("find_type_usages on a native struct succeeds"), StructResult.bSuccess))
	{
		TestTrue(TEXT("native"), StructResult.Data->GetBoolField(TEXT("native")));
		const TArray<TSharedPtr<FJsonObject>> Variables = InCategory(StructResult.Data, TEXT("variable"));
		if (TestEqual(TEXT("One variable use"), Variables.Num(), 1))
		{
			TestEqual(TEXT("variable asset"), Variables[0]->GetStringField(TEXT("asset_path")), User->GetPathName());
			TestEqual(TEXT("variable name"), Variables[0]->GetStringField(TEXT("variable")), FString(TEXT("Hit")));
		}
		TestEqual(TEXT("Nothing else uses the struct"), static_cast<int32>(StructResult.Data->GetNumberField(TEXT("usage_count"))), 1);
	}
	const FCortexCommandResult EnumResult = Run(TEXT("find_type_usages"), RequestParams(TEXT("ECortexIssue122NativeMode"), Fixture.Dir));
	if (TestTrue(TEXT("find_type_usages on a native enum by name succeeds"), EnumResult.bSuccess))
	{
		const TArray<TSharedPtr<FJsonObject>> Variables = InCategory(EnumResult.Data, TEXT("variable"));
		if (TestEqual(TEXT("One enum variable use"), Variables.Num(), 1))
		{
			TestEqual(TEXT("enum variable name"), Variables[0]->GetStringField(TEXT("variable")), FString(TEXT("Mode")));
		}
	}
	return true;
}
