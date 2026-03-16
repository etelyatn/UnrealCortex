# UMG Widget Patterns

## UserWidget Base
```cpp
UCLASS()
class UMyWidget : public UUserWidget
{
    GENERATED_BODY()

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

    UPROPERTY(meta = (BindWidget))
    TObjectPtr<UTextBlock> TitleText;

    UPROPERTY(meta = (BindWidget))
    TObjectPtr<UButton> ActionButton;

    UPROPERTY(meta = (BindWidgetOptional))
    TObjectPtr<UImage> IconImage;
};
```

## Widget Binding
`meta = (BindWidget)` binds to a widget in the UMG designer by matching variable name.
`meta = (BindWidgetOptional)` allows the widget to be missing in the designer.

## Button Click Binding
```cpp
void UMyWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (ActionButton)
    {
        ActionButton->OnClicked.AddDynamic(this, &UMyWidget::OnActionClicked);
    }
}

UFUNCTION()
void OnActionClicked();
```

## Property Binding (Text, Visibility, etc.)
Use `UPROPERTY(BlueprintReadWrite, meta = (BindWidget))` for designer-editable widgets.
For dynamic text, set in C++:
```cpp
TitleText->SetText(FText::FromString(TEXT("Hello")));
```
