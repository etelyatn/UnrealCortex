#include "Operations/CortexGraphNodeContract.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Timeline.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Engine/TimelineTemplate.h"

namespace
{
const FString VarGetPrerequisite = TEXT("Referenced UMG designer widgets must have is_variable=true; call umg.set_widget_variable before referencing them from a graph.");
const FString VarGetNonRetryable = TEXT("VARIABLE_NOT_FOUND, INVALID_FIELD");
const FString CallFunctionNonRetryable = TEXT("INVALID_FIELD");

struct FCortexFamilyMapping
{
	FName FamilyName;
	FString ResolvedClass;
	UClass* (*GetClassFunc)();
	TArray<FString> Aliases;
};
}

bool FCortexGraphNodeContract::ResolveFamily(
	const FString& NodeClassOrAlias,
	FName& OutFamilyName,
	UClass*& OutNodeClass)
{
	OutFamilyName = NAME_None;
	OutNodeClass = nullptr;

	if (NodeClassOrAlias.IsEmpty())
	{
		return false;
	}

	static const FCortexFamilyMapping Registry[] = {
		{
			FName("CallFunction"),
			TEXT("K2Node_CallFunction"),
			[]() -> UClass* { return UK2Node_CallFunction::StaticClass(); },
			{ TEXT("CallFunction"), TEXT("UK2Node_CallFunction"), TEXT("K2Node_CallFunction"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction") }
		},
		{
			FName("IfThenElse"),
			TEXT("K2Node_IfThenElse"),
			[]() -> UClass* { return UK2Node_IfThenElse::StaticClass(); },
			{ TEXT("IfThenElse"), TEXT("UK2Node_IfThenElse"), TEXT("K2Node_IfThenElse"), TEXT("Branch"), TEXT("/Script/BlueprintGraph.K2Node_IfThenElse") }
		},
		{
			FName("VariableSet"),
			TEXT("K2Node_VariableSet"),
			[]() -> UClass* { return UK2Node_VariableSet::StaticClass(); },
			{ TEXT("VariableSet"), TEXT("UK2Node_VariableSet"), TEXT("K2Node_VariableSet"), TEXT("/Script/BlueprintGraph.K2Node_VariableSet") }
		},
		{
			FName("VariableGet"),
			TEXT("K2Node_VariableGet"),
			[]() -> UClass* { return UK2Node_VariableGet::StaticClass(); },
			{ TEXT("VariableGet"), TEXT("UK2Node_VariableGet"), TEXT("K2Node_VariableGet"), TEXT("/Script/BlueprintGraph.K2Node_VariableGet") }
		},
		{
			FName("Event"),
			TEXT("K2Node_Event"),
			[]() -> UClass* { return UK2Node_Event::StaticClass(); },
			{ TEXT("Event"), TEXT("UK2Node_Event"), TEXT("K2Node_Event"), TEXT("/Script/BlueprintGraph.K2Node_Event") }
		},
		{
			FName("ExecutionSequence"),
			TEXT("K2Node_ExecutionSequence"),
			[]() -> UClass* { return UK2Node_ExecutionSequence::StaticClass(); },
			{ TEXT("ExecutionSequence"), TEXT("UK2Node_ExecutionSequence"), TEXT("K2Node_ExecutionSequence"), TEXT("Sequence"), TEXT("/Script/BlueprintGraph.K2Node_ExecutionSequence") }
		},
		{
			FName("CustomEvent"),
			TEXT("K2Node_CustomEvent"),
			[]() -> UClass* { return UK2Node_CustomEvent::StaticClass(); },
			{ TEXT("CustomEvent"), TEXT("UK2Node_CustomEvent"), TEXT("K2Node_CustomEvent"), TEXT("/Script/BlueprintGraph.K2Node_CustomEvent") }
		},
		{
			FName("Self"),
			TEXT("K2Node_Self"),
			[]() -> UClass* { return UK2Node_Self::StaticClass(); },
			{ TEXT("Self"), TEXT("UK2Node_Self"), TEXT("K2Node_Self"), TEXT("/Script/BlueprintGraph.K2Node_Self") }
		},
		{
			FName("Knot"),
			TEXT("K2Node_Knot"),
			[]() -> UClass* { return UK2Node_Knot::StaticClass(); },
			{ TEXT("Knot"), TEXT("UK2Node_Knot"), TEXT("K2Node_Knot"), TEXT("Reroute"), TEXT("/Script/BlueprintGraph.K2Node_Knot") }
		},
		{
			FName("MakeArray"),
			TEXT("K2Node_MakeArray"),
			[]() -> UClass* { return UK2Node_MakeArray::StaticClass(); },
			{ TEXT("MakeArray"), TEXT("UK2Node_MakeArray"), TEXT("K2Node_MakeArray"), TEXT("/Script/BlueprintGraph.K2Node_MakeArray") }
		},
		{
			FName("Timeline"),
			TEXT("K2Node_Timeline"),
			[]() -> UClass* { return UK2Node_Timeline::StaticClass(); },
			{ TEXT("Timeline"), TEXT("UK2Node_Timeline"), TEXT("K2Node_Timeline"), TEXT("/Script/BlueprintGraph.K2Node_Timeline") }
		},
		{
			FName("SpawnActorFromClass"),
			TEXT("K2Node_SpawnActorFromClass"),
			[]() -> UClass* { return UK2Node_SpawnActorFromClass::StaticClass(); },
			{ TEXT("SpawnActorFromClass"), TEXT("UK2Node_SpawnActorFromClass"), TEXT("K2Node_SpawnActorFromClass"), TEXT("SpawnActor"), TEXT("/Script/BlueprintGraph.K2Node_SpawnActorFromClass") }
		},
		{
			FName("DynamicCast"),
			TEXT("K2Node_DynamicCast"),
			[]() -> UClass* { return UK2Node_DynamicCast::StaticClass(); },
			{ TEXT("DynamicCast"), TEXT("UK2Node_DynamicCast"), TEXT("K2Node_DynamicCast"), TEXT("CastTo"), TEXT("Cast"), TEXT("/Script/BlueprintGraph.K2Node_DynamicCast") }
		},
		{
			FName("MacroInstance"),
			TEXT("K2Node_MacroInstance"),
			[]() -> UClass* { return UK2Node_MacroInstance::StaticClass(); },
			{ TEXT("MacroInstance"), TEXT("UK2Node_MacroInstance"), TEXT("K2Node_MacroInstance"), TEXT("/Script/BlueprintGraph.K2Node_MacroInstance") }
		},
		{
			FName("SwitchEnum"),
			TEXT("K2Node_SwitchEnum"),
			[]() -> UClass* { return UK2Node_SwitchEnum::StaticClass(); },
			{ TEXT("SwitchEnum"), TEXT("UK2Node_SwitchEnum"), TEXT("K2Node_SwitchEnum"), TEXT("/Script/BlueprintGraph.K2Node_SwitchEnum") }
		},
		{
			FName("SwitchString"),
			TEXT("K2Node_SwitchString"),
			[]() -> UClass* { return UK2Node_SwitchString::StaticClass(); },
			{ TEXT("SwitchString"), TEXT("UK2Node_SwitchString"), TEXT("K2Node_SwitchString"), TEXT("/Script/BlueprintGraph.K2Node_SwitchString") }
		},
		{
			FName("SwitchInteger"),
			TEXT("K2Node_SwitchInteger"),
			[]() -> UClass* { return UK2Node_SwitchInteger::StaticClass(); },
			{ TEXT("SwitchInteger"), TEXT("UK2Node_SwitchInteger"), TEXT("K2Node_SwitchInteger"), TEXT("/Script/BlueprintGraph.K2Node_SwitchInteger") }
		},
		{
			FName("AddDelegate"),
			TEXT("K2Node_AddDelegate"),
			[]() -> UClass* { return UK2Node_AddDelegate::StaticClass(); },
			{ TEXT("AddDelegate"), TEXT("UK2Node_AddDelegate"), TEXT("K2Node_AddDelegate"), TEXT("BindEvent"), TEXT("/Script/BlueprintGraph.K2Node_AddDelegate") }
		},
		{
			FName("RemoveDelegate"),
			TEXT("K2Node_RemoveDelegate"),
			[]() -> UClass* { return UK2Node_RemoveDelegate::StaticClass(); },
			{ TEXT("RemoveDelegate"), TEXT("UK2Node_RemoveDelegate"), TEXT("K2Node_RemoveDelegate"), TEXT("UnbindEvent"), TEXT("/Script/BlueprintGraph.K2Node_RemoveDelegate") }
		},
		{
			FName("ClearDelegate"),
			TEXT("K2Node_ClearDelegate"),
			[]() -> UClass* { return UK2Node_ClearDelegate::StaticClass(); },
			{ TEXT("ClearDelegate"), TEXT("UK2Node_ClearDelegate"), TEXT("K2Node_ClearDelegate"), TEXT("UnbindAllEvents"), TEXT("/Script/BlueprintGraph.K2Node_ClearDelegate") }
		},
		{
			FName("CreateDelegate"),
			TEXT("K2Node_CreateDelegate"),
			[]() -> UClass* { return UK2Node_CreateDelegate::StaticClass(); },
			{ TEXT("CreateDelegate"), TEXT("UK2Node_CreateDelegate"), TEXT("K2Node_CreateDelegate"), TEXT("CreateEvent"), TEXT("/Script/BlueprintGraph.K2Node_CreateDelegate") }
		},
		{
			FName("Composite"),
			TEXT("K2Node_Composite"),
			[]() -> UClass* { return UK2Node_Composite::StaticClass(); },
			{ TEXT("Composite"), TEXT("UK2Node_Composite"), TEXT("K2Node_Composite"), TEXT("/Script/BlueprintGraph.K2Node_Composite") }
		},
		{
			FName("ConstructObject"),
			TEXT("K2Node_GenericCreateObject"),
			[]() -> UClass* { return UK2Node_GenericCreateObject::StaticClass(); },
			{ TEXT("ConstructObject"), TEXT("GenericCreateObject"), TEXT("UK2Node_GenericCreateObject"), TEXT("K2Node_GenericCreateObject"), TEXT("/Script/BlueprintGraph.K2Node_GenericCreateObject") }
		}
	};

	for (const FCortexFamilyMapping& Mapping : Registry)
	{
		for (const FString& Alias : Mapping.Aliases)
		{
			if (Alias.Equals(NodeClassOrAlias, ESearchCase::IgnoreCase))
			{
				OutFamilyName = Mapping.FamilyName;
				OutNodeClass = Mapping.GetClassFunc();
				return true;
			}
		}
	}

	// Not in registry: try resolving canonical class path
	FCortexCommandResult Error;
	UClass* Resolved = nullptr;
	if (FCortexGraphSymbolResolver::ResolveClass(NodeClassOrAlias, Resolved, Error))
	{
		if (Resolved && Resolved->IsChildOf(UEdGraphNode::StaticClass()))
		{
			OutFamilyName = Resolved->GetFName();
			OutNodeClass = Resolved;
			return true;
		}
	}

	return false;
}

TSharedRef<FJsonObject> FCortexNodeConstructionContract::ToJson() const
{
	TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
	Contract->SetStringField(TEXT("node_class"), NodeClass);
	Contract->SetStringField(TEXT("resolved_class"), ResolvedClass);
	Contract->SetBoolField(TEXT("supported"), bSupported);

	auto ParamToJson = [](const FCortexNodeConstructionParam& Param) -> TSharedRef<FJsonObject>
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Param.Name);
		Obj->SetStringField(TEXT("type"), Param.Type);
		Obj->SetBoolField(TEXT("required"), Param.bRequired);
		Obj->SetStringField(TEXT("description"), Param.Description);
		return Obj;
	};

	TArray<TSharedPtr<FJsonValue>> RequiredJson;
	for (const FCortexNodeConstructionParam& Param : RequiredParams)
	{
		RequiredJson.Add(MakeShared<FJsonValueObject>(ParamToJson(Param)));
	}
	Contract->SetArrayField(TEXT("required_params"), RequiredJson);

	TArray<TSharedPtr<FJsonValue>> OptionalJson;
	for (const FCortexNodeConstructionParam& Param : OptionalParams)
	{
		OptionalJson.Add(MakeShared<FJsonValueObject>(ParamToJson(Param)));
	}
	Contract->SetArrayField(TEXT("optional_params"), OptionalJson);

	TArray<TSharedPtr<FJsonValue>> SelectorJson;
	for (const FString& Selector : Selectors)
	{
		SelectorJson.Add(MakeShared<FJsonValueString>(Selector));
	}
	Contract->SetArrayField(TEXT("selectors"), SelectorJson);

	TArray<TSharedPtr<FJsonValue>> PinJson;
	for (const FCortexNodePinPreview& Pin : ExpectedPins)
	{
		TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin.Name);
		PinObj->SetStringField(TEXT("direction"), Pin.Direction);
		PinObj->SetStringField(TEXT("type"), Pin.Type);
		PinJson.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Contract->SetArrayField(TEXT("expected_pins"), PinJson);
	Contract->SetBoolField(TEXT("pins_allocated"), bPinsAllocated);
	Contract->SetStringField(TEXT("prerequisites"), Prerequisites);
	Contract->SetStringField(TEXT("non_retryable_errors"), NonRetryableErrors);
	return Contract;
}

