#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FCortexCommandResult GetStructSchema(FCortexCommandRouter& Router, const FString& StructName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("struct_name"), StructName);
		return Router.Execute(TEXT("data.get_struct_schema"), Params);
	}

	FString SerializedSchema(const FCortexCommandResult& Result)
	{
		FString Out;
		const TSharedPtr<FJsonObject>* Schema = nullptr;
		if (Result.bSuccess && Result.Data.IsValid() && Result.Data->TryGetObjectField(TEXT("schema"), Schema) && Schema != nullptr)
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Schema->ToSharedRef(), Writer);
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetStructSchemaFullPathTest,
	"Cortex.Data.GetStructSchema.FullPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetStructSchemaFullPathTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"), MakeShared<FCortexDataCommandHandler>());

	const FCortexCommandResult ShortResult = GetStructSchema(Router, TEXT("HitResult"));
	const FCortexCommandResult PathResult = GetStructSchema(Router, TEXT("/Script/Engine.HitResult"));
	const FCortexCommandResult PrefixedResult = GetStructSchema(Router, TEXT("FHitResult"));

	TestTrue(TEXT("Short name resolves"), ShortResult.bSuccess);
	TestTrue(TEXT("Object path resolves"), PathResult.bSuccess);
	TestTrue(TEXT("F-prefixed name resolves"), PrefixedResult.bSuccess);

	const FString ShortSchema = SerializedSchema(ShortResult);
	TestFalse(TEXT("Short-name schema is not empty"), ShortSchema.IsEmpty());
	TestEqual(TEXT("Object path and short name produce the same schema"), SerializedSchema(PathResult), ShortSchema);
	TestEqual(TEXT("F-prefixed name produces the same schema"), SerializedSchema(PrefixedResult), ShortSchema);

	// A user-defined struct would be an on-disk /Game asset; the sandbox ships none under Content/Data.
	AddInfo(TEXT("No sandbox user-defined struct asset exists under Content/Data; /Game object path loading is covered only by the unknown-path refusal below."));

	{
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/Game/Data/S_DefinitelyMissing.S_DefinitelyMissing"));
		TestFalse(TEXT("Unknown /Game path is refused"), R.bSuccess);
		TestEqual(TEXT("Unknown /Game path error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}

	{
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/Script/Engine.NoSuchStructAnywhere"));
		TestFalse(TEXT("Unknown /Script path is refused"), R.bSuccess);
		TestEqual(TEXT("Unknown /Script path error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetStructSchemaNonStructAssetTest,
	"Cortex.Data.GetStructSchema.NonStructAssetRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetStructSchemaNonStructAssetTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"), MakeShared<FCortexDataCommandHandler>());

	// An existing non-struct asset must be refused from registry metadata, without loading it
	// and without a "Failed to find object" load warning.
	const TCHAR* PackageName = TEXT("/Game/Data/CT_TestCurve");
	const FString ObjectPath = FString::Printf(TEXT("%s.CT_TestCurve"), PackageName);
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		AddInfo(TEXT("Sandbox curve table fixture missing; non-struct refusal not exercised."));
		return true;
	}

	const bool bLoadedBefore = FindPackage(nullptr, PackageName) != nullptr;

	{
		const FCortexCommandResult R = GetStructSchema(Router, ObjectPath);
		TestFalse(TEXT("Non-struct asset path is refused"), R.bSuccess);
		TestEqual(TEXT("Non-struct asset error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}

	{
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/Game/Data/CT_TestCurve.NoSuchObject"));
		TestFalse(TEXT("Wrong object name in a real package is refused"), R.bSuccess);
		TestEqual(TEXT("Wrong object name error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}

	if (!bLoadedBefore)
	{
		TestTrue(TEXT("Refusing a non-struct asset does not load its package"), FindPackage(nullptr, PackageName) == nullptr);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetStructSchemaMountedRootTest,
	"Cortex.Data.GetStructSchema.MountedRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetStructSchemaMountedRootTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"), MakeShared<FCortexDataCommandHandler>());

	// A user-defined struct under a mounted non-/Game root (engine or plugin content) resolves by object path.
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	Registry.WaitForCompletion();
	FARFilter Filter;
	Filter.ClassPaths.Add(UUserDefinedStruct::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> Structs;
	Registry.GetAssets(Filter, Structs);

	const FAssetData* Candidate = nullptr;
	for (const FAssetData& Asset : Structs)
	{
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/")) && !Asset.IsAssetLoaded())
		{
			Candidate = &Asset;
			break;
		}
	}
	if (Candidate != nullptr)
	{
		const FCortexCommandResult R = GetStructSchema(Router, Candidate->GetObjectPathString());
		TestTrue(TEXT("Unloaded struct under a mounted non-/Game root resolves"), R.bSuccess);
	}
	else
	{
		AddInfo(TEXT("No unloaded UserDefinedStruct exists under a mounted non-/Game root; only the refusal paths are exercised."));
	}

	// Mounted engine root, existing non-struct asset: refused from registry metadata without loading it.
	const TCHAR* EnginePackage = TEXT("/Engine/BasicShapes/Cube");
	if (FPackageName::DoesPackageExist(EnginePackage))
	{
		const bool bLoadedBefore = FindPackage(nullptr, EnginePackage) != nullptr;
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/Engine/BasicShapes/Cube.Cube"));
		TestFalse(TEXT("Non-struct engine asset is refused"), R.bSuccess);
		TestEqual(TEXT("Non-struct engine asset error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
		if (!bLoadedBefore)
		{
			TestTrue(TEXT("Refusing a non-struct engine asset does not load its package"), FindPackage(nullptr, EnginePackage) == nullptr);
		}
	}
	else
	{
		AddInfo(TEXT("/Engine/BasicShapes/Cube is unavailable; non-struct engine refusal not exercised."));
	}

	{
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/Engine/DefinitelyMissing/S_Nope.S_Nope"));
		TestFalse(TEXT("Unknown path under a mounted root is refused"), R.bSuccess);
		TestEqual(TEXT("Unknown mounted-root path error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}

	{
		const FCortexCommandResult R = GetStructSchema(Router, TEXT("/NotMountedRoot/S_Nope.S_Nope"));
		TestFalse(TEXT("Path under an unmounted root is refused"), R.bSuccess);
		TestEqual(TEXT("Unmounted-root path error code"), R.ErrorCode, CortexErrorCodes::InvalidStructType);
	}
	return true;
}
