#pragma once

#include "CoreMinimal.h"

#include "AnimGraphNode_Base.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAssetFingerprint.h"
#include "Engine/MemberReference.h"

/** Shared helpers of the anim.rebind_function tests (one header: the test files are merged into one unity translation unit). */
namespace CortexAnimTest
{
	inline TSharedPtr<FJsonObject> RebindParams(const UAnimBlueprint* AnimBlueprint, const TCHAR* From, const TCHAR* To, const bool bDryRun, const bool bCompile = true, const bool bSave = false)
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetStringField(TEXT("from"), From);
		Params->SetStringField(TEXT("to"), To);
		Params->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(AnimBlueprint).ToJson());
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("compile"), bCompile);
		Params->SetBoolField(TEXT("save"), bSave);
		return Params;
	}

	inline FCortexCommandResult Rebind(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint, const TCHAR* From, const TCHAR* To, const bool bDryRun, const bool bCompile = true, const bool bSave = false)
	{
		return Router.Execute(TEXT("anim.rebind_function"), RebindParams(AnimBlueprint, From, To, bDryRun, bCompile, bSave));
	}

	inline TArray<FString> JsonStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text))
				{
					Values.Add(Text);
				}
			}
		}
		return Values;
	}

	/** The binding path anim.get_animbp_info shows for one node's pin property; {"<missing>"} when there is none. */
	inline TArray<FString> ReportedBindingPath(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint, const FGuid& NodeGuid, const TCHAR* PropertyName)
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("bindings")) });
		const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
		for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Info.Data, TEXT("bindings")))
		{
			FString NodeGuidText;
			FString Property;
			if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, NodeGuid)
				&& Entry->TryGetStringField(TEXT("property_name"), Property) && Property == PropertyName)
			{
				return JsonStringArray(Entry, TEXT("path"));
			}
		}
		return { TEXT("<missing>") };
	}

	/** The function a node's slot (initial_update | become_relevant | update) names in the report, with its declared_in. */
	inline bool ReportedSlot(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint, const FGuid& NodeGuid, const TCHAR* Slot, FString& OutFunction, FString& OutDeclaredIn)
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("node_functions")) });
		const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
		for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Info.Data, TEXT("node_functions")))
		{
			FString NodeGuidText;
			if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, NodeGuid) && Entry->GetStringField(TEXT("slot")) == Slot)
			{
				OutFunction = Entry->GetStringField(TEXT("function_name"));
				OutDeclaredIn = Entry->GetStringField(TEXT("declared_in"));
				return true;
			}
		}
		return false;
	}

	/** Counts the compiles of one Blueprint (UBlueprint::OnCompiled fires once per compile). */
	struct FCompileCounter
	{
		explicit FCompileCounter(UBlueprint* InBlueprint)
			: Blueprint(InBlueprint)
		{
			Handle = Blueprint->OnCompiled().AddLambda([this](UBlueprint*) { ++Count; });
		}
		~FCompileCounter() { Blueprint->OnCompiled().Remove(Handle); }
		FCompileCounter(const FCompileCounter&) = delete;
		FCompileCounter& operator=(const FCompileCounter&) = delete;

		UBlueprint* Blueprint = nullptr;
		FDelegateHandle Handle;
		int32 Count = 0;
	};
}
