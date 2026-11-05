// reflect.find_name_lookups tests. C++ fixtures are written under Saved/CortexTest120 (never under a Source/ that
// the build compiles); Blueprint fixtures are saved assets under /Game/Temp/Cortex120/NameLookup/<guid>.

#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"
#include "Operations/CortexReflectNameLookupOps.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace CortexReflectNameLookupTestSupport
{
	TArray<TSharedPtr<FJsonObject>> ObjectArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsObject());
			}
		}
		return Out;
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsString());
			}
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> JsonStrings(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	/** The hit with this api and name, or null. */
	TSharedPtr<FJsonObject> FindHit(const TArray<TSharedPtr<FJsonObject>>& Hits, const FString& Api, const FString& Name)
	{
		for (const TSharedPtr<FJsonObject>& Hit : Hits)
		{
			if (Hit->GetStringField(TEXT("api")) == Api && Hit->GetStringField(TEXT("name")) == Name)
			{
				return Hit;
			}
		}
		return nullptr;
	}

	bool HasMatch(const TSharedPtr<FJsonObject>& Hit, const FString& AssetSuffix, const FString& Member, const FString& Kind)
	{
		for (const TSharedPtr<FJsonObject>& Match : ObjectArray(Hit, TEXT("matches")))
		{
			if (Match->GetStringField(TEXT("asset_path")).EndsWith(AssetSuffix) && Match->GetStringField(TEXT("member")) == Member
				&& Match->GetStringField(TEXT("member_kind")) == Kind)
			{
				return true;
			}
		}
		return false;
	}

	/** A scratch directory under Saved/CortexTest120, removed on scope exit, holding Source/<file>. */
	struct FTempSource
	{
		FString Root;
		FString File;
		FString Relative;

		FTempSource(const FString& FileName, const FString& Body)
		{
			const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexTest120"), Guid));
			const FString SourceDir = FPaths::Combine(Root, TEXT("Source"));
			File = FPaths::Combine(SourceDir, FileName);
			Relative = File;
			FPaths::MakePathRelativeTo(Relative, *FPaths::ProjectDir());
			IFileManager::Get().MakeDirectory(*SourceDir, true);
			FFileHelper::SaveStringToFile(Body, *File);
			SourceRoot = SourceDir;
		}

		~FTempSource()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString SourceRoot;
	};

	/** Saved Blueprint fixtures under /Game/Temp/Cortex120/NameLookup/<guid>. */
	struct FNameLookupFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Cortex120/NameLookup/%s"), *Suffix);
		TArray<FString> SavedFiles;

		bool SaveAsset(UObject* Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
			{
				return false;
			}
			SavedFiles.Add(Filename);
			return true;
		}

		/** BP with function DoThing, variable Health, custom event Other and two K2_SetTimer nodes (Other, empty). */
		UBlueprint* MakeTarget(const FString& Name)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None);
			if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
			{
				return nullptr;
			}
			FEdGraphPinType FloatType;
			FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
			FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Health"), FloatType);
			UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, TEXT("DoThing"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);

			UEdGraph* Event = Blueprint->UbergraphPages[0];
			{
				FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Event);
				UK2Node_CustomEvent* Node = Creator.CreateNode();
				Node->CustomFunctionName = TEXT("Other");
				Creator.Finalize();
			}
			AddTimer(Event, TEXT("Other"));
			AddTimer(Event, FString());
			return Blueprint;
		}

		static void AddTimer(UEdGraph* Graph, const FString& FunctionName)
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(
				GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimer), UKismetSystemLibrary::StaticClass());
			Node->AllocateDefaultPins();
			if (UEdGraphPin* Pin = Node->FindPin(TEXT("FunctionName"), EGPD_Input))
			{
				Pin->DefaultValue = FunctionName;
			}
			Creator.Finalize();
		}

		void RegisterSavedFiles() const
		{
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
		}

		void Cleanup()
		{
			IAssetRegistry::GetChecked().WaitForCompletion();
			TArray<UObject*> Assets;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Asset = *It;
				if (Asset && Asset->IsAsset() && Asset->GetOutermost()->GetName().StartsWith(Dir))
				{
					Assets.Add(Asset);
				}
			}
			for (UObject* Asset : Assets)
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags);
			IFileManager::Get().DeleteDirectory(
				*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), TEXT("Cortex120"), TEXT("NameLookup"), Suffix), false, true);
		}
	};

	const TCHAR* CppSample =
		TEXT("// FindFunction(TEXT(\"Commented\"))\n")                                           // 1
		TEXT("void A(UObject* O)\n")                                                             // 2
		TEXT("{\n")                                                                              // 3
		TEXT("\tO->FindFunction(TEXT(\"DoThing\"));\n")                                          // 4
		TEXT("\tFTimerDelegate D = FTimerDelegate::CreateUFunction(this, FName(\"Other\"));\n")  // 5
		TEXT("\tO->GetClass()->FindPropertyByName(TEXT(\"Health\"));\n")                         // 6
		TEXT("\t/* block\n")                                                                     // 7
		TEXT("\tFindFunctionByName(TEXT(\"InBlock\")) */\n")                                     // 8
		TEXT("\tO->FindFunction(GET_FUNCTION_NAME_STRING_CHECKED(AFoo, Bar));\n")                // 9
		TEXT("\tO->FindFunctionByName(FName(TEXT(\"ByNameCall\")));\n")                          // 10
		TEXT("\tFindFProperty<FProperty>(Class, \"Prop\");\n")                                   // 11
		TEXT("\tconst TCHAR* S = \"FindFunction(TEXT(\\\"InString\\\"))\";\n")                    // 12
		TEXT("\tDelegate.BindUFunction(this, TEXT(\"Bound\"));\n")                               // 13
		TEXT("\tK2_SetTimer(this, TEXT(\"Tm\"), 1.f, false);\n")                                 // 14
		TEXT("\tO->FindFunction(Variable);\n")                                                   // 15
		TEXT("}\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectNameLookupCppTest,
	"Cortex.Reflect.NameLookup.Cpp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupCppTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	FTempSource Source(TEXT("Sample.cpp"), CppSample);
	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots(
		{Source.SourceRoot}, {FString::Printf(TEXT("/Game/Temp/Cortex120/NameLookup/None_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))}, {});
	if (!TestNotNull(TEXT("result"), Result.Get()))
	{
		return false;
	}

	TestEqual(TEXT("scanned_files"), static_cast<int32>(Result->GetNumberField(TEXT("scanned_files"))), 1);
	const TArray<TSharedPtr<FJsonObject>> Hits = ObjectArray(Result, TEXT("cpp"));
	TestEqual(TEXT("cpp hit count (comments, strings, checked names and non-literals skipped)"), Hits.Num(), 7);

	struct FExpected
	{
		const TCHAR* Api;
		const TCHAR* Name;
		int32 Line;
	};
	const FExpected Expected[] = {
		{TEXT("FindFunction"), TEXT("DoThing"), 4},
		{TEXT("CreateUFunction"), TEXT("Other"), 5},
		{TEXT("FindPropertyByName"), TEXT("Health"), 6},
		{TEXT("FindFunctionByName"), TEXT("ByNameCall"), 10},
		{TEXT("FindFProperty"), TEXT("Prop"), 11},
		{TEXT("BindUFunction"), TEXT("Bound"), 13},
		{TEXT("K2_SetTimer"), TEXT("Tm"), 14},
	};
	for (const FExpected& Want : Expected)
	{
		const TSharedPtr<FJsonObject> Hit = FindHit(Hits, Want.Api, Want.Name);
		if (TestNotNull(*FString::Printf(TEXT("hit %s(%s)"), Want.Api, Want.Name), Hit.Get()))
		{
			TestEqual(*FString::Printf(TEXT("%s line"), Want.Name), static_cast<int32>(Hit->GetNumberField(TEXT("line"))), Want.Line);
			TestEqual(*FString::Printf(TEXT("%s file"), Want.Name), Hit->GetStringField(TEXT("file")), Source.Relative);
			TestEqual(*FString::Printf(TEXT("%s source"), Want.Name), Hit->GetStringField(TEXT("source")), FString(TEXT("cpp")));
		}
	}
	for (const TCHAR* Skipped : {TEXT("Commented"), TEXT("InBlock"), TEXT("InString"), TEXT("Bar"), TEXT("Variable")})
	{
		for (const TSharedPtr<FJsonObject>& Hit : Hits)
		{
			TestNotEqual(*FString::Printf(TEXT("%s is not a by-name risk"), Skipped), Hit->GetStringField(TEXT("name")), FString(Skipped));
		}
	}
	TestEqual(TEXT("nothing matched without Blueprints"), StringArray(Result, TEXT("matched_names")).Num(), 0);
	TestEqual(TEXT("every looked-up name unmatched"), StringArray(Result, TEXT("unmatched_names")).Num(), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectNameLookupBlueprintTest,
	"Cortex.Reflect.NameLookup.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupBlueprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	FNameLookupFixture Fixture;
	UBlueprint* Target = Fixture.MakeTarget(TEXT("BP_Target"));
	if (!TestNotNull(TEXT("target"), Target) || !TestTrue(TEXT("save"), Fixture.SaveAsset(Target)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	FTempSource Source(TEXT("Sample.cpp"),
		TEXT("void A(UObject* O)\n{\n")
		TEXT("\tO->FindFunction(TEXT(\"DoThing\"));\n")
		TEXT("\tO->GetClass()->FindPropertyByName(TEXT(\"Health\"));\n")
		TEXT("\tFTimerDelegate::CreateUFunction(this, FName(\"Other\"));\n")
		TEXT("\tO->FindFunction(TEXT(\"Missing\"));\n}\n"));

	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots({Source.SourceRoot}, {Fixture.Dir}, {});
	if (TestNotNull(TEXT("result"), Result.Get()))
	{
		const FString Asset = TEXT("/BP_Target.BP_Target");
		const TArray<TSharedPtr<FJsonObject>> Cpp = ObjectArray(Result, TEXT("cpp"));
		TestEqual(TEXT("cpp hits"), Cpp.Num(), 4);
		const TSharedPtr<FJsonObject> DoThing = FindHit(Cpp, TEXT("FindFunction"), TEXT("DoThing"));
		const TSharedPtr<FJsonObject> Health = FindHit(Cpp, TEXT("FindPropertyByName"), TEXT("Health"));
		const TSharedPtr<FJsonObject> Other = FindHit(Cpp, TEXT("CreateUFunction"), TEXT("Other"));
		const TSharedPtr<FJsonObject> Missing = FindHit(Cpp, TEXT("FindFunction"), TEXT("Missing"));
		if (TestNotNull(TEXT("DoThing hit"), DoThing.Get()))
		{
			TestTrue(TEXT("DoThing matches BP function"), HasMatch(DoThing, Asset, TEXT("DoThing"), TEXT("function")));
		}
		if (TestNotNull(TEXT("Health hit"), Health.Get()))
		{
			TestTrue(TEXT("Health matches BP variable"), HasMatch(Health, Asset, TEXT("Health"), TEXT("variable")));
		}
		if (TestNotNull(TEXT("Other hit"), Other.Get()))
		{
			TestTrue(TEXT("Other matches BP custom event"), HasMatch(Other, Asset, TEXT("Other"), TEXT("custom_event")));
		}
		if (TestNotNull(TEXT("Missing hit"), Missing.Get()))
		{
			TestEqual(TEXT("Missing has no matches"), ObjectArray(Missing, TEXT("matches")).Num(), 0);
		}

		const TArray<TSharedPtr<FJsonObject>> Blueprint = ObjectArray(Result, TEXT("blueprint"));
		TestEqual(TEXT("one literal K2_SetTimer node (the empty one is not a by-name lookup)"), Blueprint.Num(), 1);
		if (Blueprint.Num() == 1)
		{
			TestEqual(TEXT("blueprint source"), Blueprint[0]->GetStringField(TEXT("source")), FString(TEXT("blueprint")));
			TestEqual(TEXT("blueprint api"), Blueprint[0]->GetStringField(TEXT("api")), FString(TEXT("K2_SetTimer")));
			TestEqual(TEXT("blueprint name"), Blueprint[0]->GetStringField(TEXT("name")), FString(TEXT("Other")));
			TestTrue(TEXT("blueprint asset_path"), Blueprint[0]->GetStringField(TEXT("asset_path")).EndsWith(Asset));
			TestTrue(TEXT("blueprint node_id"), !Blueprint[0]->GetStringField(TEXT("node_id")).IsEmpty());
			TestTrue(TEXT("blueprint hit matches the custom event"), HasMatch(Blueprint[0], Asset, TEXT("Other"), TEXT("custom_event")));
		}

		const TArray<FString> Matched = StringArray(Result, TEXT("matched_names"));
		const TArray<FString> Unmatched = StringArray(Result, TEXT("unmatched_names"));
		TestTrue(TEXT("DoThing matched"), Matched.Contains(TEXT("DoThing")));
		TestTrue(TEXT("Health matched"), Matched.Contains(TEXT("Health")));
		TestTrue(TEXT("Other matched"), Matched.Contains(TEXT("Other")));
		TestEqual(TEXT("matched count"), Matched.Num(), 3);
		TestTrue(TEXT("Missing unmatched"), Unmatched.Contains(TEXT("Missing")));
		TestEqual(TEXT("unmatched count"), Unmatched.Num(), 1);

		// symbols restricts which names are reported at all.
		const TSharedPtr<FJsonObject> Filtered = FCortexReflectNameLookupOps::FindNameLookupsInRoots(
			{Source.SourceRoot}, {Fixture.Dir}, TSet<FString>{TEXT("health")});
		if (TestNotNull(TEXT("filtered result"), Filtered.Get()))
		{
			const TArray<TSharedPtr<FJsonObject>> FilteredCpp = ObjectArray(Filtered, TEXT("cpp"));
			TestEqual(TEXT("filtered cpp"), FilteredCpp.Num(), 1);
			TestEqual(TEXT("filtered blueprint"), ObjectArray(Filtered, TEXT("blueprint")).Num(), 0);
			TestEqual(TEXT("filtered matched"), StringArray(Filtered, TEXT("matched_names")), TArray<FString>{TEXT("Health")});
			TestEqual(TEXT("filtered unmatched"), StringArray(Filtered, TEXT("unmatched_names")).Num(), 0);
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectNameLookupCommandTest,
	"Cortex.Reflect.NameLookup.Command",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupCommandTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	// Fixed roots: the game source dir and project plugins, never the UnrealCortex plugin's own Source.
	const TArray<FString> Roots = FCortexReflectNameLookupOps::GetFixedSourceRoots();
	const FString GameSource = FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir());
	bool bHasGameSource = false;
	const TSharedPtr<IPlugin> Own = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
	const FString OwnBase = Own.IsValid() ? FPaths::ConvertRelativePathToFull(Own->GetBaseDir()) : FString();
	TestTrue(TEXT("UnrealCortex plugin is enabled in the sandbox"), Own.IsValid());
	for (const FString& Root : Roots)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(Root);
		bHasGameSource |= FPaths::IsSamePath(Full, GameSource);
		TestFalse(*FString::Printf(TEXT("root %s excludes the plugin's own source"), *Root),
			!OwnBase.IsEmpty() && FPaths::IsUnderDirectory(Full, OwnBase));
		TestTrue(*FString::Printf(TEXT("root %s lies under the project"), *Root), FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
	}
	TestTrue(TEXT("game source dir is a root"), bHasGameSource);

	FCortexReflectCommandHandler Handler;
	{
		FCortexCommandResult Missing = Handler.Execute(TEXT("find_name_lookups"), MakeShared<FJsonObject>());
		TestFalse(TEXT("missing path_prefixes is refused"), Missing.bSuccess);
		TestEqual(TEXT("missing path_prefixes code"), Missing.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/NameLookup/None")}));
		Params->SetArrayField(TEXT("symbols"), JsonStrings({TEXT("")}));
		FCortexCommandResult BadSymbols = Handler.Execute(TEXT("find_name_lookups"), Params);
		TestFalse(TEXT("empty symbol is refused"), BadSymbols.bSuccess);
		TestEqual(TEXT("empty symbol code"), BadSymbols.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/NameLookup/None")}));
		FCortexCommandResult Ok = Handler.Execute(TEXT("find_name_lookups"), Params);
		TestTrue(TEXT("valid request succeeds"), Ok.bSuccess);
		if (Ok.bSuccess && TestTrue(TEXT("data"), Ok.Data.IsValid()))
		{
			TestTrue(TEXT("scanned_files present"), Ok.Data->HasTypedField<EJson::Number>(TEXT("scanned_files")));
			TestTrue(TEXT("cpp present"), Ok.Data->HasTypedField<EJson::Array>(TEXT("cpp")));
			TestTrue(TEXT("blueprint present"), Ok.Data->HasTypedField<EJson::Array>(TEXT("blueprint")));
		}
	}
	return true;
}
