#include "Operations/CortexMaterialGraphTarget.h"

#include "CortexBatchScope.h"
#include "Operations/CortexMaterialInputUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "MaterialEditingLibrary.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "TimerManager.h"

namespace
{
FCortexCommandResult GraphAssetNotFound(const FString& AssetPath)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::MaterialNotFound,
		FString::Printf(TEXT("Material graph asset not found: %s"), *AssetPath));
}

// RefreshGraphFromOriginal no-ops while a transaction is open, and every graph command wraps its
// edit in one. Queue open function editors for the next tick, deduplicated per function, so a
// script-driven edit replaces the editor's stale duplicate instead of being overwritten on Apply.
TSet<TWeakObjectPtr<UMaterialFunction>> GPendingFunctionEditorRefreshes;
bool GFunctionEditorRefreshScheduled = false;

void FlushFunctionEditorRefreshes()
{
	GFunctionEditorRefreshScheduled = false;
	TSet<TWeakObjectPtr<UMaterialFunction>> Pending = MoveTemp(GPendingFunctionEditorRefreshes);
	GPendingFunctionEditorRefreshes.Reset();
	for (const TWeakObjectPtr<UMaterialFunction>& WeakFunction : Pending)
	{
		if (UMaterialFunction* Function = WeakFunction.Get())
		{
			UMaterialEditingLibrary::RefreshMaterialFunctionEditor(Function);
		}
	}
}

void ScheduleFunctionEditorRefresh(UMaterialFunction* Function)
{
	if (Function == nullptr || GEditor == nullptr)
	{
		return;
	}
	GPendingFunctionEditorRefreshes.Add(Function);
	if (GFunctionEditorRefreshScheduled)
	{
		return;
	}
	GFunctionEditorRefreshScheduled = true;
	GEditor->GetTimerManager()->SetTimerForNextTick([]()
	{
		FlushFunctionEditorRefreshes();
	});
}
}

bool FCortexMaterialGraphTarget::Load(const FString& AssetPath, FCortexMaterialGraphTarget& OutTarget,
	FCortexCommandResult& OutError)
{
	OutTarget = FCortexMaterialGraphTarget();

	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = GraphAssetNotFound(AssetPath);
		return false;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (Asset == nullptr)
	{
		OutError = GraphAssetNotFound(AssetPath);
		return false;
	}

	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		OutTarget.Kind = ECortexMaterialGraphTargetKind::Material;
		OutTarget.Material = Material;
		return true;
	}

	if (UMaterialFunctionInstance* Instance = Cast<UMaterialFunctionInstance>(Asset))
	{
		UMaterialFunction* BaseFunction = Instance->GetBaseFunction();
		if (BaseFunction == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::MaterialNotFound,
				FString::Printf(TEXT("Material function instance has no base function: %s"), *AssetPath));
			return false;
		}
		OutTarget.Kind = ECortexMaterialGraphTargetKind::FunctionInstance;
		OutTarget.Function = Instance;
		OutTarget.BaseFunction = BaseFunction;
		return true;
	}

	if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
	{
		OutTarget.Kind = ECortexMaterialGraphTargetKind::Function;
		OutTarget.Function = Function;
		OutTarget.BaseFunction = Function;
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::MaterialNotFound,
		FString::Printf(
			TEXT("Not a material graph asset (expected material, material function, or material function instance): %s"),
			*AssetPath));
	return false;
}

const TCHAR* FCortexMaterialGraphTarget::GetKindName() const
{
	switch (Kind)
	{
	case ECortexMaterialGraphTargetKind::Material:
		return TEXT("material");
	case ECortexMaterialGraphTargetKind::Function:
		return TEXT("material_function");
	case ECortexMaterialGraphTargetKind::FunctionInstance:
		return TEXT("material_function_instance");
	}
	return TEXT("material");
}

UObject* FCortexMaterialGraphTarget::GetAsset() const
{
	return IsMaterial() ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
}

FString FCortexMaterialGraphTarget::GetBaseFunctionPath() const
{
	return BaseFunction ? BaseFunction->GetPathName() : FString();
}

TConstArrayView<TObjectPtr<UMaterialExpression>> FCortexMaterialGraphTarget::GetExpressions() const
{
	if (IsMaterial())
	{
		if (Material && Material->GetEditorOnlyData())
		{
			return Material->GetEditorOnlyData()->ExpressionCollection.Expressions;
		}
		return {};
	}
	if (Function)
	{
		return Function->GetExpressions();
	}
	return {};
}

UMaterialExpression* FCortexMaterialGraphTarget::FindExpression(const FString& NodeId) const
{
	for (UMaterialExpression* Expression : GetExpressions())
	{
		if (Expression && Expression->GetName() == NodeId)
		{
			return Expression;
		}
	}
	return nullptr;
}

void FCortexMaterialGraphTarget::BeginEdit() const
{
	if (IsMaterial())
	{
		if (Material)
		{
			CortexMaterialInputUtils::BeginEdit(Material);
		}
		return;
	}

	if (Function == nullptr)
	{
		return;
	}
	Function->Modify();
	if (auto* EditorData = Function->GetEditorOnlyData())
	{
		EditorData->SetFlags(RF_Transactional);
		EditorData->Modify();
	}
	if (!FCortexCommandRouter::IsInBatch())
	{
		Function->PreEditChange(nullptr);
	}
}

void FCortexMaterialGraphTarget::EndEdit(bool bRecompile) const
{
	if (IsMaterial())
	{
		if (Material)
		{
			CortexMaterialInputUtils::EndEdit(Material);
		}
		return;
	}

	if (Function == nullptr)
	{
		return;
	}

	if (!bRecompile)
	{
		Function->PostEditChange();
		Function->MarkPackageDirty();
		ScheduleFunctionEditorRefresh(BaseFunction);
		return;
	}

	if (FCortexCommandRouter::IsInBatch())
	{
		TWeakObjectPtr<UMaterialFunctionInterface> WeakFunction(Function);
		const FString CleanupKey = FString::Printf(TEXT("material_function_update:%s"), *Function->GetPathName());
		FCortexBatchScope::AddCleanupAction(CleanupKey, [WeakFunction]()
		{
			if (UMaterialFunctionInterface* Pending = WeakFunction.Get())
			{
				UMaterialEditingLibrary::UpdateMaterialFunction(Pending, nullptr);
			}
		});
		ScheduleFunctionEditorRefresh(BaseFunction);
		return;
	}

	UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	ScheduleFunctionEditorRefresh(BaseFunction);
}
