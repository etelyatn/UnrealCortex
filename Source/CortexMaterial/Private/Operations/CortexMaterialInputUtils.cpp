#include "Operations/CortexMaterialInputUtils.h"

#include "CortexBatchScope.h"
#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "ScopedTransaction.h"
#include "Misc/TVariant.h"

namespace
{
bool InlineAllowed(EMaterialProperty Property)
{
	return Property != MP_WorldPositionOffset && Property != MP_Anisotropy
		&& Property != MP_PixelDepthOffset && Property != MP_Displacement;
}

// Dispatch on the actual typed editor-data member, never downcast a base input.
template <typename Visitor>
bool VisitConstant(UMaterial* Material, EMaterialProperty Property, Visitor&& Visit)
{
	auto* Data = Material->GetEditorOnlyData();
	if (!Data)
	{
		return false;
	}
	switch (Property)
	{
	case MP_BaseColor: Visit(Data->BaseColor); break;
	case MP_Metallic: Visit(Data->Metallic); break;
	case MP_Specular: Visit(Data->Specular); break;
	case MP_Roughness: Visit(Data->Roughness); break;
	case MP_Anisotropy: Visit(Data->Anisotropy); break;
	case MP_Normal: Visit(Data->Normal); break;
	case MP_Tangent: Visit(Data->Tangent); break;
	case MP_EmissiveColor: Visit(Data->EmissiveColor); break;
	case MP_Opacity: Visit(Data->Opacity); break;
	case MP_OpacityMask: Visit(Data->OpacityMask); break;
	case MP_WorldPositionOffset: Visit(Data->WorldPositionOffset); break;
	case MP_Displacement: Visit(Data->Displacement); break;
	case MP_SubsurfaceColor: Visit(Data->SubsurfaceColor); break;
	case MP_CustomData0: Visit(Data->ClearCoat); break;
	case MP_CustomData1: Visit(Data->ClearCoatRoughness); break;
	case MP_AmbientOcclusion: Visit(Data->AmbientOcclusion); break;
	case MP_Refraction: Visit(Data->Refraction); break;
	case MP_PixelDepthOffset: Visit(Data->PixelDepthOffset); break;
	case MP_SurfaceThickness: Visit(Data->SurfaceThickness); break;
	default:
		if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
		{
			Visit(Data->CustomizedUVs[Property - MP_CustomizedUVs0]);
		}
		else
		{
			return false;
		}
		break;
	}
	return true;
}

TSharedPtr<FJsonValue> ConstantJson(float Value)
{
	return MakeShared<FJsonValueNumber>(Value);
}
TSharedPtr<FJsonValue> ConstantJson(const FLinearColor& Value)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("r"), Value.R);
	Result->SetNumberField(TEXT("g"), Value.G);
	Result->SetNumberField(TEXT("b"), Value.B);
	Result->SetNumberField(TEXT("a"), Value.A);
	return MakeShared<FJsonValueObject>(Result);
}
TSharedPtr<FJsonValue> ConstantJson(const FVector3f& Value)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Value.X);
	Result->SetNumberField(TEXT("y"), Value.Y);
	Result->SetNumberField(TEXT("z"), Value.Z);
	return MakeShared<FJsonValueObject>(Result);
}
TSharedPtr<FJsonValue> ConstantJson(const FVector2f& Value)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Value.X);
	Result->SetNumberField(TEXT("y"), Value.Y);
	return MakeShared<FJsonValueObject>(Result);
}

