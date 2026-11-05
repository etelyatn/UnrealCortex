// blueprint.export_text_inventory bounds inline responses at the shared 40000-character ceiling
// and writes the full inventory through the safe file contract when `out_path` is given.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPTextInventoryTestTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 InlineCeilingChars = 40000;

	UBlueprint* MakeRowsBlueprint(const FString& Suffix, const int32 RowCount)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CortexTextInventoryFile_%s"), *Suffix));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*FPackageName::GetLongPackageAssetName(Package->GetName())),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
		if (!Blueprint)
		{
			return nullptr;
		}
		FEdGraphPinType RowArrayType;
		RowArrayType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		RowArrayType.PinSubCategoryObject = FCortexBPTextInventoryTestRow::StaticStruct();
		RowArrayType.ContainerType = EPinContainerType::Array;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Rows"), RowArrayType))
		{
			return nullptr;
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UClass* GeneratedClass = Blueprint->GeneratedClass;
		FArrayProperty* RowsProperty = GeneratedClass ? CastField<FArrayProperty>(GeneratedClass->FindPropertyByName(TEXT("Rows"))) : nullptr;
		if (!RowsProperty)
		{
			return nullptr;
		}
		FScriptArrayHelper Rows(RowsProperty, RowsProperty->ContainerPtrToValuePtr<void>(GeneratedClass->GetDefaultObject()));
		Rows.AddValues(RowCount);
		for (int32 Index = 0; Index < RowCount; ++Index)
		{
			reinterpret_cast<FCortexBPTextInventoryTestRow*>(Rows.GetRawPtr(Index))->Label =
				FText::AsCultureInvariant(FString::Printf(TEXT("Row value number %d"), Index));
		}
		return Blueprint;
	}

	void Retire(UObject* Asset)
	{
		if (Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
	}

	FString ErrorOf(const FCortexCommandResult& Result)
	{
		FString Error;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("_error"), Error);
		}
		return Error;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPExportTextInventoryFileOutputTest,
	"Cortex.Blueprint.ExportTextInventoryFileOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPExportTextInventoryFileOutputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;

	// Small fixture: out_path inside Saved writes a file equal to the inline inventory.
	TStrongObjectPtr<UBlueprint> Small(MakeRowsBlueprint(Suffix + TEXT("S"), 3));
	if (!TestNotNull(TEXT("small fixture"), Small.Get()))
	{
		return false;
	}
	const FString SmallPath = Small->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> InlineParams = MakeShared<FJsonObject>();
	InlineParams->SetStringField(TEXT("asset_path"), SmallPath);
	const FCortexCommandResult Inline = Handler.Execute(TEXT("export_text_inventory"), InlineParams);
	TestTrue(TEXT("small inline export succeeds"), Inline.bSuccess && Inline.Data.IsValid());

	const FString OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexTextInventoryTest"),
		FString::Printf(TEXT("inventory_%s.json"), *Suffix)));
	TSharedPtr<FJsonObject> FileParams = MakeShared<FJsonObject>();
	FileParams->SetStringField(TEXT("asset_path"), SmallPath);
	FileParams->SetStringField(TEXT("out_path"), OutPath);
	const FCortexCommandResult ToFile = Handler.Execute(TEXT("export_text_inventory"), FileParams);
	if (TestTrue(*FString::Printf(TEXT("small file export succeeds (%s: %s)"), *ToFile.ErrorCode, *ToFile.ErrorMessage), ToFile.bSuccess && ToFile.Data.IsValid()))
	{
		TestFalse(TEXT("file response carries no entries"), ToFile.Data->HasField(TEXT("entries")));
		TestTrue(TEXT("file response carries counts"), ToFile.Data->HasTypedField<EJson::Object>(TEXT("counts")));
		TestEqual(TEXT("file response asset_path"), ToFile.Data->GetStringField(TEXT("asset_path")), SmallPath);
		TestTrue(TEXT("file response out_path names the file"),
			ToFile.Data->GetStringField(TEXT("out_path")).EndsWith(FString::Printf(TEXT("inventory_%s.json"), *Suffix)));
	}
	FString FileText;
	if (TestTrue(TEXT("file exists and is readable"), FFileHelper::LoadFileToString(FileText, *OutPath)) && Inline.Data.IsValid())
	{
		TSharedPtr<FJsonObject> FileJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileText);
		if (TestTrue(TEXT("file parses"), FJsonSerializer::Deserialize(Reader, FileJson) && FileJson.IsValid()))
		{
			const TArray<TSharedPtr<FJsonValue>>& FileEntries = FileJson->GetArrayField(TEXT("entries"));
			const TArray<TSharedPtr<FJsonValue>>& InlineEntries = Inline.Data->GetArrayField(TEXT("entries"));
			if (TestEqual(TEXT("file entry count equals inline"), FileEntries.Num(), InlineEntries.Num()))
			{
				for (int32 Index = 0; Index < InlineEntries.Num(); ++Index)
				{
					TestTrue(*FString::Printf(TEXT("entry %d equals inline"), Index),
						FJsonValue::CompareEqual(*FileEntries[Index], *InlineEntries[Index]));
				}
			}
			TestEqual(TEXT("file asset_path"), FileJson->GetStringField(TEXT("asset_path")), SmallPath);
		}
	}
	IFileManager::Get().Delete(*OutPath);

	// Outside allowed roots is refused and writes nothing.
	const FString OutsidePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"),
		FString::Printf(TEXT("CortexTextInventoryOutside_%s.json"), *Suffix)));
	TSharedPtr<FJsonObject> OutsideParams = MakeShared<FJsonObject>();
	OutsideParams->SetStringField(TEXT("asset_path"), SmallPath);
	OutsideParams->SetStringField(TEXT("out_path"), OutsidePath);
	const FCortexCommandResult Outside = Handler.Execute(TEXT("export_text_inventory"), OutsideParams);
	TestFalse(TEXT("out_path outside allowed roots is refused"), Outside.bSuccess);
	TestEqual(TEXT("outside refusal code"), Outside.ErrorCode, CortexErrorCodes::InvalidField);
	TestFalse(TEXT("nothing written outside allowed roots"), IFileManager::Get().FileExists(*OutsidePath));
	TestTrue(TEXT("outside refusal names the allowed roots"), Outside.ErrorMessage.Contains(TEXT("Project Saved")));

	// Oversize inventory: inline refuses with RESPONSE_TOO_LARGE, out_path succeeds.
	TStrongObjectPtr<UBlueprint> Large(MakeRowsBlueprint(Suffix + TEXT("L"), 600));
	if (!TestNotNull(TEXT("large fixture"), Large.Get()))
	{
		Retire(Small.Get());
		return false;
	}
	const FString LargePath = Large->GetOutermost()->GetName();
	TSharedPtr<FJsonObject> LargeInline = MakeShared<FJsonObject>();
	LargeInline->SetStringField(TEXT("asset_path"), LargePath);
	const FCortexCommandResult LargeInlineResult = Handler.Execute(TEXT("export_text_inventory"), LargeInline);
	TestTrue(TEXT("oversize inline returns an envelope"), LargeInlineResult.bSuccess && LargeInlineResult.Data.IsValid());
	TestEqual(TEXT("oversize inline reports RESPONSE_TOO_LARGE"), ErrorOf(LargeInlineResult), FString(TEXT("RESPONSE_TOO_LARGE")));
	if (LargeInlineResult.Data.IsValid())
	{
		TestTrue(TEXT("oversize refusal carries counts"), LargeInlineResult.Data->HasTypedField<EJson::Object>(TEXT("counts")));
		TestFalse(TEXT("oversize refusal carries no entries"), LargeInlineResult.Data->HasField(TEXT("entries")));
		double Ceiling = 0.0;
		LargeInlineResult.Data->TryGetNumberField(TEXT("max_response_chars"), Ceiling);
		TestEqual(TEXT("oversize refusal reports the shared ceiling"), static_cast<int32>(Ceiling), InlineCeilingChars);
	}

	const FString LargeOut = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexTextInventoryTest"),
		FString::Printf(TEXT("large_%s.json"), *Suffix)));
	TSharedPtr<FJsonObject> LargeFile = MakeShared<FJsonObject>();
	LargeFile->SetStringField(TEXT("asset_path"), LargePath);
	LargeFile->SetStringField(TEXT("out_path"), LargeOut);
	const FCortexCommandResult LargeFileResult = Handler.Execute(TEXT("export_text_inventory"), LargeFile);
	TestTrue(TEXT("oversize inventory succeeds with out_path"), LargeFileResult.bSuccess && LargeFileResult.Data.IsValid());
	TestEqual(TEXT("oversize file response has no error"), ErrorOf(LargeFileResult), FString());
	FString LargeText;
	if (TestTrue(TEXT("large file written"), FFileHelper::LoadFileToString(LargeText, *LargeOut)))
	{
		TestTrue(TEXT("large file exceeds the inline ceiling"), LargeText.Len() > InlineCeilingChars);
		TSharedPtr<FJsonObject> LargeJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LargeText);
		if (TestTrue(TEXT("large file parses"), FJsonSerializer::Deserialize(Reader, LargeJson) && LargeJson.IsValid()))
		{
			TestEqual(TEXT("large file holds every entry"), LargeJson->GetArrayField(TEXT("entries")).Num(), 600);
		}
	}
	IFileManager::Get().Delete(*LargeOut);

	Retire(Large.Get());
	Retire(Small.Get());
	return true;
}
