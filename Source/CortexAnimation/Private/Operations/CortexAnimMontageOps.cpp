#include "Operations/CortexAnimMontageOps.h"

#include "Animation/AnimMontage.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimMutationUtils.h"
#include "ScopedTransaction.h"

namespace
{
struct FSectionSelector
{
	int32 Index = INDEX_NONE;
	FString Name;
	double StartTime = 0.0;
};

struct FPlannedSection
{
	FName Name;
	double StartTime = 0.0;
	FName NextSectionName;
};

FString SectionNameToString(const FName& Name)
{
	return Name.IsNone() ? FString() : Name.ToString();
}

TSharedPtr<FJsonObject> SectionToJson(const FCompositeSection& Section, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("name"), Section.SectionName.ToString());
	Data->SetNumberField(TEXT("start_time"), Section.GetTime());
	Data->SetStringField(TEXT("next_section"), SectionNameToString(Section.NextSectionName));
	return Data;
}

TSharedPtr<FJsonObject> PlannedSectionToJson(const FPlannedSection& Section, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("name"), Section.Name.ToString());
	Data->SetNumberField(TEXT("start_time"), Section.StartTime);
	Data->SetStringField(TEXT("next_section"), SectionNameToString(Section.NextSectionName));
	return Data;
}

TSharedPtr<FJsonObject> MissingSectionJson()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	return Data;
}

TSharedPtr<FJsonObject> SelectorToJson(const FSectionSelector& Selector)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("index"), Selector.Index);
	Data->SetStringField(TEXT("name"), Selector.Name);
	Data->SetNumberField(TEXT("start_time"), Selector.StartTime);
	return Data;
}

TSharedPtr<FJsonObject> SelectorDetails(const FString& AssetPath)
{
	return FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), AssetPath);
}

bool TryReadSectionSelector(
	const TSharedPtr<FJsonObject>& Params,
	FSectionSelector& OutSelector,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* SelectorObject = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("selector"), SelectorObject) || SelectorObject == nullptr || !SelectorObject->IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: selector (object with index, name, start_time)"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector")));
		return false;
	}

	double RawIndex = 0.0;
	if (!(*SelectorObject)->TryGetNumberField(TEXT("index"), RawIndex)
		|| !FMath::IsFinite(RawIndex)
		|| RawIndex < 0.0
		|| RawIndex > static_cast<double>(TNumericLimits<int32>::Max())
		|| FMath::FloorToDouble(RawIndex) != RawIndex)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.index must be a non-negative integer"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.index")));
		return false;
	}

	if (!(*SelectorObject)->TryGetStringField(TEXT("name"), OutSelector.Name) || OutSelector.Name.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.name must be a non-empty string"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.name")));
		return false;
	}

	if (!(*SelectorObject)->TryGetNumberField(TEXT("start_time"), OutSelector.StartTime)
		|| !FMath::IsFinite(OutSelector.StartTime))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.start_time must be a finite number"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector.start_time")));
		return false;
	}

	OutSelector.Index = static_cast<int32>(RawIndex);
	return true;
}

bool TryReadOptionalString(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	FString& OutValue,
	bool& bOutPresent,
	FCortexCommandResult& OutError)
{
	bOutPresent = Params.IsValid() && Params->HasField(FieldName);
	if (!bOutPresent)
	{
		OutValue.Empty();
		return true;
	}

	if (!Params->TryGetStringField(FieldName, OutValue))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a string when provided"), FieldName),
			FCortexAnimMutationUtils::MakeFieldDetails(FieldName));
		return false;
	}
	return true;
}

int32 FindSectionExact(const UAnimMontage* Montage, const FSectionSelector& Selector)
{
	if (Montage == nullptr || !Montage->CompositeSections.IsValidIndex(Selector.Index))
	{
		return INDEX_NONE;
	}

	const FCompositeSection& Section = Montage->CompositeSections[Selector.Index];
	return Section.SectionName.ToString() == Selector.Name
		&& FMath::IsNearlyEqual(Section.GetTime(), static_cast<float>(Selector.StartTime), 0.0001f)
		? Selector.Index
		: INDEX_NONE;
}