FCortexNodeConstructionContract FCortexGraphNodeContract::Describe(const FString& NodeClassName)
{
	FCortexNodeConstructionContract Contract;
	Contract.NodeClass = NodeClassName;

	FName FamilyName;
	UClass* NodeClass = nullptr;
	if (!ResolveFamily(NodeClassName, FamilyName, NodeClass))
	{
		Contract.ResolvedClass = TEXT("");
		Contract.bSupported = false;
		return Contract;
	}

	auto Set = [&Contract](const FString& ResolvedClass) -> void
	{
		Contract.ResolvedClass = ResolvedClass;
		Contract.bSupported = true;
	};

	if (FamilyName == FName("CallFunction"))
	{
		Set(TEXT("K2Node_CallFunction"));
		Contract.RequiredParams.Add({ TEXT("function_name"), TEXT("string"), true, TEXT("Canonical owner/function selector in ClassName.FunctionName format, e.g. KismetSystemLibrary.PrintString, or separate owner_class + function_name") });
		Contract.OptionalParams.Add({ TEXT("owner_class"), TEXT("string"), false, TEXT("Canonical owner class path, e.g. /Script/Engine.KismetSystemLibrary") });
		Contract.OptionalParams.Add({ TEXT("call_kind"), TEXT("string"), false, TEXT("Call kind: ordinary, interface_message") });
		Contract.Prerequisites = TEXT("Resolve the function selector with describe_node before wiring pins.");
		Contract.NonRetryableErrors = CallFunctionNonRetryable;
	}
	else if (FamilyName == FName("IfThenElse"))
	{
		Set(TEXT("K2Node_IfThenElse"));
	}
	else if (FamilyName == FName("VariableSet") || FamilyName == FName("VariableGet"))
	{
		Set(FamilyName == FName("VariableSet") ? TEXT("K2Node_VariableSet") : TEXT("K2Node_VariableGet"));
		Contract.RequiredParams.Add({ TEXT("variable_name"), TEXT("string"), true, TEXT("Property or Blueprint variable name to reference") });
		Contract.OptionalParams.Add({ TEXT("variable_class"), TEXT("string"), false, TEXT("Owner class for external class properties, e.g. Actor for bHidden") });
		Contract.OptionalParams.Add({ TEXT("owner_class"), TEXT("string"), false, TEXT("Canonical owner class path, e.g. /Script/Engine.Actor") });
		Contract.Prerequisites = VarGetPrerequisite;
		Contract.NonRetryableErrors = VarGetNonRetryable;
	}
	else if (FamilyName == FName("Event"))
	{
		Set(TEXT("K2Node_Event"));
		Contract.OptionalParams.Add({ TEXT("function_name"), TEXT("string"), false, TEXT("Event selector in ClassName.EventName format, e.g. Actor.ReceiveBeginPlay") });
		Contract.OptionalParams.Add({ TEXT("owner_class"), TEXT("string"), false, TEXT("Canonical owner class path, e.g. /Script/Engine.Actor") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD");
	}
	else if (FamilyName == FName("ExecutionSequence"))
	{
		Set(TEXT("K2Node_ExecutionSequence"));
	}
	else if (FamilyName == FName("CustomEvent"))
	{
		Set(TEXT("K2Node_CustomEvent"));
	}
	else if (FamilyName == FName("Self"))
	{
		Set(TEXT("K2Node_Self"));
	}
	else if (FamilyName == FName("Knot"))
	{
		Set(TEXT("K2Node_Knot"));
	}
	else if (FamilyName == FName("MakeArray"))
	{
		Set(TEXT("K2Node_MakeArray"));
	}
	else if (FamilyName == FName("Timeline"))
	{
		Set(TEXT("K2Node_Timeline"));
		Contract.RequiredParams.Add({ TEXT("timeline_name"), TEXT("string"), true, TEXT("Name of the UTimelineTemplate already present on the Blueprint") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, VARIABLE_NOT_FOUND");
	}
	else if (FamilyName == FName("SpawnActorFromClass"))
	{
		Set(TEXT("K2Node_SpawnActorFromClass"));
	}
	else if (FamilyName == FName("DynamicCast"))
	{
		Set(TEXT("K2Node_DynamicCast"));
		Contract.RequiredParams.Add({ TEXT("class"), TEXT("string"), true, TEXT("Cast target class; alias target_class accepted") });
		Contract.OptionalParams.Add({ TEXT("is_pure"), TEXT("bool"), false, TEXT("Pure cast mode (no execution pins); alias bIsPureCast, pure; requires a target class") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, CLASS_NOT_FOUND");
	}
	else if (FamilyName == FName("ConstructObject"))
	{
		Set(TEXT("K2Node_GenericCreateObject"));
		Contract.RequiredParams.Add({ TEXT("class"), TEXT("string"), true, TEXT("Class to construct") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, CLASS_NOT_FOUND");
	}
	else if (FamilyName == FName("MacroInstance"))
	{
		Set(TEXT("K2Node_MacroInstance"));
		Contract.RequiredParams.Add({ TEXT("macro_path"), TEXT("string"), true, TEXT("Asset path to the macro Blueprint graph") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, ASSET_NOT_FOUND");
	}
	else if (FamilyName == FName("SwitchEnum"))
	{
		Set(TEXT("K2Node_SwitchEnum"));
		Contract.OptionalParams.Add({ TEXT("enum_name"), TEXT("string"), false, TEXT("Reflected enum to switch on; pins allocate from its entries") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, CLASS_NOT_FOUND");
	}
	else if (FamilyName == FName("SwitchString"))
	{
		Set(TEXT("K2Node_SwitchString"));
	}
	else if (FamilyName == FName("SwitchInteger"))
	{
		Set(TEXT("K2Node_SwitchInteger"));
	}
	else if (FamilyName == FName("AddDelegate") || FamilyName == FName("RemoveDelegate") || FamilyName == FName("ClearDelegate"))
	{
		Set(FamilyName == FName("AddDelegate") ? TEXT("K2Node_AddDelegate")
			: FamilyName == FName("RemoveDelegate") ? TEXT("K2Node_RemoveDelegate") : TEXT("K2Node_ClearDelegate"));
		Contract.RequiredParams.Add({ TEXT("delegate_name"), TEXT("string"), true, TEXT("Multicast delegate name") });
		Contract.OptionalParams.Add({ TEXT("delegate_class"), TEXT("string"), false, TEXT("Owner class for external delegates; omit for self-context event dispatchers") });
		Contract.OptionalParams.Add({ TEXT("owner_class"), TEXT("string"), false, TEXT("Canonical owner class path") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD, VARIABLE_NOT_FOUND");
	}
	else if (FamilyName == FName("CreateDelegate"))
	{
		Set(TEXT("K2Node_CreateDelegate"));
		Contract.OptionalParams.Add({ TEXT("function_name"), TEXT("string"), false, TEXT("Target function name (bare name)") });
		Contract.NonRetryableErrors = TEXT("INVALID_FIELD");
	}
	else if (FamilyName == FName("Composite"))
	{
		Set(TEXT("K2Node_Composite"));
	}
	else
	{
		Set(NodeClass ? NodeClass->GetName() : NodeClassName);
	}

	return Contract;
}

bool FCortexGraphNodeContract::Validate(
	const FString& NodeClassName,
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeParams,
	FCortexCommandResult& OutError)
{
	const FCortexNodeConstructionContract Contract = Describe(NodeClassName);
	if (!Contract.bSupported)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node class not supported: %s"), *NodeClassName));
		return false;
	}

	auto Fail = [&OutError, &Contract](const FString& Field, const FString& Message) -> bool
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("field"), Field);
		Details->SetStringField(TEXT("message"), Message);
		Details->SetObjectField(TEXT("describe_node"), Contract.ToJson());
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, Details);
		return false;
	};

	auto FailWithError = [&OutError, &Contract](const FString& DefaultField, const FCortexCommandResult& InError) -> bool
	{
		TSharedPtr<FJsonObject> Details = InError.ErrorDetails.IsValid()
			? InError.ErrorDetails
			: MakeShared<FJsonObject>();
		if (!Details->HasField(TEXT("field")) && !DefaultField.IsEmpty())
		{
			Details->SetStringField(TEXT("field"), DefaultField);
		}
		if (!Details->HasField(TEXT("message")))
		{
			Details->SetStringField(TEXT("message"), InError.ErrorMessage);
		}
		Details->SetObjectField(TEXT("describe_node"), Contract.ToJson());
		OutError = FCortexCommandRouter::Error(
			InError.ErrorCode.IsEmpty() ? CortexErrorCodes::InvalidField : InError.ErrorCode,
			InError.ErrorMessage,
			Details);
		return false;
	};

	FName FamilyName;
	UClass* ResolvedClass = nullptr;
	ResolveFamily(NodeClassName, FamilyName, ResolvedClass);

	if (FamilyName == FName("CallFunction"))
	{
		FString CallKind;
		if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("call_kind"), CallKind))
		{
			if (CallKind.Equals(TEXT("parent"), ESearchCase::IgnoreCase))
			{
				return Fail(TEXT("params.call_kind"), TEXT("CallFunction does not support parent calls; use explicit parent call"));
			}
		}

		FCortexResolvedSymbol Symbol;
		if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, OutError))
		{
			return FailWithError(TEXT("params.function_name"), OutError);
		}

		if (!UEdGraphSchema_K2::CanUserKismetCallFunction(Symbol.Function))
		{
			return Fail(TEXT("params.function_name"), FString::Printf(TEXT("Function '%s' on class '%s' is not callable in Blueprints"), *Symbol.MemberName.ToString(), *Symbol.ContextClass->GetName()));
		}
	}
	else if (FamilyName == FName("VariableSet") || FamilyName == FName("VariableGet"))
	{
		const bool bIsWrite = (FamilyName == FName("VariableSet"));
		FCortexResolvedSymbol Symbol;
		if (!FCortexGraphSymbolResolver::ResolveProperty(Blueprint, NodeParams, bIsWrite, Symbol, OutError))
		{
			return FailWithError(TEXT("params.variable_name"), OutError);
		}
	}
	else if (FamilyName == FName("Timeline"))
	{
		FString TimelineName;
		if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("timeline_name"), TimelineName) || TimelineName.IsEmpty())
		{
			return Fail(TEXT("params.timeline_name"), TEXT("Timeline requires params.timeline_name"));
		}
		if (Blueprint && Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName)) == nullptr)
		{
			return Fail(TEXT("params.timeline_name"), FString::Printf(TEXT("Timeline template not found on Blueprint: %s"), *TimelineName));
		}
	}
	else if (FamilyName == FName("MacroInstance"))
	{
		FString MacroPath;
		if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("macro_path"), MacroPath) || MacroPath.IsEmpty())
		{
			return Fail(TEXT("params.macro_path"), TEXT("MacroInstance requires params.macro_path"));
		}
		UEdGraph* MacroGraph = ResolveMacroGraph(Blueprint, MacroPath);
		if (MacroGraph == nullptr)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("field"), TEXT("params.macro_path"));
			Details->SetStringField(TEXT("message"), FString::Printf(TEXT("Macro graph not found: %s"), *MacroPath));
			Details->SetObjectField(TEXT("describe_node"), Contract.ToJson());
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, FString::Printf(TEXT("Macro graph not found: %s"), *MacroPath), Details);
			return false;
		}
	}
	else if (FamilyName == FName("AddDelegate") || FamilyName == FName("RemoveDelegate") || FamilyName == FName("ClearDelegate"))
	{
		FString DelegateName;
		if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("delegate_name"), DelegateName) || DelegateName.IsEmpty())
		{
			return Fail(TEXT("params.delegate_name"), FString::Printf(TEXT("%s requires params.delegate_name"), *NodeClassName));
		}
		FString DelegateClass;
		if (!NodeParams->TryGetStringField(TEXT("delegate_class"), DelegateClass) && !NodeParams->TryGetStringField(TEXT("owner_class"), DelegateClass))
		{
			DelegateClass.Reset();
		}
		if (!DelegateClass.IsEmpty())
		{
			UClass* OwnerClass = nullptr;
			if (!FCortexGraphSymbolResolver::ResolveClass(DelegateClass, OwnerClass, OutError))
			{
				return FailWithError(TEXT("params.delegate_class"), OutError);
			}
			if (OwnerClass == nullptr || CastField<FMulticastDelegateProperty>(OwnerClass->FindPropertyByName(FName(*DelegateName))) == nullptr)
			{
				return Fail(TEXT("params.delegate_name"), FString::Printf(TEXT("Multicast delegate property not found: %s on class %s"), *DelegateName, *DelegateClass));
			}
		}
		else
		{
			UClass* SelfClass = Blueprint ? (Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass) : nullptr;
			if (SelfClass == nullptr || CastField<FMulticastDelegateProperty>(SelfClass->FindPropertyByName(FName(*DelegateName))) == nullptr)
			{
				return Fail(TEXT("params.delegate_name"), FString::Printf(TEXT("Self delegate property not found: %s"), *DelegateName));
			}
		}
	}
	else if (FamilyName == FName("DynamicCast"))
	{
		FString TargetClassIdentifier;
		const bool bHasClass = NodeParams.IsValid() && (
			NodeParams->TryGetStringField(TEXT("class"), TargetClassIdentifier)
			|| NodeParams->TryGetStringField(TEXT("target_class"), TargetClassIdentifier));
		if (bHasClass)
		{
			UClass* TargetClass = nullptr;
			if (!FCortexGraphSymbolResolver::ResolveClass(TargetClassIdentifier, TargetClass, OutError))
			{
				return FailWithError(TEXT("params.class"), OutError);
			}
		}
		// Purity is only meaningful together with a cast target: a purity-only request describes a
		// node whose identity can never be verified, so it is refused instead of being accepted and
		// then failing after mutation.
		const bool bDeclaresPurity = NodeParams.IsValid() && (
			NodeParams->HasField(TEXT("is_pure")) || NodeParams->HasField(TEXT("pure"))
			|| NodeParams->HasField(TEXT("bIsPureCast")));
		if (!bHasClass && bDeclaresPurity)
		{
			return Fail(TEXT("params.class"), TEXT("DynamicCast purity requires a target class: params.class or target_class"));
		}
	}
	else if (FamilyName == FName("ConstructObject"))
	{
		FString ClassIdentifier;
		if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("class"), ClassIdentifier) || ClassIdentifier.IsEmpty())
		{
			return Fail(TEXT("params.class"), TEXT("ConstructObject requires params.class"));
		}
		UClass* ConstructClass = nullptr;
		if (!FCortexGraphSymbolResolver::ResolveClass(ClassIdentifier, ConstructClass, OutError))
		{
			return FailWithError(TEXT("params.class"), OutError);
		}
		if (ConstructClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return Fail(TEXT("params.class"), FString::Printf(TEXT("Class '%s' cannot be constructed (abstract or deprecated)"), *ClassIdentifier));
		}
	}
	else if (FamilyName == FName("SwitchEnum"))
	{
		FString EnumName;
		if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("enum_name"), EnumName) && !EnumName.IsEmpty())
		{
			if (FindFirstObject<UEnum>(*EnumName) == nullptr)
			{
				return Fail(TEXT("params.enum_name"), FString::Printf(TEXT("Enum not found: %s"), *EnumName));
			}
		}
	}
	else if (FamilyName == FName("Event"))
	{
		FString FunctionName;
		if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("function_name"), FunctionName) && !FunctionName.IsEmpty())
		{
			FCortexResolvedSymbol Symbol;
			if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, OutError))
			{
				return FailWithError(TEXT("params.function_name"), OutError);
			}
			if (Symbol.Function && !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Symbol.Function))
			{
				return Fail(TEXT("params.function_name"), FString::Printf(TEXT("Function '%s' cannot be placed as an event"), *Symbol.MemberName.ToString()));
			}
		}
	}

	return true;
}