bool ReadFloat(const TSharedPtr<FJsonValue>& Value, float& Out)
{
	double Number;
	if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
		|| !FMath::IsFinite(Number) || FMath::Abs(Number) > MAX_flt)
	{
		return false;
	}
	Out = static_cast<float>(Number);
	return true;
}
bool ParseConstant(const TSharedPtr<FJsonValue>& Value, float& Out)
{
	return ReadFloat(Value, Out);
}
bool ReadComponents(const TSharedPtr<FJsonValue>& Value, TConstArrayView<const TCHAR*> Names, TArrayView<float> Out)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid()
		|| (*Object)->Values.Num() != Names.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		if (!ReadFloat((*Object)->TryGetField(Names[Index]), Out[Index]))
		{
			return false;
		}
	}
	return true;
}
bool ParseConstant(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
{
	const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
	float Values[4];
	if (!ReadComponents(Value, Names, Values))
	{
		return false;
	}
	Out = FLinearColor(Values[0], Values[1], Values[2], Values[3]);
	return true;
}
bool ParseConstant(const TSharedPtr<FJsonValue>& Value, FVector3f& Out)
{
	const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("z")};
	float Values[3];
	if (!ReadComponents(Value, Names, Values))
	{
		return false;
	}
	Out = FVector3f(Values[0], Values[1], Values[2]);
	return true;
}
bool ParseConstant(const TSharedPtr<FJsonValue>& Value, FVector2f& Out)
{
	const TCHAR* Names[] = {TEXT("x"), TEXT("y")};
	float Values[2];
	if (!ReadComponents(Value, Names, Values))
	{
		return false;
	}
	Out = FVector2f(Values[0], Values[1]);
	return true;
}

FString Normalize(const FString& Name)
{
	return Name.Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT("")).ToLower();
}

void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const TCHAR* Code, const FString& Message, const FString& Input = FString())
{
	auto Warning = MakeShared<FJsonObject>();
	Warning->SetStringField(TEXT("code"), Code);
	Warning->SetStringField(TEXT("message"), Message);
	if (!Input.IsEmpty())
	{
		Warning->SetStringField(TEXT("input"), Input);
	}
	Warnings.Add(MakeShared<FJsonValueObject>(Warning));
}
}

