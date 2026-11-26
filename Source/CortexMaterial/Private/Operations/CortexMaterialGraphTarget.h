#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Materials/MaterialExpression.h"

class UMaterial;
class UMaterialFunction;
class UMaterialFunctionInterface;

/** Which asset kind a material graph command addresses. */
enum class ECortexMaterialGraphTargetKind : uint8
{
	Material,
	Function,
	FunctionInstance
};

/**
 * One graph asset resolved from an MCP asset path: a UMaterial, a UMaterialFunction, or a
 * read-only UMaterialFunctionInstance (whose graph is its base function's).
 */
class FCortexMaterialGraphTarget
{
public:
	static bool Load(const FString& AssetPath, FCortexMaterialGraphTarget& OutTarget,
		FCortexCommandResult& OutError);

	ECortexMaterialGraphTargetKind GetKind() const { return Kind; }
	const TCHAR* GetKindName() const;
	UObject* GetAsset() const;
	UMaterial* GetMaterial() const { return Material; }
	UMaterialFunctionInterface* GetFunction() const { return Function; }
	UMaterialFunction* GetBaseFunction() const { return BaseFunction; }
	FString GetBaseFunctionPath() const;

	bool IsMaterial() const { return Kind == ECortexMaterialGraphTargetKind::Material; }
	bool IsReadOnly() const { return Kind == ECortexMaterialGraphTargetKind::FunctionInstance; }

	TConstArrayView<TObjectPtr<UMaterialExpression>> GetExpressions() const;
	UMaterialExpression* FindExpression(const FString& NodeId) const;

	/** Opens a transaction-recordable edit; materials use the established material input helpers. */
	void BeginEdit() const;
	/** Commits the edit; a function recompiles dependents now or once at the end of a batch. */
	void EndEdit(bool bRecompile = true) const;

private:
	ECortexMaterialGraphTargetKind Kind = ECortexMaterialGraphTargetKind::Material;
	UMaterial* Material = nullptr;
	UMaterialFunctionInterface* Function = nullptr;
	UMaterialFunction* BaseFunction = nullptr;
};