bool FCortexGraphNodeContract::ApplyNodeConstructionParams(
	UEdGraph* Graph,
	UEdGraphNode* NewNode,
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeParams,
	FString& OutError)
{
	(void)Graph;
	if (!NodeParams.IsValid())
	{
		return true;
	}

	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode))
	{
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult Error;
		if (FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, Error))
		{
			CallNode->SetFromFunction(Symbol.Function);
		}
		else
		{
			OutError = Error.ErrorMessage;
			return false;
		}
	}

	if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(NewNode))
	{
		const bool bIsWrite = VarNode->IsA<UK2Node_VariableSet>();
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult Error;
		if (FCortexGraphSymbolResolver::ResolveProperty(Blueprint, NodeParams, bIsWrite, Symbol, Error))
		{
			if (Symbol.Property)
			{
				VarNode->SetFromProperty(Symbol.Property, false, Symbol.ContextClass);
			}
			else
			{
				VarNode->VariableReference.SetSelfMember(Symbol.MemberName);
			}
		}
		else
		{
			OutError = Error.ErrorMessage;
			return false;
		}
	}

	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(NewNode))
	{
		bool bIsPure = false;
		if (NodeParams->TryGetBoolField(TEXT("is_pure"), bIsPure)
			|| NodeParams->TryGetBoolField(TEXT("pure"), bIsPure)
			|| NodeParams->TryGetBoolField(TEXT("bIsPureCast"), bIsPure))
		{
			CastNode->SetPurity(bIsPure);
		}

		FString TargetClassIdentifier;
		const bool bHasClass =
			NodeParams->TryGetStringField(TEXT("class"), TargetClassIdentifier)
			|| NodeParams->TryGetStringField(TEXT("target_class"), TargetClassIdentifier);
		if (bHasClass)
		{
			UClass* TargetClass = nullptr;
			FCortexCommandResult Error;
			if (!FCortexGraphSymbolResolver::ResolveClass(TargetClassIdentifier, TargetClass, Error))
			{
				OutError = Error.ErrorMessage;
				return false;
			}
			CastNode->TargetType = TargetClass;
			CastNode->ReconstructNode();
		}
	}

	if (UK2Node_GenericCreateObject* CreateNode = Cast<UK2Node_GenericCreateObject>(NewNode))
	{
		FString ClassIdentifier;
		if (NodeParams->TryGetStringField(TEXT("class"), ClassIdentifier))
		{
			UClass* ConstructClass = nullptr;
			FCortexCommandResult Error;
			if (!FCortexGraphSymbolResolver::ResolveClass(ClassIdentifier, ConstructClass, Error))
			{
				OutError = Error.ErrorMessage;
				return false;
			}
			if (CreateNode->Pins.Num() == 0)
			{
				CreateNode->AllocateDefaultPins();
			}
			UEdGraphPin* ClassPin = CreateNode->GetClassPin();
			if (ClassPin)
			{
				ClassPin->DefaultObject = ConstructClass;
				CreateNode->PinDefaultValueChanged(ClassPin);
				CreateNode->ReconstructNode();
			}
		}
	}

	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(NewNode))
	{
		FString FunctionName;
		if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("function_name"), FunctionName) && !FunctionName.IsEmpty())
		{
			FCortexResolvedSymbol Symbol;
			FCortexCommandResult Error;
			if (FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, Error))
			{
				EventNode->EventReference.SetExternalMember(Symbol.MemberName, Symbol.ContextClass);
				EventNode->bOverrideFunction = true;
			}
			else
			{
				OutError = Error.ErrorMessage;
				return false;
			}
		}
	}

	if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(NewNode))
	{
		FString DelegateName;
		if (NodeParams->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			FString DelegateClass;
			if (!NodeParams->TryGetStringField(TEXT("delegate_class"), DelegateClass) && !NodeParams->TryGetStringField(TEXT("owner_class"), DelegateClass))
			{
				DelegateClass.Reset();
			}
			if (!DelegateClass.IsEmpty())
			{
				UClass* OwnerClass = nullptr;
				FCortexCommandResult Error;
				if (FCortexGraphSymbolResolver::ResolveClass(DelegateClass, OwnerClass, Error) && OwnerClass)
				{
					FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(OwnerClass->FindPropertyByName(FName(*DelegateName)));
					if (DelegateProp == nullptr)
					{
						OutError = FString::Printf(TEXT("Multicast delegate property not found: %s on class %s"), *DelegateName, *DelegateClass);
						return false;
					}
					DelegateNode->SetFromProperty(DelegateProp, false, OwnerClass);
				}
				else
				{
					OutError = Error.ErrorMessage;
					return false;
				}
			}
			else
			{
				UClass* SelfClass = Blueprint->SkeletonGeneratedClass
					? Blueprint->SkeletonGeneratedClass
					: Blueprint->GeneratedClass;
				FMulticastDelegateProperty* DelegateProp = SelfClass
					? CastField<FMulticastDelegateProperty>(SelfClass->FindPropertyByName(FName(*DelegateName)))
					: nullptr;
				if (DelegateProp == nullptr)
				{
					OutError = FString::Printf(TEXT("Self delegate property not found: %s"), *DelegateName);
					return false;
				}
				DelegateNode->SetFromProperty(DelegateProp, true, SelfClass);
			}
		}
	}

	if (UK2Node_CreateDelegate* CreateDelegateNode = Cast<UK2Node_CreateDelegate>(NewNode))
	{
		FString FunctionName;
		if (NodeParams->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			CreateDelegateNode->SetFunction(FName(*FunctionName));
		}
	}

	if (UK2Node_SwitchEnum* SwitchEnumNode = Cast<UK2Node_SwitchEnum>(NewNode))
	{
		FString EnumName;
		if (NodeParams->TryGetStringField(TEXT("enum_name"), EnumName) && !EnumName.IsEmpty())
		{
			UEnum* Enum = FindFirstObject<UEnum>(*EnumName);
			if (Enum == nullptr)
			{
				OutError = FString::Printf(TEXT("Enum not found: %s"), *EnumName);
				return false;
			}
			SwitchEnumNode->SetEnum(Enum);
		}
	}

	if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(NewNode))
	{
		FString TimelineName;
		if (NodeParams->TryGetStringField(TEXT("timeline_name"), TimelineName))
		{
			TimelineNode->TimelineName = FName(*TimelineName);
			if (Blueprint)
			{
				if (UTimelineTemplate* Template = Blueprint->FindTimelineTemplateByVariableName(TimelineNode->TimelineName))
				{
					TimelineNode->TimelineGuid = Template->TimelineGuid;
				}
			}
		}
	}

	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NewNode))
	{
		FString MacroPath;
		if (NodeParams->TryGetStringField(TEXT("macro_path"), MacroPath))
		{
			UEdGraph* MacroGraph = ResolveMacroGraph(Blueprint, MacroPath);
			if (MacroGraph)
			{
				MacroNode->SetMacroGraph(MacroGraph);
			}
		}
	}

	return true;
}

UEdGraph* FCortexGraphNodeContract::ResolveMacroGraph(
	UBlueprint* Blueprint,
	const FString& MacroPath)
{
	if (MacroPath.IsEmpty())
	{
		return nullptr;
	}

	if (Blueprint != nullptr)
	{
		for (UEdGraph* G : Blueprint->MacroGraphs)
		{
			if (G && (G->GetName() == MacroPath || G->GetPathName() == MacroPath))
			{
				return G;
			}
		}
	}

	const FString LongPackageName = FPackageName::ObjectPathToPackageName(MacroPath);
	if (!LongPackageName.IsEmpty() && (FindPackage(nullptr, *LongPackageName) != nullptr || FPackageName::DoesPackageExist(LongPackageName)))
	{
		return LoadObject<UEdGraph>(nullptr, *MacroPath);
	}

	return nullptr;
}