namespace CortexMaterialInputUtils
{
FString InputName(EMaterialProperty Property)
{
	FString Name = StaticEnum<EMaterialProperty>()->GetNameStringByValue(Property);
	Name.RemoveFromStart(TEXT("MP_"));
	return Name;
}

bool ResolveInput(UMaterial* Material, const FString& Name, EMaterialProperty& OutProperty, FExpressionInput*& OutInput)
{
	if (!Material || !Material->GetEditorOnlyData() || Name.IsEmpty())
	{
		return false;
	}
	const FString Normalized = Normalize(Name);
	for (int32 Index = 0; Index < MP_MAX; ++Index)
	{
		const auto Property = static_cast<EMaterialProperty>(Index);
		const FString Candidate = Normalize(InputName(Property));
		if (Normalized == Candidate || Normalized == Candidate.Replace(TEXT("customizeduvs"), TEXT("customizeduv"))
			|| (Property == MP_CustomData0 && Normalized == TEXT("clearcoat"))
			|| (Property == MP_CustomData1 && Normalized == TEXT("clearcoatroughness")))
		{
			OutProperty = Property;
			OutInput = Material->GetExpressionInputForProperty(Property);
			return OutInput != nullptr;
		}
	}
	return false;
}

TSharedRef<FJsonObject> SerializeInput(UMaterial* Material, EMaterialProperty Property)
{
	FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("input"), InputName(Property));
	Result->SetBoolField(TEXT("connected"), Input->Expression != nullptr);
	if (Input->Expression)
	{
		Result->SetStringField(TEXT("source_node"), Input->Expression->GetName());
	}
	else
	{
		Result->SetField(TEXT("source_node"), MakeShared<FJsonValueNull>());
	}
	Result->SetNumberField(TEXT("output_index"), Input->OutputIndex);
	Result->SetNumberField(TEXT("mask"), Input->Mask);
	Result->SetNumberField(TEXT("mask_r"), Input->MaskR);
	Result->SetNumberField(TEXT("mask_g"), Input->MaskG);
	Result->SetNumberField(TEXT("mask_b"), Input->MaskB);
	Result->SetNumberField(TEXT("mask_a"), Input->MaskA);
	const bool bSupports = VisitConstant(Material, Property, [&](const auto& Typed)
	{
		Result->SetBoolField(TEXT("use_constant"), !!Typed.UseConstant);
		Result->SetField(TEXT("constant"), ConstantJson(Typed.Constant));
	});
	Result->SetBoolField(TEXT("supports_constant"), bSupports);
	Result->SetBoolField(TEXT("inline_constant_allowed"), bSupports && InlineAllowed(Property));
	if (!bSupports)
	{
		Result->SetField(TEXT("use_constant"), MakeShared<FJsonValueNull>());
		Result->SetField(TEXT("constant"), MakeShared<FJsonValueNull>());
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> SerializeInputs(UMaterial* Material)
{
	TArray<TSharedPtr<FJsonValue>> Inputs;
	if (Material->GetEditorOnlyData())
	{
		for (int32 Index = 0; Index < MP_MAX; ++Index)
		{
			const auto Property = static_cast<EMaterialProperty>(Index);
			if (Material->GetExpressionInputForProperty(Property))
			{
				Inputs.Add(MakeShared<FJsonValueObject>(SerializeInput(Material, Property)));
			}
		}
	}
	return Inputs;
}

FString BlendModeName(EBlendMode Mode)
{
	FString Name = StaticEnum<EBlendMode>()->GetNameStringByValue(Mode);
	Name.RemoveFromStart(TEXT("BLEND_"));
	return Name;
}

TArray<TSharedPtr<FJsonValue>> CollectWarnings(UMaterial* Material)
{
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (Material->BlendMode != Material->GetBlendMode())
	{
		AddWarning(Warnings, TEXT("BLEND_MODE_MISMATCH"), FString::Printf(
			TEXT("Authored blend mode %s differs from effective blend mode %s; inspect can_masked_be_assumed_opaque."),
			*BlendModeName(Material->BlendMode), *BlendModeName(Material->GetBlendMode())));
	}
	if (Material->GetEditorOnlyData() && !Material->IsUsingNewHLSLGenerator())
	{
		for (int32 Index = 0; Index < MP_MAX; ++Index)
		{
			const auto Property = static_cast<EMaterialProperty>(Index);
			if (InlineAllowed(Property))
			{
				VisitConstant(Material, Property, [&](const auto& Typed)
				{
					if (Typed.Expression && Typed.UseConstant)
					{
						// The legacy translator bypasses the connection whenever use_constant is selected, but the
						// stored constant is not always what gets compiled (world-space normals use the vertex
						// normal), so the message must not claim the inline constant is used.
						AddWarning(Warnings, TEXT("CONNECTED_EXPRESSION_IGNORED"),
							TEXT("The legacy material translator bypasses this connected expression while use_constant is selected; clear use_constant to compile the connection."), InputName(Property));
					}
				});
			}
		}
	}
	return Warnings;
}

void UpdateUseConstant(UMaterial* Material, EMaterialProperty Property, bool bDisconnected)
{
	VisitConstant(Material, Property, [&](auto& Typed)
	{
		Typed.UseConstant = bDisconnected && InlineAllowed(Property);
	});
}

void BeginEdit(UMaterial* Material)
{
	Material->Modify();
	if (auto* Data = Material->GetEditorOnlyData())
	{
		Data->SetFlags(RF_Transactional);
		Data->Modify();
	}
	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PreEditChange(nullptr);
	}
}

void EndEdit(UMaterial* Material)
{
	if (FCortexCommandRouter::IsInBatch())
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	else
	{
		Material->PostEditChange();
	}
	Material->MarkPackageDirty();
}

FCortexCommandResult ApplyPatch(UMaterial* Material, EMaterialProperty Property, const TSharedPtr<FJsonObject>& Params)
{
	FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
	auto Invalid = [](const FString& Reason)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Reason);
	};
	const TCHAR* PatchFields[] = {TEXT("use_constant"), TEXT("constant"), TEXT("output_index"), TEXT("mask"), TEXT("mask_r"), TEXT("mask_g"), TEXT("mask_b"), TEXT("mask_a")};
	int32 PatchCount = 0;
	for (const auto& Entry : Params->Values)
	{
		const FString Key = CortexEngineCompat::JsonKeyToString(Entry.Key);
		if (Key == TEXT("asset_path") || Key == TEXT("input") || Key.StartsWith(TEXT("_")))
		{
			continue;
		}
		bool bKnown = false;
		for (const TCHAR* Field : PatchFields)
		{
			bKnown |= Key == Field;
		}
		if (!bKnown)
		{
			return Invalid(FString::Printf(TEXT("Unknown material input field: %s"), *Key));
		}
		++PatchCount;
	}
	if (!Input || PatchCount == 0)
	{
		return Invalid(TEXT("An available material input and at least one patch field are required"));
	}
	bool bUseConstant = false;
	const bool bSetUse = Params->HasField(TEXT("use_constant"));
	if (bSetUse && (Params->TryGetField(TEXT("use_constant"))->Type != EJson::Boolean
		|| !Params->TryGetBoolField(TEXT("use_constant"), bUseConstant)))
	{
		return Invalid(TEXT("use_constant must be a boolean"));
	}
	const auto Constant = Params->TryGetField(TEXT("constant"));
	bool bConstantValid = !Constant.IsValid();
	TVariant<float, FLinearColor, FVector3f, FVector2f> StagedConstant;
	const bool bSupports = VisitConstant(Material, Property, [&](const auto& Typed)
	{
		if (Constant.IsValid())
		{
			using FConstantType = std::decay_t<decltype(Typed.Constant)>;
			FConstantType Staged;
			bConstantValid = ParseConstant(Constant, Staged);
			if (bConstantValid)
			{
				StagedConstant.Set<FConstantType>(Staged);
			}
		}
	});
	if (((bSetUse || Constant.IsValid()) && !bSupports) || !bConstantValid || (bUseConstant && !InlineAllowed(Property)))
	{
		return Invalid(TEXT("Constant type, finite value, or inline-constant policy is invalid for this input"));
	}
	FExpressionInput Staged = *Input;
	if (Params->HasField(TEXT("output_index")))
	{
		double Index = 0;
		if (Params->TryGetField(TEXT("output_index"))->Type != EJson::Number
			|| !Params->TryGetNumberField(TEXT("output_index"), Index) || !FMath::IsFinite(Index)
			|| Index < 0 || Index > MAX_int32 || Index != FMath::FloorToDouble(Index)
			|| !Input->Expression || !Input->Expression->GetOutputs().IsValidIndex(static_cast<int32>(Index)))
		{
			return Invalid(TEXT("output_index must select an existing output of the connected expression"));
		}
		Staged.Connect(static_cast<int32>(Index), Input->Expression);
	}
	int32* Masks[] = {&Staged.Mask, &Staged.MaskR, &Staged.MaskG, &Staged.MaskB, &Staged.MaskA};
	const TCHAR* MaskNames[] = {TEXT("mask"), TEXT("mask_r"), TEXT("mask_g"), TEXT("mask_b"), TEXT("mask_a")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Masks); ++Index)
	{
		if (Params->HasField(MaskNames[Index]))
		{
			double Number = 0;
			if (Params->TryGetField(MaskNames[Index])->Type != EJson::Number
				|| !Params->TryGetNumberField(MaskNames[Index], Number) || (Number != 0 && Number != 1))
			{
				return Invalid(FString::Printf(TEXT("%s must be integer 0 or 1"), MaskNames[Index]));
			}
			*Masks[Index] = static_cast<int32>(Number);
		}
	}
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Material Input")), !FCortexCommandRouter::IsInBatch());
	BeginEdit(Material);
	*Input = Staged;
	VisitConstant(Material, Property, [&](auto& Typed)
	{
		if (bSetUse)
		{
			Typed.UseConstant = bUseConstant;
		}
		if (Constant.IsValid())
		{
			Typed.Constant = StagedConstant.Get<std::decay_t<decltype(Typed.Constant)>>();
		}
	});
	EndEdit(Material);
	// Echo the caller's requested path like every other material mutation; only fall back to the loaded
	// object path when the request carried no asset_path.
	FString RequestedPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), RequestedPath) || RequestedPath.IsEmpty())
	{
		RequestedPath = Material->GetPathName();
	}
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), RequestedPath);
	Result->SetBoolField(TEXT("updated"), true);
	Result->SetObjectField(TEXT("input_state"), SerializeInput(Material, Property));
	Result->SetArrayField(TEXT("warnings"), CollectWarnings(Material));
	return FCortexCommandRouter::Success(Result);
}
}
