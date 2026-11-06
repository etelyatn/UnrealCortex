#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UStateTree;
struct FStateTreeCompilerLog;

class FCortexSTValidationOps
{
public:
	static FCortexCommandResult CheckStructure(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ValidateAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Compile(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RunPostMutationFixups(UStateTree* StateTree);

	/** status / compile_status / error_count / warning_count / diagnostics / fingerprint for one compile attempt (shared by compile and by the migration ops). */
	static TSharedPtr<FJsonObject> MakeCompilePayload(
		const FString& AssetPath,
		UStateTree* StateTree,
		const FStateTreeCompilerLog& CompileLog,
		bool bCompiled);
};
