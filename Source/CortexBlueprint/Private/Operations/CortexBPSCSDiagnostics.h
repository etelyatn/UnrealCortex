#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UActorComponent;
class UClass;
class FProperty;
class USCS_Node;

class FCortexBPSCSDiagnostics
{
public:
	struct FDirtyReport
	{
		TArray<FString> AcknowledgmentKeys;
		FString Explanation;
		TArray<FString> SubObjectKeys;
		TArray<FString> TopLevelKeys;

		struct FDirtyDetail
		{
			FString Key;
			FString Kind;
			FString ComponentClass;
		};
		TArray<FDirtyDetail> DirtyDetails;

		bool HasLoss() const { return !AcknowledgmentKeys.IsEmpty(); }
		bool HasSubObjectLoss() const { return !SubObjectKeys.IsEmpty(); }

		TSharedPtr<FJsonObject> ToRefusalJson() const;
		TSharedPtr<FJsonObject> ToDiffJson() const;
	};

	static FDirtyReport DetectSCSNodeDirtyState(USCS_Node* Node, UBlueprint* BP);

	enum class ECollisionSeverity
	{
		Blocking,
		Adoptable
	};

	enum class ECollisionInheritedKind
	{
		UProperty,
		SCS,
		Delegate,
		Interface,
		SparseClassData
	};

	struct FCollision
	{
		FName SCSNodeName;
		UClass* SCSComponentClass = nullptr;
		FProperty* InheritedProperty = nullptr;
		UClass* InheritedSCSClass = nullptr;
		UClass* InheritedFromClass = nullptr;
		ECollisionInheritedKind Kind = ECollisionInheritedKind::UProperty;
		ECollisionSeverity Severity = ECollisionSeverity::Blocking;
	};

	static TArray<FCollision> DetectSCSInheritedCollisions(UBlueprint* BP);

	struct FResolveResult
	{
		UActorComponent* Component = nullptr;
		bool bIsAmbiguous = false;
		TArray<FString> AmbiguousCandidates;
		FString FailureReason;
	};

	static FResolveResult ResolveComponentTemplateByName(UBlueprint* BP, const FString& Name);
};
