#include "Operations/CortexBPSnapshotOps.h"

#include "Algo/Sort.h"
#include "Components/ActorComponent.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Misc/EngineVersion.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTextInventoryOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace CortexBPSnapshotPrivate
{
	/** Compact-JSON ceiling of one inline response, shared with the other bounded Cortex reads. */
	constexpr int32 SnapshotMaxInlineChars = 40000;

	namespace Section
	{
		const TCHAR* const Variables = TEXT("variables");
		const TCHAR* const Cdo = TEXT("cdo");
		const TCHAR* const Components = TEXT("components");
		const TCHAR* const Widgets = TEXT("widgets");
		const TCHAR* const Bindings = TEXT("bindings");
		const TCHAR* const Animations = TEXT("animations");
		const TCHAR* const Text = TEXT("text");
		const TCHAR* const Interfaces = TEXT("interfaces");
		const TCHAR* const GraphEntries = TEXT("graph_entries");
	}

	using FRecords = TArray<TSharedPtr<FJsonObject>>;

	bool KeyLess(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	}

	TSharedPtr<FJsonObject> NewRecord(const FString& Key)
	{
		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("key"), Key);
		return Record;
	}

	TArray<TSharedPtr<FJsonValue>> SortedValues(FRecords& Records)
	{
		Algo::SortBy(Records, [](const TSharedPtr<FJsonObject>& Record) { return Record->GetStringField(TEXT("key")); }, KeyLess);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Records.Num());
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			Values.Add(MakeShared<FJsonValueObject>(Record));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> SortedStringValues(TArray<FString> Strings)
	{
		Algo::Sort(Strings, KeyLess);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	// ---- reflection helpers (CortexBlueprint has no UMG dependency; see CortexBPAnalysisOps) ----

	bool IsWidgetBlueprint(const UBlueprint* Blueprint)
	{
		static const UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
		return WidgetBlueprintClass && Blueprint && Blueprint->IsA(WidgetBlueprintClass);
	}

	UObject* ReadObjectProperty(const UObject* Owner, const TCHAR* Name)
	{
		const FObjectProperty* Property = Owner ? CastField<FObjectProperty>(Owner->GetClass()->FindPropertyByName(Name)) : nullptr;
		return Property ? Property->GetObjectPropertyValue_InContainer(Owner) : nullptr;
	}

	FString ResolveWidgetParentName(const UObject* Widget)
	{
		const UObject* Slot = ReadObjectProperty(Widget, TEXT("Slot"));
		const UObject* Parent = ReadObjectProperty(Slot, TEXT("Parent"));
		return Parent ? Parent->GetName() : FString();
	}

	// ---- section builders ----

	TSharedPtr<FJsonValue> SerializeProperty(
		const FProperty* Property, const void* Container, const FString& Name, bool& bOutIncomplete);

	TArray<TSharedPtr<FJsonValue>> BuildVariables(const UBlueprint* Blueprint)
	{
		FRecords Records;
		// A full compile copies FBPVariableDescription::DefaultValue into the CDO and clears it, so the effective
		// default is read from the generated class CDO instead.
		const UClass* GeneratedClass = Blueprint->GeneratedClass;
		const UObject* DefaultsObject = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> Record = NewRecord(Variable.VarName.ToString());
			Record->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Variable.VarType));
			Record->SetStringField(TEXT("category"), Variable.Category.ToString());
			const FProperty* DefaultProperty = DefaultsObject ? GeneratedClass->FindPropertyByName(Variable.VarName) : nullptr;
			if (DefaultProperty)
			{
				bool bIncomplete = false;
				Record->SetField(TEXT("default"), SerializeProperty(DefaultProperty, DefaultsObject, Variable.VarName.ToString(), bIncomplete));
				if (bIncomplete)
				{
					Record->SetBoolField(TEXT("default_incomplete"), true);
				}
			}
			else
			{
				Record->SetField(TEXT("default"), MakeShared<FJsonValueNull>());
			}

			struct FFlagName { uint64 Flag; const TCHAR* Name; };
			static const FFlagName FlagNames[] = {
				{CPF_Edit, TEXT("editable")},
				{CPF_BlueprintReadOnly, TEXT("blueprint_read_only")},
				{CPF_DisableEditOnInstance, TEXT("disable_edit_on_instance")},
				{CPF_ExposeOnSpawn, TEXT("expose_on_spawn")},
				{CPF_Net, TEXT("replicated")},
				{CPF_SaveGame, TEXT("save_game")},
				{CPF_Transient, TEXT("transient")},
			};
			TArray<FString> Flags;
			for (const FFlagName& FlagName : FlagNames)
			{
				if ((Variable.PropertyFlags & FlagName.Flag) != 0)
				{
					Flags.Add(FlagName.Name);
				}
			}
			Record->SetArrayField(TEXT("flags"), SortedStringValues(Flags));
			Record->SetStringField(TEXT("rep_notify"), Variable.RepNotifyFunc.ToString());
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	bool IsSnapshotProperty(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
			&& !Property->HasAnyPropertyFlags(CPF_Deprecated)
			&& !CastField<FDelegateProperty>(Property)
			&& !CastField<FMulticastDelegateProperty>(Property);
	}

	TSharedPtr<FJsonValue> SerializeProperty(
		const FProperty* Property, const void* Container, const FString& Name, bool& bOutIncomplete)
	{
		const FCortexPropertySerializationResult Result = FCortexSerializer::PropertyToJsonDeep(
			Property, Property->ContainerPtrToValuePtr<void>(Container), FCortexSerializationPolicy(), Name);
		bOutIncomplete = Result.Issues.Num() > 0 || Result.bPartial;
		return Result.JsonValue.IsValid() ? Result.JsonValue : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	}

	TArray<TSharedPtr<FJsonValue>> BuildCdo(const UBlueprint* Blueprint)
	{
		FRecords Records;
		const UClass* GeneratedClass = Blueprint->GeneratedClass;
		const UObject* CDO = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
		if (!CDO)
		{
			return {};
		}
		for (TFieldIterator<FProperty> It(GeneratedClass); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsSnapshotProperty(Property))
			{
				continue;
			}
			const FString Name = Property->GetName();
			TSharedPtr<FJsonObject> Record = NewRecord(Name);
			const UClass* Owner = Property->GetOwnerClass();
			Record->SetStringField(TEXT("declared_in"), Owner ? Owner->GetPathName() : FString());
			Record->SetStringField(TEXT("type"), Property->GetCPPType());
			bool bIncomplete = false;
			Record->SetField(TEXT("value"), SerializeProperty(Property, CDO, Name, bIncomplete));
			if (bIncomplete)
			{
				Record->SetBoolField(TEXT("incomplete"), true);
			}
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	/** Editable properties of a component template that differ from its archetype. */
	TSharedPtr<FJsonObject> BuildOverrides(const UObject* Template)
	{
		TSharedPtr<FJsonObject> Overrides = MakeShared<FJsonObject>();
		const UObject* Archetype = Template ? Template->GetArchetype() : nullptr;
		if (!Archetype)
		{
			return Overrides;
		}
		for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if (!IsSnapshotProperty(Property) || Property->HasAnyPropertyFlags(CPF_Transient)
				|| !Owner || !Archetype->IsA(Owner))
			{
				continue;
			}
			if (!Property->Identical_InContainer(Template, Archetype, 0, PPF_DeepComparison))
			{
				bool bIncomplete = false;
				Overrides->SetField(Property->GetName(), SerializeProperty(Property, Template, Property->GetName(), bIncomplete));
			}
		}
		return Overrides;
	}

	void AddComponentFields(const TSharedPtr<FJsonObject>& Record, const UObject* Template)
	{
		Record->SetStringField(TEXT("class_path"), Template->GetClass()->GetPathName());
		Record->SetObjectField(TEXT("overrides"), BuildOverrides(Template));
	}

	TArray<TSharedPtr<FJsonValue>> BuildComponents(const UBlueprint* Blueprint)
	{
		FRecords Records;
		TSet<FString> Keys;

		// Own SCS nodes (including nodes that override an inherited component).
		if (Blueprint->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate)
				{
					continue;
				}
				const FString Key = TEXT("scs:") + Node->GetVariableName().ToString();
				if (Keys.Contains(Key))
				{
					continue;
				}
				Keys.Add(Key);
				TSharedPtr<FJsonObject> Record = NewRecord(Key);
				// The node is attached to a component of a parent class (ParentComponentOwnerClassName); it is not itself inherited.
				Record->SetBoolField(TEXT("attached_to_inherited"), !Node->ParentComponentOwnerClassName.IsNone());
				Record->SetStringField(TEXT("parent_component"), Node->ParentComponentOrVariableName.ToString());
				Record->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
				AddComponentFields(Record, Node->ComponentTemplate);
				Records.Add(Record);
			}
		}

		// Inherited templates as this Blueprint sees them. A parent Blueprint's SCS node is read through this Blueprint's
		// InheritableComponentHandler (where its own overrides of that component live); a native parent's default
		// subobjects are read from this Blueprint's generated-class CDO (whose subobjects carry its own overrides and
		// whose archetype is the parent's subobject).
		UBlueprintGeneratedClass* OwnClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		const UInheritableComponentHandler* OverrideHandler = OwnClass ? OwnClass->GetInheritableComponentHandler(false) : nullptr;
		for (const UClass* Class = Blueprint->ParentClass; Class; Class = Class->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Class))
			{
				if (!BlueprintClass->SimpleConstructionScript)
				{
					continue;
				}
				for (const USCS_Node* Node : BlueprintClass->SimpleConstructionScript->GetAllNodes())
				{
					if (!Node || !Node->ComponentTemplate)
					{
						continue;
					}
					const FString Key = TEXT("inherited:") + Node->GetVariableName().ToString();
					if (Keys.Contains(Key))
					{
						continue;
					}
					Keys.Add(Key);
					const UActorComponent* Template = Node->ComponentTemplate;
					if (OverrideHandler)
					{
						if (const UActorComponent* Override = OverrideHandler->GetOverridenComponentTemplate(FComponentKey(Node)))
						{
							Template = Override;
						}
					}
					TSharedPtr<FJsonObject> Record = NewRecord(Key);
					Record->SetStringField(TEXT("owner_class"), Class->GetPathName());
					AddComponentFields(Record, Template);
					Records.Add(Record);
				}
				continue;
			}

			UObject* Cdo = OwnClass ? OwnClass->GetDefaultObject() : Class->GetDefaultObject();
			TArray<UObject*> Subobjects;
			Cdo->GetDefaultSubobjects(Subobjects);
			for (const UObject* Subobject : Subobjects)
			{
				if (!Subobject || !Subobject->IsA<UActorComponent>())
				{
					continue;
				}
				const FString Key = TEXT("inherited:") + Subobject->GetName();
				if (Keys.Contains(Key))
				{
					continue;
				}
				Keys.Add(Key);
				TSharedPtr<FJsonObject> Record = NewRecord(Key);
				Record->SetStringField(TEXT("owner_class"), Class->GetPathName());
				AddComponentFields(Record, Subobject);
				Records.Add(Record);
			}
			break; // the first native class's CDO already carries every native subobject
		}
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildWidgets(const UBlueprint* Blueprint)
	{
		FRecords Records;
		static const UClass* WidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.Widget"));
		const UObject* WidgetTree = IsWidgetBlueprint(Blueprint) ? ReadObjectProperty(Blueprint, TEXT("WidgetTree")) : nullptr;
		if (!WidgetTree || !WidgetClass)
		{
			return {};
		}
		ForEachObjectWithOuter(WidgetTree, [&Records](UObject* Widget)
		{
			if (!Widget || Widget->HasAnyFlags(RF_ClassDefaultObject) || !Widget->IsA(WidgetClass))
			{
				return;
			}
			TSharedPtr<FJsonObject> Record = NewRecord(Widget->GetName());
			Record->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());
			const FBoolProperty* IsVariable = CastField<FBoolProperty>(Widget->GetClass()->FindPropertyByName(TEXT("bIsVariable")));
			Record->SetBoolField(TEXT("is_variable"), IsVariable && IsVariable->GetPropertyValue_InContainer(Widget));
			Record->SetStringField(TEXT("parent"), ResolveWidgetParentName(Widget));
			Records.Add(Record);
		});
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildBindings(const UBlueprint* Blueprint)
	{
		FRecords Records;
		const FArrayProperty* BindingsProperty = IsWidgetBlueprint(Blueprint)
			? CastField<FArrayProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("Bindings"))) : nullptr;
		const FStructProperty* BindingStruct = BindingsProperty ? CastField<FStructProperty>(BindingsProperty->Inner) : nullptr;
		if (!BindingStruct)
		{
			return {};
		}
		const FStrProperty* ObjectName = CastField<FStrProperty>(BindingStruct->Struct->FindPropertyByName(TEXT("ObjectName")));
		const FNameProperty* PropertyName = CastField<FNameProperty>(BindingStruct->Struct->FindPropertyByName(TEXT("PropertyName")));
		const FNameProperty* FunctionName = CastField<FNameProperty>(BindingStruct->Struct->FindPropertyByName(TEXT("FunctionName")));
		if (!ObjectName || !PropertyName || !FunctionName)
		{
			return {};
		}
		FScriptArrayHelper Bindings(BindingsProperty, BindingsProperty->ContainerPtrToValuePtr<void>(Blueprint));
		for (int32 Index = 0; Index < Bindings.Num(); ++Index)
		{
			const void* Binding = Bindings.GetRawPtr(Index);
			const FString Widget = ObjectName->GetPropertyValue_InContainer(Binding);
			const FString Property = PropertyName->GetPropertyValue_InContainer(Binding).ToString();
			TSharedPtr<FJsonObject> Record = NewRecord(Widget + TEXT(".") + Property);
			Record->SetStringField(TEXT("widget_name"), Widget);
			Record->SetStringField(TEXT("property"), Property);
			Record->SetStringField(TEXT("bound_function"), FunctionName->GetPropertyValue_InContainer(Binding).ToString());
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildAnimations(const UBlueprint* Blueprint)
	{
		FRecords Records;
		const FArrayProperty* AnimationsProperty = IsWidgetBlueprint(Blueprint)
			? CastField<FArrayProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("Animations"))) : nullptr;
		const FObjectProperty* AnimationInner = AnimationsProperty ? CastField<FObjectProperty>(AnimationsProperty->Inner) : nullptr;
		if (!AnimationInner)
		{
			return {};
		}
		FScriptArrayHelper Animations(AnimationsProperty, AnimationsProperty->ContainerPtrToValuePtr<void>(Blueprint));
		for (int32 Index = 0; Index < Animations.Num(); ++Index)
		{
			const UObject* Animation = AnimationInner->GetObjectPropertyValue(Animations.GetRawPtr(Index));
			if (!Animation)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Record = NewRecord(Animation->GetName());
			Record->SetStringField(TEXT("class_path"), Animation->GetClass()->GetPathName());

			TArray<FString> BoundWidgets;
			const FArrayProperty* BindingsProperty = CastField<FArrayProperty>(Animation->GetClass()->FindPropertyByName(TEXT("AnimationBindings")));
			const FStructProperty* BindingStruct = BindingsProperty ? CastField<FStructProperty>(BindingsProperty->Inner) : nullptr;
			const FNameProperty* WidgetName = BindingStruct
				? CastField<FNameProperty>(BindingStruct->Struct->FindPropertyByName(TEXT("WidgetName"))) : nullptr;
			if (WidgetName)
			{
				FScriptArrayHelper Bindings(BindingsProperty, BindingsProperty->ContainerPtrToValuePtr<void>(Animation));
				for (int32 BindingIndex = 0; BindingIndex < Bindings.Num(); ++BindingIndex)
				{
					const FName Name = WidgetName->GetPropertyValue_InContainer(Bindings.GetRawPtr(BindingIndex));
					if (!Name.IsNone())
					{
						BoundWidgets.AddUnique(Name.ToString());
					}
				}
			}
			Record->SetArrayField(TEXT("bound_widget_names"), SortedStringValues(BoundWidgets));
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildText(UBlueprint* Blueprint)
	{
		FRecords Entries;
		FCortexBPTextInventoryOps::CollectTextEntries(Blueprint, Entries);
		FRecords Records;
		for (const TSharedPtr<FJsonObject>& Entry : Entries)
		{
			TSharedPtr<FJsonObject> Record = NewRecord(FString::Printf(TEXT("%s|%s|%s"),
				*Entry->GetStringField(TEXT("location")), *Entry->GetStringField(TEXT("owner")), *Entry->GetStringField(TEXT("path"))));
			for (const auto& Field : Entry->Values)
			{
				// The collector's string table "key" would shadow the record identity.
				const FString FieldName(Field.Key.ToView());
				Record->SetField(FieldName == TEXT("key") ? FString(TEXT("string_table_key")) : FieldName, Field.Value);
			}
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildInterfaces(const UBlueprint* Blueprint)
	{
		FRecords Records;
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			if (!Interface.Interface)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Record = NewRecord(Interface.Interface->GetPathName());
			TArray<FString> Graphs;
			for (const UEdGraph* Graph : Interface.Graphs)
			{
				if (Graph)
				{
					Graphs.Add(Graph->GetName());
				}
			}
			Record->SetArrayField(TEXT("graphs"), SortedStringValues(Graphs));
			Records.Add(Record);
		}
		return SortedValues(Records);
	}

	TArray<TSharedPtr<FJsonValue>> BuildGraphEntries(UBlueprint* Blueprint)
	{
		FRecords Records;
		TSet<FString> Keys;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const TCHAR* Kind = nullptr;
				FString Name;
				if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					Kind = Event->IsA<UK2Node_CustomEvent>() ? TEXT("custom_event") : TEXT("event");
					Name = Event->GetFunctionName().ToString();
				}
				else if (Node && Node->IsA<UK2Node_FunctionEntry>())
				{
					Kind = TEXT("function_entry");
					Name = Graph->GetName();
				}
				else
				{
					continue;
				}

				FString Key = FString::Printf(TEXT("%s|%s|%s"), *Graph->GetName(), Kind, *Name);
				for (int32 Duplicate = 2; Keys.Contains(Key); ++Duplicate)
				{
					Key = FString::Printf(TEXT("%s|%s|%s#%d"), *Graph->GetName(), Kind, *Name, Duplicate);
				}
				Keys.Add(Key);
				TSharedPtr<FJsonObject> Record = NewRecord(Key);
				Record->SetStringField(TEXT("graph"), Graph->GetName());
				Record->SetStringField(TEXT("kind"), Kind);
				Record->SetStringField(TEXT("name"), Name);
				Record->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				Records.Add(Record);
			}
		}
		return SortedValues(Records);
	}

	// ---- compare ----

	struct FSnapshotFile
	{
		FCortexResolvedFilePath Path;
		TSharedPtr<FJsonObject> Root;
		TSharedPtr<FJsonObject> Sections;
		int32 Version = 0;
	};

	bool LoadSnapshotFile(const FString& Requested, const TCHAR* Label, FSnapshotFile& Out, FCortexCommandResult& OutError)
	{
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveReadPath(Requested, Out.Path, ErrorCode, ErrorMessage))
		{
			OutError = FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
			return false;
		}
		FString Contents;
		if (!FCortexSafeFileContract::ReadTextFile(Out.Path, Contents, ErrorCode, ErrorMessage))
		{
			OutError = FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, Out.Root) || !Out.Root.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::MalformedJson,
				FString::Printf(TEXT("%s snapshot is not valid JSON: %s"), Label, *Out.Path.AbsolutePath));
			return false;
		}
		double Version = 0.0;
		const TSharedPtr<FJsonObject>* Sections = nullptr;
		if (!Out.Root->TryGetNumberField(TEXT("format_version"), Version) || !Out.Root->TryGetObjectField(TEXT("sections"), Sections))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s file is not a Blueprint snapshot (needs format_version and sections): %s"),
					Label, *Out.Path.AbsolutePath));
			return false;
		}
		Out.Version = static_cast<int32>(Version);
		Out.Sections = *Sections;
		return true;
	}

	bool ReadRecords(const FSnapshotFile& File, const TCHAR* Label, const FString& SectionName,
		TMap<FString, TSharedPtr<FJsonObject>>& OutRecords, FCortexCommandResult& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!File.Sections->TryGetArrayField(SectionName, Values))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s snapshot section '%s' is malformed"), Label, *SectionName));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Record = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Key;
			if (!Record.IsValid() || !Record->TryGetStringField(TEXT("key"), Key))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s snapshot section '%s' holds a record without a key"), Label, *SectionName));
				return false;
			}
			if (OutRecords.Contains(Key))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s snapshot section '%s' holds the record key '%s' more than once"), Label, *SectionName, *Key));
				return false;
			}
			OutRecords.Add(Key, Record);
		}
		return true;
	}

	/** Equal on every field except those ignored; the `value` field of a float/double pair compares at float precision. */
	bool FieldsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B,
		const TSet<FString>& Ignored = TSet<FString>(), const bool bFloatPrecisionValue = false)
	{
		TSet<FString> Names;
		for (const auto& Pair : A->Values) { Names.Add(FString(Pair.Key.ToView())); }
		for (const auto& Pair : B->Values) { Names.Add(FString(Pair.Key.ToView())); }
		for (const FString& Name : Names)
		{
			if (Ignored.Contains(Name))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Left = A->TryGetField(Name);
			const TSharedPtr<FJsonValue> Right = B->TryGetField(Name);
			if (!Left.IsValid() || !Right.IsValid())
			{
				return false;
			}
			double LeftNumber = 0.0;
			double RightNumber = 0.0;
			if (bFloatPrecisionValue && Name == TEXT("value") && Left->TryGetNumber(LeftNumber) && Right->TryGetNumber(RightNumber))
			{
				if (static_cast<float>(LeftNumber) != static_cast<float>(RightNumber))
				{
					return false;
				}
				continue;
			}
			if (!FJsonValue::CompareEqual(*Left, *Right))
			{
				return false;
			}
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> ChangedFieldNames(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		TSet<FString> Names;
		for (const auto& Pair : A->Values) { Names.Add(FString(Pair.Key.ToView())); }
		for (const auto& Pair : B->Values) { Names.Add(FString(Pair.Key.ToView())); }
		TArray<FString> Changed;
		for (const FString& Name : Names)
		{
			const TSharedPtr<FJsonValue> Left = A->TryGetField(Name);
			const TSharedPtr<FJsonValue> Right = B->TryGetField(Name);
			if (!Left.IsValid() || !Right.IsValid() || !FJsonValue::CompareEqual(*Left, *Right))
			{
				Changed.Add(Name);
			}
		}
		return SortedStringValues(Changed);
	}

	TSharedPtr<FJsonObject> MakeDifference(const FString& SectionName, const FString& Key, const TCHAR* Kind,
		const TSharedPtr<FJsonObject>& Before, const TSharedPtr<FJsonObject>& After)
	{
		TSharedPtr<FJsonObject> Difference = MakeShared<FJsonObject>();
		Difference->SetStringField(TEXT("section"), SectionName);
		Difference->SetStringField(TEXT("key"), Key);
		Difference->SetStringField(TEXT("kind"), Kind);
		if (Before.IsValid())
		{
			Difference->SetObjectField(TEXT("before"), Before);
		}
		if (After.IsValid())
		{
			Difference->SetObjectField(TEXT("after"), After);
		}
		return Difference;
	}

	FString SerializeCompact(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}
}

const TArray<FString>& FCortexBPSnapshotOps::AllSectionNames()
{
	using namespace CortexBPSnapshotPrivate;
	static const TArray<FString> Names = {
		Section::Variables, Section::Cdo, Section::Components, Section::Widgets, Section::Bindings,
		Section::Animations, Section::Text, Section::Interfaces, Section::GraphEntries};
	return Names;
}

TSharedPtr<FJsonObject> FCortexBPSnapshotOps::BuildSnapshot(UBlueprint* Blueprint, const TSet<FString>& Sections, FString& OutError)
{
	using namespace CortexBPSnapshotPrivate;
	OutError.Reset();
	for (const FString& Name : Sections)
	{
		if (!AllSectionNames().Contains(Name))
		{
			OutError = FString::Printf(TEXT("Unknown snapshot section '%s' (valid: %s)"), *Name, *FString::Join(AllSectionNames(), TEXT(", ")));
			return nullptr;
		}
	}
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	const auto Wants = [&Sections](const TCHAR* Name) { return Sections.Num() == 0 || Sections.Contains(Name); };
	TSharedPtr<FJsonObject> Built = MakeShared<FJsonObject>();
	if (Wants(Section::Variables)) { Built->SetArrayField(Section::Variables, BuildVariables(Blueprint)); }
	if (Wants(Section::Cdo)) { Built->SetArrayField(Section::Cdo, BuildCdo(Blueprint)); }
	if (Wants(Section::Components)) { Built->SetArrayField(Section::Components, BuildComponents(Blueprint)); }
	if (Wants(Section::Widgets)) { Built->SetArrayField(Section::Widgets, BuildWidgets(Blueprint)); }
	if (Wants(Section::Bindings)) { Built->SetArrayField(Section::Bindings, BuildBindings(Blueprint)); }
	if (Wants(Section::Animations)) { Built->SetArrayField(Section::Animations, BuildAnimations(Blueprint)); }
	if (Wants(Section::Text)) { Built->SetArrayField(Section::Text, BuildText(Blueprint)); }
	if (Wants(Section::Interfaces)) { Built->SetArrayField(Section::Interfaces, BuildInterfaces(Blueprint)); }
	if (Wants(Section::GraphEntries)) { Built->SetArrayField(Section::GraphEntries, BuildGraphEntries(Blueprint)); }

	TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetNumberField(TEXT("format_version"), FormatVersion);
	Snapshot->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Snapshot->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
	Snapshot->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Snapshot->SetObjectField(TEXT("sections"), Built);
	return Snapshot;
}

FCortexCommandResult FCortexBPSnapshotOps::ExportSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexBPSnapshotPrivate;
	FString AssetPath;
	FString OutPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: out_path"));
	}

	TSet<FString> Sections;
	const TArray<TSharedPtr<FJsonValue>>* SectionValues = nullptr;
	if (Params->HasField(TEXT("sections")))
	{
		if (!Params->TryGetArrayField(TEXT("sections"), SectionValues))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("sections must be an array of section names"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *SectionValues)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("sections must be an array of section names"));
			}
			if (!AllSectionNames().Contains(Name))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown snapshot section '%s' (valid: %s)"), *Name, *FString::Join(AllSectionNames(), TEXT(", "))));
			}
			Sections.Add(Name);
		}
	}

	// An unusable out_path is refused before any asset work.
	FCortexResolvedFilePath ResolvedOutPath;
	FString PathErrorCode;
	FString PathErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(OutPath, ResolvedOutPath, PathErrorCode, PathErrorMessage))
	{
		return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, LoadError);
	}

	FString BuildError;
	const TSharedPtr<FJsonObject> Snapshot = BuildSnapshot(Blueprint, Sections, BuildError);
	if (!Snapshot.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, BuildError);
	}

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Snapshot.ToSharedRef());
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode, WriteResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
	for (const auto& Pair : Snapshot->GetObjectField(TEXT("sections"))->Values)
	{
		Counts->SetNumberField(FString(Pair.Key.ToView()), Pair.Value->AsArray().Num());
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("out_path"), ResolvedOutPath.AbsolutePath);
	Data->SetNumberField(TEXT("format_version"), FormatVersion);
	Data->SetObjectField(TEXT("counts"), Counts);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPSnapshotOps::CompareSnapshots(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexBPSnapshotPrivate;
	FString BeforePath;
	FString AfterPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("before"), BeforePath) || BeforePath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("after"), AfterPath) || AfterPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required params: before and after"));
	}
	FString ReportPath;
	const bool bReport = Params->TryGetStringField(TEXT("report_path"), ReportPath);

	FCortexResolvedFilePath ResolvedReportPath;
	FCortexCommandResult Error;
	FSnapshotFile Before;
	FSnapshotFile After;
	if (!LoadSnapshotFile(BeforePath, TEXT("before"), Before, Error) || !LoadSnapshotFile(AfterPath, TEXT("after"), After, Error))
	{
		return Error;
	}
	if (FCortexSafeFileContract::AreSameCanonicalFile(Before.Path.AbsolutePath, After.Path.AbsolutePath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidFilePath, TEXT("before and after must be different files"));
	}
	if (bReport)
	{
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(ReportPath, ResolvedReportPath, ErrorCode, ErrorMessage)
			|| !FCortexSafeFileContract::PrepareWritePath(ResolvedReportPath, ErrorCode, ErrorMessage))
		{
			return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		}
		if (FCortexSafeFileContract::AreSameCanonicalFile(ResolvedReportPath.AbsolutePath, Before.Path.AbsolutePath)
			|| FCortexSafeFileContract::AreSameCanonicalFile(ResolvedReportPath.AbsolutePath, After.Path.AbsolutePath))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidFilePath,
				TEXT("report_path must resolve to a different file than before and after"));
		}
	}

	if (Before.Version != After.Version)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Snapshot format_version mismatch: before is %d, after is %d"), Before.Version, After.Version));
	}
	if (Before.Version != FormatVersion)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Unsupported snapshot format_version %d (supported: %d)"), Before.Version, FormatVersion));
	}

	// A section recorded on one side only cannot be compared; it is never read as empty.
	TArray<FString> Compared;
	for (const FString& Name : AllSectionNames())
	{
		const bool bInBefore = Before.Sections->HasField(Name);
		const bool bInAfter = After.Sections->HasField(Name);
		if (bInBefore != bInAfter)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Snapshot section '%s' is present only in the %s snapshot; export both with the same sections"),
					*Name, bInBefore ? TEXT("before") : TEXT("after")));
		}
		if (bInBefore)
		{
			Compared.Add(Name);
		}
	}
	if (Compared.Num() == 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("The snapshots record no section to compare; export them with at least one section"));
	}

	TArray<TSharedPtr<FJsonValue>> Differences;
	TSharedPtr<FJsonObject> SectionCounts = MakeShared<FJsonObject>();
	int32 Totals[4] = {0, 0, 0, 0}; // added, removed, changed, moved
	for (const FString& Name : Compared)
	{
		TMap<FString, TSharedPtr<FJsonObject>> BeforeRecords;
		TMap<FString, TSharedPtr<FJsonObject>> AfterRecords;
		if (!ReadRecords(Before, TEXT("before"), Name, BeforeRecords, Error) || !ReadRecords(After, TEXT("after"), Name, AfterRecords, Error))
		{
			return Error;
		}
		TSet<FString> KeySet;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : BeforeRecords) { KeySet.Add(Pair.Key); }
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : AfterRecords) { KeySet.Add(Pair.Key); }
		TArray<FString> Keys = KeySet.Array();
		Algo::Sort(Keys, KeyLess);

		int32 Counts[4] = {0, 0, 0, 0};
		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonObject>* Old = BeforeRecords.Find(Key);
			const TSharedPtr<FJsonObject>* New = AfterRecords.Find(Key);
			if (!Old)
			{
				Differences.Add(MakeShared<FJsonValueObject>(MakeDifference(Name, Key, TEXT("added"), nullptr, *New)));
				++Counts[0];
			}
			else if (!New)
			{
				Differences.Add(MakeShared<FJsonValueObject>(MakeDifference(Name, Key, TEXT("removed"), *Old, nullptr)));
				++Counts[1];
			}
			else if (!FieldsEqual(*Old, *New))
			{
				// A CDO property whose declaring class changed while its value stayed equal moved, it did not change.
				// The type text may differ across the move (a Blueprint float is a double, a native bool is a bitfield), so
				// the decision is made on the value alone; float/double pairs are compared at float precision.
				bool bMoved = false;
				TSharedPtr<FJsonObject> TypeChange;
				if (Name == Section::Cdo && (*Old)->GetStringField(TEXT("declared_in")) != (*New)->GetStringField(TEXT("declared_in")))
				{
					const FString OldType = (*Old)->GetStringField(TEXT("type"));
					const FString NewType = (*New)->GetStringField(TEXT("type"));
					const bool bFloatPair = (OldType == TEXT("double") && NewType == TEXT("float"))
						|| (OldType == TEXT("float") && NewType == TEXT("double"));
					static const TSet<FString> MoveIgnored = {TEXT("declared_in"), TEXT("type"), TEXT("incomplete")};
					bMoved = FieldsEqual(*Old, *New, MoveIgnored, bFloatPair);
					if (bMoved && OldType != NewType)
					{
						TypeChange = MakeShared<FJsonObject>();
						TypeChange->SetStringField(TEXT("before"), OldType);
						TypeChange->SetStringField(TEXT("after"), NewType);
					}
				}
				TSharedPtr<FJsonObject> Difference = MakeDifference(Name, Key, bMoved ? TEXT("moved") : TEXT("changed"), *Old, *New);
				if (!bMoved)
				{
					Difference->SetArrayField(TEXT("fields"), ChangedFieldNames(*Old, *New));
				}
				if (TypeChange.IsValid())
				{
					Difference->SetObjectField(TEXT("type_change"), TypeChange);
				}
				if (bMoved && ((*Old)->HasField(TEXT("incomplete")) || (*New)->HasField(TEXT("incomplete"))))
				{
					Difference->SetBoolField(TEXT("incomplete"), true); // an unproven value comparison is flagged, never silent
				}
				Differences.Add(MakeShared<FJsonValueObject>(Difference));
				++Counts[bMoved ? 3 : 2];
			}
		}

		TSharedPtr<FJsonObject> CountJson = MakeShared<FJsonObject>();
		CountJson->SetNumberField(TEXT("added"), Counts[0]);
		CountJson->SetNumberField(TEXT("removed"), Counts[1]);
		CountJson->SetNumberField(TEXT("changed"), Counts[2]);
		CountJson->SetNumberField(TEXT("moved"), Counts[3]);
		SectionCounts->SetObjectField(Name, CountJson);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Totals[Index] += Counts[Index];
		}
	}

	TSharedPtr<FJsonObject> TotalsJson = MakeShared<FJsonObject>();
	TotalsJson->SetNumberField(TEXT("added"), Totals[0]);
	TotalsJson->SetNumberField(TEXT("removed"), Totals[1]);
	TotalsJson->SetNumberField(TEXT("changed"), Totals[2]);
	TotalsJson->SetNumberField(TEXT("moved"), Totals[3]);

	TSharedPtr<FJsonObject> ParentClass = MakeShared<FJsonObject>();
	ParentClass->SetStringField(TEXT("before"), Before.Root->GetStringField(TEXT("parent_class")));
	ParentClass->SetStringField(TEXT("after"), After.Root->GetStringField(TEXT("parent_class")));

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(TEXT("before"), Before.Path.AbsolutePath);
	Summary->SetStringField(TEXT("after"), After.Path.AbsolutePath);
	Summary->SetNumberField(TEXT("format_version"), Before.Version);
	Summary->SetObjectField(TEXT("parent_class"), ParentClass);
	TArray<TSharedPtr<FJsonValue>> ComparedValues;
	for (const FString& Name : Compared)
	{
		ComparedValues.Add(MakeShared<FJsonValueString>(Name));
	}
	Summary->SetArrayField(TEXT("sections_compared"), ComparedValues);
	Summary->SetBoolField(TEXT("identical"), Differences.Num() == 0);
	Summary->SetNumberField(TEXT("difference_count"), Differences.Num());
	Summary->SetObjectField(TEXT("totals"), TotalsJson);
	Summary->SetObjectField(TEXT("counts"), SectionCounts);

	if (bReport)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		for (const auto& Pair : Summary->Values)
		{
			Report->SetField(FString(Pair.Key.ToView()), Pair.Value);
		}
		Report->SetArrayField(TEXT("differences"), Differences);
		const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedReportPath, Report);
		if (!WriteResult.bWritten)
		{
			return FCortexCommandRouter::Error(
				WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode, WriteResult.ErrorMessage);
		}
		Summary->SetStringField(TEXT("report_path"), ResolvedReportPath.AbsolutePath);
		return FCortexCommandRouter::Success(Summary);
	}

	Summary->SetArrayField(TEXT("differences"), Differences);
	if (SerializeCompact(Summary.ToSharedRef()).Len() > SnapshotMaxInlineChars)
	{
		Summary->RemoveField(TEXT("differences"));
		Summary->SetStringField(TEXT("_error"), TEXT("RESPONSE_TOO_LARGE"));
		Summary->SetBoolField(TEXT("reader_complete"), false);
		Summary->SetNumberField(TEXT("max_response_chars"), SnapshotMaxInlineChars);
		Summary->SetStringField(TEXT("hint"), TEXT("Pass report_path to write the full report to a file."));
	}
	return FCortexCommandRouter::Success(Summary);
}
