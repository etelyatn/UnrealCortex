#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexShutdownCommandRegisteredTest,
	"Cortex.Core.Shutdown.CommandRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexShutdownCommandRegisteredTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	if (CapResult.bSuccess && CapResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
		if (CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr)
		{
			const TSharedPtr<FJsonObject>* CoreObj = nullptr;
			if ((*DomainsObj)->TryGetObjectField(TEXT("core"), CoreObj) && CoreObj != nullptr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
				if ((*CoreObj)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr)
				{
					bool bFoundShutdown = false;
					for (const TSharedPtr<FJsonValue>& CmdVal : *Commands)
					{
						const TSharedPtr<FJsonObject>* CmdObj = nullptr;
						if (CmdVal->TryGetObject(CmdObj) && CmdObj != nullptr)
						{
							FString CmdName;
							if ((*CmdObj)->TryGetStringField(TEXT("name"), CmdName) && CmdName == TEXT("shutdown"))
							{
								bFoundShutdown = true;
								break;
							}
						}
					}
					TestTrue(TEXT("shutdown command found in core capabilities"), bFoundShutdown);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexShutdownDoubleGuardTest,
	"Cortex.Core.Shutdown.DoubleGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexShutdownDoubleGuardTest::RunTest(const FString& Parameters)
{
	// Cannot call shutdown in automation because it exits the editor process.
	AddInfo(TEXT("Double-guard behavior is covered by E2E lifecycle tests"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexShutdownRefusesDirtyTest,
	"Cortex.Core.Shutdown.RefusesDirty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexShutdownRefusesDirtyTest::RunTest(const FString& Parameters)
{
	const FString PackageName = FString::Printf(TEXT("/Game/__CortexShutdownDirtyTest_%s"), *FGuid::NewGuid().ToString().Right(8));
	UPackage* Package = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("Fixture package"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(true);

	FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	// Default force is false. A refusal arms no exit: a second call is refused the same way instead of
	// reporting "Shutdown already in progress", and the editor keeps running to finish this test.
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		const FCortexCommandResult Result = Router.Execute(TEXT("core.shutdown"), MakeShared<FJsonObject>());
		TestFalse(TEXT("shutdown with dirty packages and no force is refused"), Result.bSuccess);
		TestEqual(TEXT("refusal code"), Result.ErrorCode, CortexErrorCodes::UnsavedChanges);
		if (!Result.ErrorDetails.IsValid())
		{
			AddError(TEXT("Refusal carries details"));
			continue;
		}

		TestTrue(TEXT("details.count covers the fixture"), Result.ErrorDetails->GetNumberField(TEXT("count")) >= 1.0);
		const TArray<TSharedPtr<FJsonValue>>* Dirty = nullptr;
		if (TestTrue(TEXT("details.dirty_packages present"), Result.ErrorDetails->TryGetArrayField(TEXT("dirty_packages"), Dirty)) && Dirty != nullptr)
		{
			bool bFound = false;
			for (const TSharedPtr<FJsonValue>& Value : *Dirty)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				FString Name;
				if (Value->TryGetObject(Entry) && Entry != nullptr
					&& (*Entry)->TryGetStringField(TEXT("package"), Name) && Name == PackageName)
				{
					bFound = true;
				}
			}
			TestTrue(TEXT("fixture package is listed in dirty_packages"), bFound);
		}
	}

	TestTrue(TEXT("refusal leaves the dirty flag untouched"), Package->IsDirty());

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return true;
}
