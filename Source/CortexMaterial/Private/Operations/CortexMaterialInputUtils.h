#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "MaterialShared.h"

class UMaterial;
class UMaterialExpression;
struct FExpressionInput;
class FJsonObject;
class FJsonValue;

namespace CortexMaterialInputUtils
{
	FString InputName(EMaterialProperty Property);
	bool ResolveInput(UMaterial* Material, const FString& Name, EMaterialProperty& OutProperty, FExpressionInput*& OutInput);
	TSharedRef<FJsonObject> SerializeInput(UMaterial* Material, EMaterialProperty Property);
	TArray<TSharedPtr<FJsonValue>> SerializeInputs(UMaterial* Material);
	TArray<TSharedPtr<FJsonValue>> CollectWarnings(UMaterial* Material);
	void UpdateUseConstant(UMaterial* Material, EMaterialProperty Property, bool bDisconnected);
	FCortexCommandResult ApplyPatch(UMaterial* Material, EMaterialProperty Property, const TSharedPtr<FJsonObject>& Params);
	void BeginEdit(UMaterial* Material);
	void EndEdit(UMaterial* Material);
	/** Records an expression in the open transaction. Expressions created by earlier Cortex versions lack RF_Transactional. */
	void ModifyExpression(UMaterialExpression* Expression);
	FString BlendModeName(EBlendMode Mode);
}
