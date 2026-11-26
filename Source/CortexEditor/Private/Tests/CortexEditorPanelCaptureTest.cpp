// editor.capture_editor_panel renders an Editor panel (an asset editor, its Details panel or graph, or a whole
// top-level window) to a PNG through Slate, and refuses invalid panels, unknown assets, maps and missing panels
// without writing a file.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexEditorCommandHandler.h"
#include "CortexTypes.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FString MakePanelCapturePath(const FString& Suffix, const TCHAR* Label)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexTests");
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir / FString::Printf(TEXT("cortex_panel_%s_%s.png"), Label, *Suffix);
	}

	TSharedPtr<FJsonObject> MakePanelParams(const FString& AssetPath, const FString& Panel, const FString& OutputPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (!AssetPath.IsEmpty())
		{
			Params->SetStringField(TEXT("asset_path"), AssetPath);
		}
		if (!Panel.IsEmpty())
		{
			Params->SetStringField(TEXT("panel"), Panel);
		}
		if (!OutputPath.IsEmpty())
		{
			Params->SetStringField(TEXT("output_path"), OutputPath);
		}
		return Params;
	}

	/** Decodes the PNG on disk; returns false when it is missing or not a PNG. */
	bool DecodePng(const FString& Path, int32& OutWidth, int32& OutHeight, TArray64<uint8>& OutBgra)
	{
		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
		{
			return false;
		}
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(FileBytes.GetData(), FileBytes.Num()))
		{
			return false;
		}
		OutWidth = static_cast<int32>(Png->GetWidth());
		OutHeight = static_cast<int32>(Png->GetHeight());
		return Png->GetRaw(ERGBFormat::BGRA, 8, OutBgra);
	}

	/** A blank (single-colour) image is what a capture of an undrawn surface looks like. */
	bool HasSeveralColors(const TArray64<uint8>& Bgra)
	{
		TSet<uint32> Colors;
		for (int64 Index = 0; Index + 3 < Bgra.Num() && Colors.Num() < 8; Index += 4)
		{
			Colors.Add(Bgra[Index] | (Bgra[Index + 1] << 8) | (Bgra[Index + 2] << 16));
		}
		return Colors.Num() >= 8;
	}

	struct FPanelCapture
	{
		bool bSuccess = false;
		int32 Width = 0;
		int32 Height = 0;
		FString WidgetType;
	};

	/** Fraction of pixels that differ between two PNGs of the same size; 1 when they cannot be compared. */
	double DifferingPixelFraction(const FString& PathA, const FString& PathB)
	{
		int32 WidthA = 0;
		int32 HeightA = 0;
		int32 WidthB = 0;
		int32 HeightB = 0;
		TArray64<uint8> A;
		TArray64<uint8> B;
		if (!DecodePng(PathA, WidthA, HeightA, A) || !DecodePng(PathB, WidthB, HeightB, B)
			|| WidthA != WidthB || HeightA != HeightB || A.Num() != B.Num() || A.Num() == 0)
		{
			return 1.0;
		}
		int64 Differing = 0;
		for (int64 Index = 0; Index + 3 < A.Num(); Index += 4)
		{
			if (FMemory::Memcmp(&A[Index], &B[Index], 3) != 0)
			{
				++Differing;
			}
		}
		return static_cast<double>(Differing) / static_cast<double>(A.Num() / 4);
	}

	/** Runs one capture and checks the response against the PNG it wrote. */
	FPanelCapture CaptureAndCheck(FAutomationTestBase& Test, FCortexEditorCommandHandler& Handler,
		const FString& AssetPath, const FString& Panel, const FString& ExpectedPanel, const FString& OutputPath)
	{
		FPanelCapture Capture;
		const FCortexCommandResult Result = Handler.Execute(TEXT("capture_editor_panel"), MakePanelParams(AssetPath, Panel, OutputPath));
		const FString Label = FString::Printf(TEXT("panel '%s'"), *ExpectedPanel);
		if (!Test.TestTrue(FString::Printf(TEXT("%s capture succeeds (%s: %s)"), *Label, *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
			|| !Test.TestTrue(FString::Printf(TEXT("%s returns data"), *Label), Result.Data.IsValid()))
		{
			return Capture;
		}

		Test.TestEqual(FString::Printf(TEXT("%s reports the panel"), *Label), Result.Data->GetStringField(TEXT("panel")), ExpectedPanel);
		Test.TestEqual(FString::Printf(TEXT("%s reports the written path"), *Label),
			Result.Data->GetStringField(TEXT("path")), FPaths::ConvertRelativePathToFull(OutputPath));
		Test.TestFalse(FString::Printf(TEXT("%s names the captured window"), *Label), Result.Data->GetStringField(TEXT("window_title")).IsEmpty());
		Capture.WidgetType = Result.Data->GetStringField(TEXT("widget_type"));
		Capture.Width = static_cast<int32>(Result.Data->GetNumberField(TEXT("width")));
		Capture.Height = static_cast<int32>(Result.Data->GetNumberField(TEXT("height")));
		if (!AssetPath.IsEmpty())
		{
			Test.TestEqual(FString::Printf(TEXT("%s echoes the asset"), *Label), Result.Data->GetStringField(TEXT("asset_path")), AssetPath);
			Test.TestFalse(FString::Printf(TEXT("%s names the asset editor"), *Label), Result.Data->GetStringField(TEXT("editor_name")).IsEmpty());
		}

		int32 PngWidth = 0;
		int32 PngHeight = 0;
		TArray64<uint8> Bgra;
		if (Test.TestTrue(FString::Printf(TEXT("%s wrote a decodable PNG"), *Label), DecodePng(OutputPath, PngWidth, PngHeight, Bgra)))
		{
			Test.TestTrue(FString::Printf(TEXT("%s is not empty"), *Label), Capture.Width > 0 && Capture.Height > 0);
			Test.TestEqual(FString::Printf(TEXT("%s PNG width matches the response"), *Label), PngWidth, Capture.Width);
			Test.TestEqual(FString::Printf(TEXT("%s PNG height matches the response"), *Label), PngHeight, Capture.Height);
			Test.TestTrue(FString::Printf(TEXT("%s shows drawn UI, not a blank surface"), *Label), HasSeveralColors(Bgra));
			Test.TestEqual(FString::Printf(TEXT("%s reports the file size"), *Label),
				static_cast<int64>(Result.Data->GetNumberField(TEXT("file_size_bytes"))), IFileManager::Get().FileSize(*OutputPath));
			Capture.bSuccess = true;
		}
		return Capture;
	}

	void ExpectRefusal(FAutomationTestBase& Test, FCortexEditorCommandHandler& Handler, const TCHAR* Label,
		const TSharedPtr<FJsonObject>& Params, const FString& ExpectedCode, const FString& OutputPath)
	{
		const FCortexCommandResult Result = Handler.Execute(TEXT("capture_editor_panel"), Params);
		Test.TestFalse(FString::Printf(TEXT("%s is refused"), Label), Result.bSuccess);
		Test.TestEqual(FString::Printf(TEXT("%s code (%s)"), Label, *Result.ErrorMessage), Result.ErrorCode, ExpectedCode);
		Test.TestFalse(FString::Printf(TEXT("%s writes no file"), Label), IFileManager::Get().FileExists(*OutputPath));
	}

	void DiscardFixture(UAssetEditorSubsystem* AssetEditors, UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return;
		}
		if (AssetEditors != nullptr)
		{
			AssetEditors->CloseAllEditorsForAsset(Asset);
		}
		Asset->ClearFlags(RF_Standalone);
		Asset->MarkAsGarbage();
		Asset->GetOutermost()->SetDirtyFlag(false);
		Asset->GetOutermost()->MarkAsGarbage();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureEditorPanelAssetTest,
	"Cortex.Editor.CaptureEditorPanel.AssetEditorPanels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorCaptureEditorPanelAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}
	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!TestNotNull(TEXT("Asset editor subsystem is available"), AssetEditors))
	{
		return false;
	}

	// A resident material (registered, never saved): the material editor shows its graph and a Details panel.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString AssetName = FString::Printf(TEXT("M_PanelCapture_%s"), *Suffix);
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexPanelCapture/%s"), *AssetName));
	UMaterial* Material = NewObject<UMaterial>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
	Constant->R = 0.5f;
	Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Constant);
	FAssetRegistryModule::AssetCreated(Material);
	Package->SetDirtyFlag(false);
	const FString AssetPath = Material->GetPathName();

	FCortexEditorCommandHandler Handler;
	const FString GraphPath = MakePanelCapturePath(Suffix, TEXT("graph"));
	const FString GraphAgainPath = MakePanelCapturePath(Suffix, TEXT("graph_again"));
	const FString DetailsPath = MakePanelCapturePath(Suffix, TEXT("details"));
	const FString EditorPath = MakePanelCapturePath(Suffix, TEXT("editor"));
	const FString WindowPath = MakePanelCapturePath(Suffix, TEXT("window"));

	const FPanelCapture Graph = CaptureAndCheck(*this, Handler, AssetPath, TEXT("graph"), TEXT("graph"), GraphPath);
	TestTrue(TEXT("the material editor is open after the capture"), AssetEditors->FindEditorForAsset(Material, false) != nullptr);
	// The first capture opened the editor; it must already show the settled graph, as a later capture does.
	const FPanelCapture GraphAgain = CaptureAndCheck(*this, Handler, AssetPath, TEXT("graph"), TEXT("graph"), GraphAgainPath);
	if (Graph.bSuccess && GraphAgain.bSuccess)
	{
		const double Changed = DifferingPixelFraction(GraphPath, GraphAgainPath);
		TestTrue(FString::Printf(TEXT("the graph captured while opening the editor matches a later capture (%.1f%% of pixels differ)"), Changed * 100.0),
			Changed < 0.02);
	}
	const FPanelCapture Details = CaptureAndCheck(*this, Handler, AssetPath, TEXT("details"), TEXT("details"), DetailsPath);
	const FPanelCapture Editor = CaptureAndCheck(*this, Handler, AssetPath, FString(), TEXT("editor"), EditorPath);
	const FPanelCapture Window = CaptureAndCheck(*this, Handler, AssetPath, TEXT("window"), TEXT("window"), WindowPath);

	if (Graph.bSuccess && Details.bSuccess && Editor.bSuccess && Window.bSuccess)
	{
		TestEqual(TEXT("graph resolves to the graph editor widget"), Graph.WidgetType, FString(TEXT("SGraphEditor")));
		TestEqual(TEXT("details resolves to a Details view widget"), Details.WidgetType, FString(TEXT("SDetailsView")));
		TestEqual(TEXT("window resolves to the hosting window"), Window.WidgetType, FString(TEXT("SWindow")));
		// Each panel lies inside the asset editor, which lies inside its window.
		TestTrue(TEXT("the graph is a part of the editor"), Graph.Width <= Editor.Width && Graph.Height <= Editor.Height
			&& Graph.Width * Graph.Height < Editor.Width * Editor.Height);
		TestTrue(TEXT("the Details panel is a part of the editor"), Details.Width <= Editor.Width && Details.Height <= Editor.Height
			&& Details.Width * Details.Height < Editor.Width * Editor.Height);
		TestTrue(TEXT("the editor is a part of its window"), Editor.Width <= Window.Width && Editor.Height <= Window.Height
			&& Editor.Width * Editor.Height < Window.Width * Window.Height);
	}

	for (const FString& Path : {GraphPath, GraphAgainPath, DetailsPath, EditorPath, WindowPath})
	{
		IFileManager::Get().Delete(*Path, false, true, true);
	}
	DiscardFixture(AssetEditors, Material);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureEditorPanelWindowTest,
	"Cortex.Editor.CaptureEditorPanel.ActiveWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorCaptureEditorPanelWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString WindowPath = MakePanelCapturePath(Suffix, TEXT("active_window"));
	const FString DetailsPath = MakePanelCapturePath(Suffix, TEXT("level_details"));

	// Without an asset, the default captures the active top-level window (the main frame when none is active).
	const FPanelCapture Window = CaptureAndCheck(*this, Handler, FString(), FString(), TEXT("window"), WindowPath);
	if (Window.bSuccess)
	{
		TestEqual(TEXT("the default capture is a window"), Window.WidgetType, FString(TEXT("SWindow")));
	}

	// Without an asset, details finds the Details panel of that window: the level editor's Details panel, which shows
	// properties once an actor is selected (empty, it is one flat colour). The selection is restored afterwards.
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	AActor* Subject = nullptr;
	if (EditorWorld != nullptr)
	{
		for (TActorIterator<AActor> It(EditorWorld); It; ++It)
		{
			if (It->IsSelectable() && It->IsEditable() && !It->IsA<AWorldSettings>())
			{
				Subject = *It;
				break;
			}
		}
	}
	if (Subject == nullptr)
	{
		AddInfo(TEXT("The editor world has no selectable actor; the level Details capture is skipped"));
	}
	else
	{
		// Only the subject is added and removed again; any existing selection (actors, components, BSP) is left alone.
		const bool bWasSelected = Subject->IsSelected();
		if (!bWasSelected)
		{
			GEditor->SelectActor(Subject, true, true);
		}

		const FPanelCapture Details = CaptureAndCheck(*this, Handler, FString(), TEXT("details"), TEXT("details"), DetailsPath);
		if (Window.bSuccess && Details.bSuccess)
		{
			TestEqual(TEXT("the level editor Details panel is a Details view"), Details.WidgetType, FString(TEXT("SDetailsView")));
			TestTrue(TEXT("the Details panel is a part of the window"), Details.Width * Details.Height < Window.Width * Window.Height);
		}

		if (!bWasSelected)
		{
			GEditor->SelectActor(Subject, false, true);
		}
	}

	IFileManager::Get().Delete(*WindowPath, false, true, true);
	IFileManager::Get().Delete(*DetailsPath, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureEditorPanelRefusalTest,
	"Cortex.Editor.CaptureEditorPanel.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorCaptureEditorPanelRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}
	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("Asset editor subsystem is available"), AssetEditors) || !TestNotNull(TEXT("Editor world is available"), EditorWorld))
	{
		return false;
	}

	FCortexEditorCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString OutputPath = MakePanelCapturePath(Suffix, TEXT("refused"));

	ExpectRefusal(*this, Handler, TEXT("an unknown panel"),
		MakePanelParams(FString(), TEXT("toolbar"), OutputPath), CortexErrorCodes::InvalidField, OutputPath);
	ExpectRefusal(*this, Handler, TEXT("panel 'editor' without an asset"),
		MakePanelParams(FString(), TEXT("editor"), OutputPath), CortexErrorCodes::InvalidField, OutputPath);
	ExpectRefusal(*this, Handler, TEXT("an asset that does not exist"),
		MakePanelParams(FString::Printf(TEXT("/Game/Temp/CortexPanelCapture_%s/Missing.Missing"), *Suffix), TEXT("details"), OutputPath),
		CortexErrorCodes::AssetNotFound, OutputPath);

	// A map is never opened: opening it would switch the editor world.
	ExpectRefusal(*this, Handler, TEXT("a map"),
		MakePanelParams(EditorWorld->GetPathName(), TEXT("editor"), OutputPath), CortexErrorCodes::InvalidOperation, OutputPath);
	TestTrue(TEXT("the editor world is unchanged after the map refusal"), GEditor->GetEditorWorldContext().World() == EditorWorld);

	// A curve opens the curve asset editor, which has no graph editor.
	const FString CurveName = FString::Printf(TEXT("Curve_PanelCapture_%s"), *Suffix);
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexPanelCapture/%s"), *CurveName));
	UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *CurveName, RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Curve);
	Package->SetDirtyFlag(false);
	ExpectRefusal(*this, Handler, TEXT("a panel the asset editor does not show"),
		MakePanelParams(Curve->GetPathName(), TEXT("graph"), OutputPath), CortexErrorCodes::WidgetNotFound, OutputPath);

	IFileManager::Get().Delete(*OutputPath, false, true, true);
	DiscardFixture(AssetEditors, Curve);
	return true;
}

#endif