int32 FindSectionByName(const UAnimMontage* Montage, const FName& Name)
{
	if (Montage == nullptr)
	{
		return INDEX_NONE;
	}

	for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
	{
		if (Montage->CompositeSections[Index].SectionName == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool HasSectionName(const UAnimMontage* Montage, const FName& Name, int32 ExcludedIndex = INDEX_NONE)
{
	const int32 Index = FindSectionByName(Montage, Name);
	return Index != INDEX_NONE && Index != ExcludedIndex;
}

TArray<FPlannedSection> CurrentSections(const UAnimMontage* Montage)
{
	TArray<FPlannedSection> Sections;
	if (Montage == nullptr)
	{
		return Sections;
	}

	Sections.Reserve(Montage->CompositeSections.Num());
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		Sections.Add({ Section.SectionName, Section.GetTime(), Section.NextSectionName });
	}
	return Sections;
}

void SortSections(TArray<FPlannedSection>& Sections)
{
	Sections.Sort([](const FPlannedSection& Left, const FPlannedSection& Right)
	{
		if (!FMath::IsNearlyEqual(Left.StartTime, Right.StartTime))
		{
			return Left.StartTime < Right.StartTime;
		}
		return Left.Name.ToString() < Right.Name.ToString();
	});
}

void SortMontageSections(UAnimMontage* Montage)
{
	Montage->CompositeSections.Sort([](const FCompositeSection& Left, const FCompositeSection& Right)
	{
		if (!FMath::IsNearlyEqual(Left.GetTime(), Right.GetTime()))
		{
			return Left.GetTime() < Right.GetTime();
		}
		return Left.SectionName.ToString() < Right.SectionName.ToString();
	});
}

bool AreEquivalentSections(const TArray<FPlannedSection>& Left, const TArray<FPlannedSection>& Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (Left[Index].Name != Right[Index].Name
			|| Left[Index].StartTime != Right[Index].StartTime
			|| Left[Index].NextSectionName != Right[Index].NextSectionName)
		{
			return false;
		}
	}
	return true;
}

int32 FindPlannedSectionByName(const TArray<FPlannedSection>& Sections, const FName& Name)
{
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		if (Sections[Index].Name == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TSharedPtr<FJsonObject> PlannedSectionByName(const TArray<FPlannedSection>& Sections, const FName& Name)
{
	const int32 Index = FindPlannedSectionByName(Sections, Name);
	return Index == INDEX_NONE ? MissingSectionJson() : PlannedSectionToJson(Sections[Index], Index);
}

void RestoreLinks(UAnimMontage* Montage, const TMap<FName, FName>& Links)
{
	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (const FName* ExistingNext = Links.Find(Section.SectionName))
		{
			Section.NextSectionName = *ExistingNext;
		}
	}
}

TMap<FName, FName> CaptureLinks(const UAnimMontage* Montage)
{
	TMap<FName, FName> Links;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		Links.Add(Section.SectionName, Section.NextSectionName);
	}
	return Links;
}

void RefreshMontage(UAnimMontage* Montage)
{
	Montage->UpdateLinkableElements();
	Montage->RefreshCacheData();
	Montage->MarkPackageDirty();
}

FCortexCommandResult SectionNotFound(const FCortexAnimResolvedAsset& Resolved)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::AssetNotFound,
		TEXT("Montage section selector did not match an existing section"),
		SelectorDetails(Resolved.AssetPath));
}

bool ValidateNextSection(
	const UAnimMontage* Montage,
	const FString& NextSection,
	const FName& FinalName,
	FCortexCommandResult& OutError,
	const FString& FieldName)
{
	if (NextSection.IsEmpty())
	{
		return true;
	}

	if (NextSection == FinalName.ToString() || HasSectionName(Montage, FName(*NextSection)))
	{
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidField,
		FString::Printf(TEXT("%s must reference an existing montage section"), *FieldName),
		FCortexAnimMutationUtils::MakeFieldDetails(FieldName));
	return false;
}

void RewriteInboundLinks(UAnimMontage* Montage, const FName& OldName, const FName& NewName)
{
	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.NextSectionName == OldName)
		{
			Section.NextSectionName = NewName;
		}
	}
}
}

