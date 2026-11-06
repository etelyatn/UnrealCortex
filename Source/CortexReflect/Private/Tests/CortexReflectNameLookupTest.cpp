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
#include "Components/SceneComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
			AddTimer(Event, TEXT("DoThing"), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_ClearTimer));
			if (Blueprint->SimpleConstructionScript)
			{
				USCS_Node* Component = Blueprint->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("BodyComp"));
				Blueprint->SimpleConstructionScript->AddNode(Component);
			}
			return Blueprint;
		}

		static void AddTimer(UEdGraph* Graph, const FString& FunctionName, const FName TimerFunction = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimer))
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(
				TimerFunction, UKismetSystemLibrary::StaticClass());
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
	FCortexReflectNameLookupCoverageTest,
	"Cortex.Reflect.NameLookup.Coverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupCoverageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	// Line accuracy after an apostrophe in a directive and a multi-line raw string; more APIs; multi-line calls.
	FTempSource Source(TEXT("Coverage.inl"),
		TEXT("#error don't do this\n")                                                            // 1
		TEXT("void A(UObject* O)\n")                                                              // 2
		TEXT("{\n")                                                                               // 3
		TEXT("\tO->FindFunction(TEXT(\"AfterApostrophe\"));\n")                                   // 4
		TEXT("\tDelegate.AddUFunction(this, FName(TEXT(\"Added\"), FNAME_Find));\n")              // 5
		TEXT("\tO->CallFunctionByNameWithArguments(TEXT(\"RunCmd arg1 arg2\"), Ar, nullptr);\n")  // 6
		TEXT("\tO->ProcessEvent(O->FindFunction(TEXT(\"Nested\")), nullptr);\n")                  // 7
		TEXT("\tK2_SetTimer(\n")                                                                  // 8
		TEXT("\t\tGetOuter(Foo(1, 2)),\n")                                                        // 9
		TEXT("\t\tTEXT(\"Multi,(Line)\"),\n")                                                     // 10
		TEXT("\t\t1.f, false);\n")                                                                // 11
		TEXT("\tconst char* Raw = R\"x(\n")                                                       // 12
		TEXT("FindFunction(TEXT(\"InRaw\"))\n")                                                   // 13
		TEXT(")x\";\n")                                                                           // 14
		TEXT("\tO->RemoveUFunction(this, \"AfterRaw\");\n")                                       // 15
		TEXT("\tFindFieldChecked<FProperty>(Scope, TEXT(\"Field\"));\n")                          // 16
		TEXT("\tK2_ClearTimer(this, TEXT(\"Clr\"));\n")                                           // 17
		TEXT("}\n"));
	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots(
		{Source.SourceRoot}, {FString::Printf(TEXT("/Game/Temp/Cortex120/NameLookup/None_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))}, {});
	if (!TestNotNull(TEXT("result"), Result.Get()))
	{
		return false;
	}
	TestEqual(TEXT("an .inl file is scanned"), static_cast<int32>(Result->GetNumberField(TEXT("scanned_files"))), 1);
	const TArray<TSharedPtr<FJsonObject>> Hits = ObjectArray(Result, TEXT("cpp"));
	TestEqual(TEXT("hit count (raw string body is not code)"), Hits.Num(), 8);

	struct FExpected
	{
		const TCHAR* Api;
		const TCHAR* Name;
		int32 Line;
	};
	const FExpected Expected[] = {
		{TEXT("FindFunction"), TEXT("AfterApostrophe"), 4},
		{TEXT("AddUFunction"), TEXT("Added"), 5},
		{TEXT("CallFunctionByNameWithArguments"), TEXT("RunCmd"), 6},
		{TEXT("FindFunction"), TEXT("Nested"), 7},
		{TEXT("K2_SetTimer"), TEXT("Multi,(Line)"), 8},
		{TEXT("RemoveUFunction"), TEXT("AfterRaw"), 15},
		{TEXT("FindFieldChecked"), TEXT("Field"), 16},
		{TEXT("K2_ClearTimer"), TEXT("Clr"), 17},
	};
	for (const FExpected& Want : Expected)
	{
		const TSharedPtr<FJsonObject> Hit = FindHit(Hits, Want.Api, Want.Name);
		if (TestNotNull(*FString::Printf(TEXT("hit %s(%s)"), Want.Api, Want.Name), Hit.Get()))
		{
			TestEqual(*FString::Printf(TEXT("%s line"), Want.Name), static_cast<int32>(Hit->GetNumberField(TEXT("line"))), Want.Line);
		}
	}
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
		TEXT("\tO->GetClass()->FindPropertyByName(TEXT(\"BodyComp\"));\n")
		TEXT("\tFTimerDelegate::CreateUFunction(this, FName(\"Other\"));\n")
		TEXT("\tO->FindFunction(TEXT(\"Missing\"));\n}\n"));

	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots({Source.SourceRoot}, {Fixture.Dir}, {});
	if (TestNotNull(TEXT("result"), Result.Get()))
	{
		const FString Asset = TEXT("/BP_Target.BP_Target");
		const TArray<TSharedPtr<FJsonObject>> Cpp = ObjectArray(Result, TEXT("cpp"));
		TestEqual(TEXT("cpp hits"), Cpp.Num(), 5);
		const TSharedPtr<FJsonObject> DoThing = FindHit(Cpp, TEXT("FindFunction"), TEXT("DoThing"));
		const TSharedPtr<FJsonObject> Health = FindHit(Cpp, TEXT("FindPropertyByName"), TEXT("Health"));
		const TSharedPtr<FJsonObject> Other = FindHit(Cpp, TEXT("CreateUFunction"), TEXT("Other"));
		const TSharedPtr<FJsonObject> Missing = FindHit(Cpp, TEXT("FindFunction"), TEXT("Missing"));
		const TSharedPtr<FJsonObject> BodyComp = FindHit(Cpp, TEXT("FindPropertyByName"), TEXT("BodyComp"));
		if (TestNotNull(TEXT("BodyComp hit"), BodyComp.Get()))
		{
			TestTrue(TEXT("BodyComp matches SCS component variable"), HasMatch(BodyComp, Asset, TEXT("BodyComp"), TEXT("component")));
		}
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
		TestEqual(TEXT("two literal timer nodes (the empty one is not a by-name lookup)"), Blueprint.Num(), 2);
		const TSharedPtr<FJsonObject> SetTimer = FindHit(Blueprint, TEXT("K2_SetTimer"), TEXT("Other"));
		const TSharedPtr<FJsonObject> ClearTimer = FindHit(Blueprint, TEXT("K2_ClearTimer"), TEXT("DoThing"));
		if (TestNotNull(TEXT("K2_SetTimer hit"), SetTimer.Get()))
		{
			TestEqual(TEXT("blueprint source"), SetTimer->GetStringField(TEXT("source")), FString(TEXT("blueprint")));
			TestTrue(TEXT("blueprint asset_path"), SetTimer->GetStringField(TEXT("asset_path")).EndsWith(Asset));
			TestTrue(TEXT("blueprint node_id"), !SetTimer->GetStringField(TEXT("node_id")).IsEmpty());
			TestTrue(TEXT("blueprint hit matches the custom event"), HasMatch(SetTimer, Asset, TEXT("Other"), TEXT("custom_event")));
		}
		if (TestNotNull(TEXT("K2_ClearTimer hit"), ClearTimer.Get()))
		{
			TestTrue(TEXT("clear timer hit matches the function"), HasMatch(ClearTimer, Asset, TEXT("DoThing"), TEXT("function")));
		}

		const TArray<FString> Matched = StringArray(Result, TEXT("matched_names"));
		const TArray<FString> Unmatched = StringArray(Result, TEXT("unmatched_names"));
		TestTrue(TEXT("DoThing matched"), Matched.Contains(TEXT("DoThing")));
		TestTrue(TEXT("Health matched"), Matched.Contains(TEXT("Health")));
		TestTrue(TEXT("Other matched"), Matched.Contains(TEXT("Other")));
		TestEqual(TEXT("matched count"), Matched.Num(), 4);
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

		// matched_assets counts the Blueprints per prefix and a prefix that matched nothing earns a warning.
		const FString EmptyPrefix = TEXT("/Game/Temp/Cortex120/NameLookup/NoSuchFolder");
		const TSharedPtr<FJsonObject> WithMissing = FCortexReflectNameLookupOps::FindNameLookupsInRoots(
			{Source.SourceRoot}, {Fixture.Dir, EmptyPrefix}, {});
		const TSharedPtr<FJsonObject>* MatchedAssets = nullptr;
		if (TestNotNull(TEXT("result with an empty prefix"), WithMissing.Get())
			&& TestTrue(TEXT("matched_assets present"), WithMissing->TryGetObjectField(TEXT("matched_assets"), MatchedAssets)))
		{
			TestEqual(TEXT("fixture folder matched its Blueprint"), static_cast<int32>((*MatchedAssets)->GetNumberField(Fixture.Dir)), 1);
			TestEqual(TEXT("missing folder matched nothing"), static_cast<int32>((*MatchedAssets)->GetNumberField(EmptyPrefix)), 0);
			TestEqual(TEXT("one warning names the empty prefix"), StringArray(WithMissing, TEXT("warnings")).Num(), 1);
		}
		TestEqual(TEXT("scanned_files counts the readable source"), static_cast<int32>(Result->GetNumberField(TEXT("scanned_files"))), 1);
		TestEqual(TEXT("no unreadable files"), StringArray(Result, TEXT("unreadable_files")).Num(), 0);
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectNameLookupWidgetMembersTest,
	"Cortex.Reflect.NameLookup.WidgetMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupWidgetMembersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	// A Widget Blueprint's variable widgets are generated properties (UBlueprint::GeneratedVariables), not NewVariables.
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	UClass* WidgetGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetBlueprintGeneratedClass"));
	if (!TestNotNull(TEXT("widget blueprint classes"), WidgetBlueprintClass) || !TestNotNull(TEXT("widget generated class"), WidgetGeneratedClass))
	{
		return false;
	}

	FNameLookupFixture Fixture;
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/WBP_Hud"), *Fixture.Dir));
	UBlueprint* Widget = FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(), Package, TEXT("WBP_Hud"), BPTYPE_Normal, WidgetBlueprintClass, WidgetGeneratedClass, NAME_None);
	const FObjectProperty* TreeProperty = Widget ? CastField<FObjectProperty>(Widget->GetClass()->FindPropertyByName(TEXT("WidgetTree"))) : nullptr;
	if (!TestNotNull(TEXT("widget blueprint"), Widget) || !TestNotNull(TEXT("WidgetTree property"), TreeProperty))
	{
		Fixture.Cleanup();
		return false;
	}
	UWidgetTree* Tree = Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Widget));
	if (!Tree)
	{
		Tree = NewObject<UWidgetTree>(Widget, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);
		TreeProperty->SetObjectPropertyValue_InContainer(Widget, Tree);
	}
	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Tree->RootWidget = Root;
	UTextBlock* HealthText = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HealthText"));
	HealthText->bIsVariable = true;
	Root->AddChild(HealthText);
	UTextBlock* Decoration = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DecorationText"));
	Root->AddChild(Decoration);
	FKismetEditorUtilities::CompileBlueprint(Widget);

	const bool bGenerated = Widget->GeneratedVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("HealthText"); });
	if (!TestTrue(TEXT("the engine compiler generated the HealthText variable"), bGenerated)
		|| !TestTrue(TEXT("save"), Fixture.SaveAsset(Widget)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	FTempSource Source(TEXT("Sample.cpp"),
		TEXT("void A(UObject* O)\n{\n")
		TEXT("\tO->GetClass()->FindPropertyByName(TEXT(\"HealthText\"));\n")
		TEXT("\tO->GetClass()->FindPropertyByName(TEXT(\"DecorationText\"));\n}\n"));

	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots({Source.SourceRoot}, {Fixture.Dir}, {});
	if (TestNotNull(TEXT("result"), Result.Get()))
	{
		const TArray<TSharedPtr<FJsonObject>> Cpp = ObjectArray(Result, TEXT("cpp"));
		const TSharedPtr<FJsonObject> Health = FindHit(Cpp, TEXT("FindPropertyByName"), TEXT("HealthText"));
		const TSharedPtr<FJsonObject> Plain = FindHit(Cpp, TEXT("FindPropertyByName"), TEXT("DecorationText"));
		if (TestNotNull(TEXT("HealthText hit"), Health.Get()))
		{
			TestTrue(TEXT("HealthText matches the widget variable"),
				HasMatch(Health, TEXT("/WBP_Hud.WBP_Hud"), TEXT("HealthText"), TEXT("widget_variable")));
		}
		if (TestNotNull(TEXT("DecorationText hit"), Plain.Get()))
		{
			TestEqual(TEXT("a widget that is not a variable has no property to match"), ObjectArray(Plain, TEXT("matches")).Num(), 0);
		}
		TestTrue(TEXT("HealthText is matched, not unmatched"), StringArray(Result, TEXT("matched_names")).Contains(TEXT("HealthText")));
		TestTrue(TEXT("DecorationText stays unmatched"), StringArray(Result, TEXT("unmatched_names")).Contains(TEXT("DecorationText")));
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
	{
		// StrictParams: a misspelled parameter is refused instead of returning an unfiltered result.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/NameLookup/None")}));
		Params->SetArrayField(TEXT("symbol"), JsonStrings({TEXT("Health")}));
		FCortexCommandResult Typo = Handler.Execute(TEXT("find_name_lookups"), Params);
		TestFalse(TEXT("misspelled symbols param is refused"), Typo.bSuccess);
		TestEqual(TEXT("misspelled param code"), Typo.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	{
		// An unusable out_path is refused before any work and writes nothing.
		const FString Forbidden = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), TEXT("Cortex120LookupRefused.json")));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/NameLookup/None")}));
		Params->SetStringField(TEXT("out_path"), Forbidden);
		FCortexCommandResult Refused = Handler.Execute(TEXT("find_name_lookups"), Params);
		TestFalse(TEXT("out_path outside the write policy is refused"), Refused.bSuccess);
		TestFalse(TEXT("refused out_path was not written"), IFileManager::Get().FileExists(*Forbidden));
	}
	{
		// With out_path the full result is written and the response is a compact summary.
		const FString OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Cortex120"), FString::Printf(TEXT("Lookup_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/NameLookup/None")}));
		Params->SetStringField(TEXT("out_path"), OutPath);
		FCortexCommandResult Summary = Handler.Execute(TEXT("find_name_lookups"), Params);
		if (TestTrue(*FString::Printf(TEXT("out_path run succeeds (%s)"), *Summary.ErrorMessage), Summary.bSuccess) && TestTrue(TEXT("summary data"), Summary.Data.IsValid()))
		{
			TestEqual(TEXT("summary names the file"), Summary.Data->GetStringField(TEXT("out_path")), OutPath);
			TestTrue(TEXT("summary counts cpp hits"), Summary.Data->HasField(TEXT("cpp_total")));
			TestTrue(TEXT("summary counts blueprint hits"), Summary.Data->HasField(TEXT("blueprint_total")));
			TestTrue(TEXT("summary keeps at most the first 10 cpp hits"), ObjectArray(Summary.Data, TEXT("cpp")).Num() <= 10);
			FString Text;
			TSharedPtr<FJsonObject> File;
			if (TestTrue(TEXT("report file is written"), FFileHelper::LoadFileToString(Text, *OutPath))
				&& TestTrue(TEXT("report parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), File) && File.IsValid()))
			{
				TestEqual(TEXT("file holds every cpp hit"), ObjectArray(File, TEXT("cpp")).Num(), static_cast<int32>(Summary.Data->GetNumberField(TEXT("cpp_total"))));
			}
		}
		IFileManager::Get().Delete(*OutPath);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectNameLookupUnreadableTest,
	"Cortex.Reflect.NameLookup.UnreadableFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectNameLookupUnreadableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectNameLookupTestSupport;

	FTempSource Source(TEXT("Locked.cpp"), TEXT("void A(UObject* O) { O->FindFunction(TEXT(\"Hidden\")); }\n"));
	const FString ReadableFile = FPaths::Combine(FPaths::GetPath(Source.File), TEXT("Readable.cpp"));
	FFileHelper::SaveStringToFile(TEXT("void B(UObject* O) { O->FindFunction(TEXT(\"Visible\")); }\n"), *ReadableFile);

	AddExpectedMessage(TEXT("find_name_lookups: could not read"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	// An open exclusive writer makes the file unreadable for the scan.
	TUniquePtr<FArchive> Lock(IFileManager::Get().CreateFileWriter(*Source.File, FILEWRITE_Append));
	if (!TestNotNull(TEXT("lock acquired"), Lock.Get()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Result = FCortexReflectNameLookupOps::FindNameLookupsInRoots(
		{Source.SourceRoot}, {TEXT("/Game/Temp/Cortex120/NameLookup/None")}, {});
	if (TestNotNull(TEXT("result"), Result.Get()))
	{
		TestEqual(TEXT("only the readable file counts as scanned"), static_cast<int32>(Result->GetNumberField(TEXT("scanned_files"))), 1);
		const TArray<FString> Unreadable = StringArray(Result, TEXT("unreadable_files"));
		if (TestEqual(TEXT("one unreadable file"), Unreadable.Num(), 1))
		{
			TestTrue(TEXT("the locked file is named"), Unreadable[0].EndsWith(TEXT("Locked.cpp")));
		}
		const TArray<TSharedPtr<FJsonObject>> Cpp = ObjectArray(Result, TEXT("cpp"));
		TestEqual(TEXT("only the readable file produced a hit"), Cpp.Num(), 1);
	}
	Lock.Reset();
	return true;
}
