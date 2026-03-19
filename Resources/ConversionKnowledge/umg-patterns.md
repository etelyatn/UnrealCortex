# UMG Widget Blueprint Conversion Patterns

## Core Principle

Widget BP migration moves LOGIC to C++. The widget tree stays in the UMG designer.
C++ references designer widgets via `BindWidget`. Never recreate the widget hierarchy in C++.

## UserWidget Base Class

```cpp
UCLASS()
class UMyWidget : public UUserWidget
{
    GENERATED_BODY()

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

    // BindWidget — MUST match widget name in UMG designer exactly
    UPROPERTY(meta = (BindWidget))
    TObjectPtr<UTextBlock> TitleText;

    UPROPERTY(meta = (BindWidget))
    TObjectPtr<UButton> ActionButton;

    // BindWidgetOptional — widget may be missing in subclasses
    UPROPERTY(meta = (BindWidgetOptional))
    TObjectPtr<UImage> IconImage;

    // BindWidgetAnim — references a designer-created animation
    UPROPERTY(meta = (BindWidgetAnim), Transient)
    TObjectPtr<UWidgetAnimation> FadeInAnimation;
};
```

## Lifecycle Overrides

| Blueprint Event | C++ Override |
|----------------|-------------|
| Event Construct | `NativeConstruct()` |
| Event Destruct | `NativeDestruct()` |
| Event Tick | `NativeTick(const FGeometry&, float)` |
| Event Pre-Construct | `NativePreConstruct(bool bIsDesignTime)` (design-time preview) |
| Event On Initialized | `NativeOnInitialized()` |

Always call `Super::` in every override.

## Delegate Binding Pattern

```cpp
void UMyWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (ActionButton)
    {
        ActionButton->OnClicked.AddDynamic(this, &UMyWidget::OnActionClicked);
    }
    if (NameInput)
    {
        NameInput->OnTextChanged.AddDynamic(this, &UMyWidget::OnNameChanged);
    }
}

void UMyWidget::NativeDestruct()
{
    // ALWAYS unbind — child widget pointers may already be null during teardown
    if (ActionButton)
    {
        ActionButton->OnClicked.RemoveDynamic(this, &UMyWidget::OnActionClicked);
    }
    if (NameInput)
    {
        NameInput->OnTextChanged.RemoveDynamic(this, &UMyWidget::OnNameChanged);
    }

    Super::NativeDestruct();
}
```

## Common Widget Delegates

| Widget Type | Delegate | Signature |
|-------------|----------|-----------|
| UButton | OnClicked | `void()` |
| UButton | OnPressed | `void()` |
| UButton | OnReleased | `void()` |
| UButton | OnHovered | `void()` |
| UButton | OnUnhovered | `void()` |
| UCheckBox | OnCheckStateChanged | `void(bool bIsChecked)` |
| UEditableTextBox | OnTextChanged | `void(const FText& Text)` |
| UEditableTextBox | OnTextCommitted | `void(const FText& Text, ETextCommit::Type)` |
| UComboBoxString | OnSelectionChanged | `void(FString SelectedItem, ESelectInfo::Type)` |
| USlider | OnValueChanged | `void(float Value)` |
| USpinBox | OnValueChanged | `void(float Value)` |
| UScrollBox | OnUserScrolled | `void(float CurrentOffset)` |

## Property Updates (Common Patterns)

```cpp
// Text
TitleText->SetText(FText::FromString(TEXT("Hello")));

// Visibility
PanelWidget->SetVisibility(ESlateVisibility::Collapsed);

// Color
TitleText->SetColorAndOpacity(FSlateColor(FLinearColor::Red));

// Image
IconImage->SetBrushFromTexture(MyTexture);
IconImage->SetBrushFromSoftTexture(MySoftTexture);

// Enable/Disable
ActionButton->SetIsEnabled(bCanAct);
```

## ListView / TileView Pattern

```cpp
// Entry widget class set in designer, NOT in C++
// C++ manages the data:
UPROPERTY(meta = (BindWidget))
TObjectPtr<UListView> ItemList;

// Populate:
ItemList->SetListItems(MyDataArray);

// Entry widget implements IUserObjectListEntry:
void UMyEntryWidget::NativeOnListItemObjectSet(UObject* ListItemObject)
{
    if (UMyItemData* Data = Cast<UMyItemData>(ListItemObject))
    {
        ItemNameText->SetText(FText::FromString(Data->Name));
    }
}
```

## Include Paths

| Class | Include |
|-------|---------|
| UUserWidget | `Blueprint/UserWidget.h` |
| UButton | `Components/Button.h` |
| UTextBlock | `Components/TextBlock.h` |
| UImage | `Components/Image.h` |
| UEditableTextBox | `Components/EditableTextBox.h` |
| UCheckBox | `Components/CheckBox.h` |
| UComboBoxString | `Components/ComboBoxString.h` |
| USlider | `Components/Slider.h` |
| UScrollBox | `Components/ScrollBox.h` |
| UListView | `Components/ListView.h` |
| UWidgetAnimation | `Animation/WidgetAnimation.h` |
| UWidgetSwitcher | `Components/WidgetSwitcher.h` |
| UOverlay | `Components/Overlay.h` |
| USizeBox | `Components/SizeBox.h` |