FCortexCommandResult FCortexAnimMontageOps::AddSection(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FCortexAnimResolvedAsset Resolved;
	UAnimMontage* Montage = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareMontageMutation(Params, Resolved, Montage, bDryRun, bSave, Error))
	{
		return Error;
	}

	FString Name;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: name (non-empty string)"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("name"), Resolved.AssetPath));
	}

	double StartTime = 0.0;
	if (!FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("start_time"), StartTime, Error)
		|| !FCortexAnimMutationUtils::ValidateMontageTime(Montage, TEXT("start_time"), StartTime, Error))
	{
		return Error;
	}

	FString NextSection;
	bool bNextSectionPresent = false;
	if (!TryReadOptionalString(Params, TEXT("next_section"), NextSection, bNextSectionPresent, Error))
	{
		return Error;
	}

	const FName SectionName(*Name);
	if (HasSectionName(Montage, SectionName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			TEXT("Montage section name already exists"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("name"), Resolved.AssetPath));
	}
	if (bNextSectionPresent && !ValidateNextSection(Montage, NextSection, SectionName, Error, TEXT("next_section")))
	{
		return Error;
	}

	const bool bDirtyBefore = Montage->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = MissingSectionJson();
	TArray<FPlannedSection> PlannedSections = CurrentSections(Montage);
	PlannedSections.Add({ SectionName, static_cast<float>(StartTime), NextSection.IsEmpty() ? NAME_None : FName(*NextSection) });
	SortSections(PlannedSections);
	const TSharedPtr<FJsonObject> PlannedAfter = PlannedSectionByName(PlannedSections, SectionName);
	const int32 PlannedIndex = FindPlannedSectionByName(PlannedSections, SectionName);
	const FSectionSelector PlannedSelector { PlannedIndex, Name, PlannedSections[PlannedIndex].StartTime };
	const TSharedPtr<FJsonObject> Selector = SelectorToJson(PlannedSelector);

	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("add_montage_section"), Selector, true, bDirtyBefore, bDirtyBefore, false, {}, Before, PlannedAfter, Montage));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Anim Montage Section")));
		Montage->Modify();
		const TMap<FName, FName> ExistingLinks = CaptureLinks(Montage);
		Montage->AddAnimCompositeSection(SectionName, static_cast<float>(StartTime));
		SortMontageSections(Montage);
		RestoreLinks(Montage, ExistingLinks);
		const int32 AddedIndex = FindSectionByName(Montage, SectionName);
		if (AddedIndex == INDEX_NONE)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Animation montage declined section creation"));
		}
		Montage->CompositeSections[AddedIndex].NextSectionName = NextSection.IsEmpty() ? NAME_None : FName(*NextSection);
		RefreshMontage(Montage);
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveMontageIfRequested(Montage, bSave, SavedPackages, Error))
	{
		return Error;
	}

	const int32 AddedIndex = FindSectionByName(Montage, SectionName);
	const FCompositeSection& AddedSection = Montage->CompositeSections[AddedIndex];
	const FSectionSelector ActualSelector { AddedIndex, AddedSection.SectionName.ToString(), AddedSection.GetTime() };
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("add_montage_section"), SelectorToJson(ActualSelector), true, bDirtyBefore, Montage->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, SectionToJson(Montage->CompositeSections[AddedIndex], AddedIndex), Montage));
}

FCortexCommandResult FCortexAnimMontageOps::UpdateSection(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FSectionSelector Selector;
	if (!TryReadSectionSelector(Params, Selector, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	UAnimMontage* Montage = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareMontageMutation(Params, Resolved, Montage, bDryRun, bSave, Error))
	{
		return Error;
	}

	const int32 SectionIndex = FindSectionExact(Montage, Selector);
	if (SectionIndex == INDEX_NONE)
	{
		return SectionNotFound(Resolved);
	}

	FString NewName = Selector.Name;
	FString RequestedName;
	bool bNamePresent = false;
	if (!TryReadOptionalString(Params, TEXT("new_name"), RequestedName, bNamePresent, Error))
	{
		return Error;
	}
	if (bNamePresent)
	{
		if (RequestedName.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("new_name must be non-empty"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("new_name")));
		}
		NewName = RequestedName;
	}

	double NewStartTime = Selector.StartTime;
	const bool bStartTimePresent = Params.IsValid() && Params->HasField(TEXT("new_start_time"));
	if (bStartTimePresent && (!FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("new_start_time"), NewStartTime, Error)
		|| !FCortexAnimMutationUtils::ValidateMontageTime(Montage, TEXT("new_start_time"), NewStartTime, Error)))
	{
		return Error;
	}

	FString NewNextSection;
	bool bNextSectionPresent = false;
	if (!TryReadOptionalString(Params, TEXT("new_next_section"), NewNextSection, bNextSectionPresent, Error))
	{
		return Error;
	}
	if (!bNamePresent && !bStartTimePresent && !bNextSectionPresent)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Update requires new_name, new_start_time, or new_next_section"));
	}

	const FName OldName(*Selector.Name);
	const FName FinalName(*NewName);
	if (FinalName != OldName && HasSectionName(Montage, FinalName, SectionIndex))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			TEXT("Montage section name already exists"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("new_name"), Resolved.AssetPath));
	}

	const FString EffectiveNext = bNextSectionPresent
		? NewNextSection
		: SectionNameToString(Montage->CompositeSections[SectionIndex].NextSectionName);
	const FName EffectiveNextName = EffectiveNext == Selector.Name && FinalName != OldName
		? FinalName
		: (EffectiveNext.IsEmpty() ? NAME_None : FName(*EffectiveNext));
	if (!ValidateNextSection(Montage, EffectiveNext, FinalName, Error, TEXT("new_next_section")))
	{
		return Error;
	}

	const bool bDirtyBefore = Montage->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = SectionToJson(Montage->CompositeSections[SectionIndex], SectionIndex);
	TArray<FPlannedSection> PlannedSections = CurrentSections(Montage);
	PlannedSections[SectionIndex].Name = FinalName;
	PlannedSections[SectionIndex].StartTime = static_cast<float>(NewStartTime);
	if (FinalName != OldName)
	{
		for (FPlannedSection& Section : PlannedSections)
		{
			if (Section.NextSectionName == OldName)
			{
				Section.NextSectionName = FinalName;
			}
		}
	}
	PlannedSections[SectionIndex].NextSectionName = EffectiveNextName;
	SortSections(PlannedSections);
	const TSharedPtr<FJsonObject> PlannedAfter = PlannedSectionByName(PlannedSections, FinalName);
	TArray<FPlannedSection> CanonicalCurrentSections = CurrentSections(Montage);
	SortSections(CanonicalCurrentSections);
	if (AreEquivalentSections(CanonicalCurrentSections, PlannedSections))
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("update_montage_section"), SelectorToJson(Selector), false, bDirtyBefore, bDirtyBefore, false, {}, Before, Before, Montage));
	}

	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("update_montage_section"), SelectorToJson(Selector), true, bDirtyBefore, bDirtyBefore, false, {}, Before, PlannedAfter, Montage));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Update Anim Montage Section")));
		Montage->Modify();
		Montage->CompositeSections[SectionIndex].SectionName = FinalName;
		Montage->CompositeSections[SectionIndex].Link(Montage, static_cast<float>(NewStartTime));
		if (FinalName != OldName)
		{
			RewriteInboundLinks(Montage, OldName, FinalName);
		}
		Montage->CompositeSections[SectionIndex].NextSectionName = EffectiveNextName;
		SortMontageSections(Montage);
		RefreshMontage(Montage);
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveMontageIfRequested(Montage, bSave, SavedPackages, Error))
	{
		return Error;
	}

	const int32 UpdatedIndex = FindSectionByName(Montage, FinalName);
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("update_montage_section"), SelectorToJson(Selector), true, bDirtyBefore, Montage->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, SectionToJson(Montage->CompositeSections[UpdatedIndex], UpdatedIndex), Montage));
}

FCortexCommandResult FCortexAnimMontageOps::RemoveSection(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	FSectionSelector Selector;
	if (!TryReadSectionSelector(Params, Selector, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	UAnimMontage* Montage = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareMontageMutation(Params, Resolved, Montage, bDryRun, bSave, Error))
	{
		return Error;
	}

	const int32 SectionIndex = FindSectionExact(Montage, Selector);
	if (SectionIndex == INDEX_NONE)
	{
		return SectionNotFound(Resolved);
	}

	const FName RemovedName = Montage->CompositeSections[SectionIndex].SectionName;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.SectionName != RemovedName && Section.NextSectionName == RemovedName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Cannot remove a montage section referenced by another section"),
				SelectorDetails(Resolved.AssetPath));
		}
	}

	const bool bDirtyBefore = Montage->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = SectionToJson(Montage->CompositeSections[SectionIndex], SectionIndex);
	const TSharedPtr<FJsonObject> After = MissingSectionJson();
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved, TEXT("remove_montage_section"), SelectorToJson(Selector), true, bDirtyBefore, bDirtyBefore, false, {}, Before, After, Montage));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Anim Montage Section")));
		Montage->Modify();
		const TMap<FName, FName> ExistingLinks = CaptureLinks(Montage);
		Montage->DeleteAnimCompositeSection(SectionIndex);
		RestoreLinks(Montage, ExistingLinks);
		SortMontageSections(Montage);
		RefreshMontage(Montage);
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveMontageIfRequested(Montage, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved, TEXT("remove_montage_section"), SelectorToJson(Selector), true, bDirtyBefore, Montage->GetPackage()->IsDirty(), bSave, SavedPackages,
		Before, After, Montage));
}
