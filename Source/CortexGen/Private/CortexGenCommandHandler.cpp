#include "CortexGenCommandHandler.h"
#include "CortexGenModule.h"
#include "CortexGenSettings.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexGenJobManager.h"
#include "Providers/ICortexGenProvider.h"
#include "Dom/JsonObject.h"

FCortexGenCommandHandler::FCortexGenCommandHandler()
    : JobManager(MakeShared<FCortexGenJobManager>())
{
}

FCortexGenCommandHandler::FCortexGenCommandHandler(TSharedPtr<FCortexGenJobManager> InJobManager)
    : JobManager(InJobManager)
{
}

FCortexCommandResult FCortexGenCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    if (Command == TEXT("start_mesh"))       return HandleStartMesh(Params);
    if (Command == TEXT("start_image"))      return HandleStartImage(Params);
    if (Command == TEXT("start_texturing"))  return HandleStartTexturing(Params);
    if (Command == TEXT("job_status"))       return HandleJobStatus(Params);
    if (Command == TEXT("list_jobs"))        return HandleListJobs(Params);
    if (Command == TEXT("cancel_job"))       return HandleCancelJob(Params);
    if (Command == TEXT("retry_import"))     return HandleRetryImport(Params);
    if (Command == TEXT("list_providers"))   return HandleListProviders(Params);
    if (Command == TEXT("delete_job"))       return HandleDeleteJob(Params);
    if (Command == TEXT("get_config"))       return HandleGetConfig(Params);

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown gen command: %s"), *Command));
}

TArray<FCortexCommandInfo> FCortexGenCommandHandler::GetSupportedCommands() const
{
    TArray<FCortexCommandInfo> Commands;

    Commands.Add(FCortexCommandInfo{TEXT("start_mesh"), TEXT("Generate a 3D mesh from a text prompt or reference image")}
        .Optional(TEXT("prompt"), TEXT("string"), TEXT("Text description of the mesh to generate"))
        .Optional(TEXT("source_image_path"), TEXT("string"), TEXT("Local file path to reference image for image-to-mesh"))
        .Optional(TEXT("provider"), TEXT("string"), TEXT("Provider ID to use (default: settings default)"))
        .Optional(TEXT("destination"), TEXT("string"), TEXT("UE content path for import destination"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("start_image"), TEXT("Generate a reference image from a text prompt")}
        .Required(TEXT("prompt"), TEXT("string"), TEXT("Text description of the image to generate"))
        .Optional(TEXT("provider"), TEXT("string"), TEXT("Provider ID to use (default: settings default)"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("start_texturing"), TEXT("Apply AI texturing to an existing mesh")}
        .Required(TEXT("source_model_path"), TEXT("string"), TEXT("UE asset path of the mesh to texture"))
        .Optional(TEXT("prompt"), TEXT("string"), TEXT("Text description for the texturing style"))
        .Optional(TEXT("provider"), TEXT("string"), TEXT("Provider ID to use (default: settings default)"))
        .Optional(TEXT("destination"), TEXT("string"), TEXT("UE content path for import destination"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("job_status"), TEXT("Get the current status of a generation job")}
        .Required(TEXT("job_id"), TEXT("string"), TEXT("Job ID returned by start_mesh/start_image/start_texturing"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("list_jobs"), TEXT("List generation jobs with optional status filter")}
        .Optional(TEXT("status"), TEXT("string"), TEXT("Filter by status (e.g. Pending, Processing, Imported, Failed)"))
        .Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum number of jobs to return (0 = no limit)"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("cancel_job"), TEXT("Cancel an active generation job")}
        .Required(TEXT("job_id"), TEXT("string"), TEXT("Job ID to cancel"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("retry_import"), TEXT("Retry a failed download or import step without re-generating")}
        .Required(TEXT("job_id"), TEXT("string"), TEXT("Job ID in DownloadFailed or ImportFailed state"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("list_providers"), TEXT("List all registered AI generation providers and their capabilities")}
    );

    Commands.Add(FCortexCommandInfo{TEXT("delete_job"), TEXT("Delete a completed, failed, or cancelled job from history")}
        .Required(TEXT("job_id"), TEXT("string"), TEXT("Job ID to delete"))
    );

    Commands.Add(FCortexCommandInfo{TEXT("get_config"), TEXT("Get the current CortexGen configuration from settings")}
    );

    return Commands;
}

//-----------------------------------------------------------------------------
// Command handlers
//-----------------------------------------------------------------------------

FCortexCommandResult FCortexGenCommandHandler::HandleStartMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString Prompt;
    FString SourceImagePath;

    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("prompt"), Prompt);
        Params->TryGetStringField(TEXT("source_image_path"), SourceImagePath);
    }

    const bool bHasPrompt = !Prompt.IsEmpty();
    const bool bHasImage  = !SourceImagePath.IsEmpty();

    if (!bHasPrompt && !bHasImage)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("start_mesh requires either 'prompt' or 'source_image_path' (or both)"));
    }

    ECortexGenJobType JobType;
    if (bHasPrompt && bHasImage)
    {
        JobType = ECortexGenJobType::MeshFromBoth;
    }
    else if (bHasImage)
    {
        JobType = ECortexGenJobType::MeshFromImage;
    }
    else
    {
        JobType = ECortexGenJobType::MeshFromText;
    }

    return SubmitGenJob(JobType, Params);
}

FCortexCommandResult FCortexGenCommandHandler::HandleStartImage(const TSharedPtr<FJsonObject>& Params)
{
    FString Prompt;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("start_image requires 'prompt'"));
    }

    return SubmitGenJob(ECortexGenJobType::ImageFromText, Params);
}

FCortexCommandResult FCortexGenCommandHandler::HandleStartTexturing(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceModelPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_model_path"), SourceModelPath) || SourceModelPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("start_texturing requires 'source_model_path'"));
    }

    return SubmitGenJob(ECortexGenJobType::Texturing, Params);
}

FCortexCommandResult FCortexGenCommandHandler::SubmitGenJob(ECortexGenJobType Type, const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    // Build request
    FCortexGenJobRequest Request;
    Request.Type = Type;

    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("prompt"), Request.Prompt);
        Params->TryGetStringField(TEXT("source_image_path"), Request.SourceImagePath);
        Params->TryGetStringField(TEXT("source_model_path"), Request.SourceModelPath);
        Params->TryGetStringField(TEXT("destination"), Request.Destination);
    }

    // Determine provider
    FString ProviderId;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("provider"), ProviderId);
    }
    if (ProviderId.IsEmpty())
    {
        const UCortexGenSettings* Settings = UCortexGenSettings::Get();
        ProviderId = Settings ? Settings->DefaultProvider : TEXT("meshy");
    }

    FString JobId;
    FString OutError;
    bool bSuccess = JobManager->SubmitJob(ProviderId, Request, JobId, OutError);

    if (!bSuccess)
    {
        // Map error text to error code heuristically
        FString ErrorCode = CortexErrorCodes::ProviderError;
        if (OutError.Contains(TEXT("not found")))
        {
            ErrorCode = CortexErrorCodes::ProviderNotFound;
        }
        else if (OutError.Contains(TEXT("does not support")))
        {
            ErrorCode = CortexErrorCodes::CapabilityNotSupported;
        }
        else if (OutError.Contains(TEXT("concurrent")))
        {
            ErrorCode = CortexErrorCodes::JobLimitReached;
        }
        return FCortexCommandRouter::Error(ErrorCode, OutError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), JobId);
    Data->SetStringField(TEXT("status"), TEXT("pending"));
    Data->SetStringField(TEXT("provider"), ProviderId);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleJobStatus(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    FString JobId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("job_status requires 'job_id'"));
    }

    const FCortexGenJobState* State = JobManager->GetJobState(JobId);
    if (!State)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::JobNotFound,
            FString::Printf(TEXT("Job '%s' not found"), *JobId));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), State->JobId);
    Data->SetStringField(TEXT("status"),
        StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(
            static_cast<int64>(State->Status)).ToLower());
    Data->SetStringField(TEXT("provider"), State->Provider);
    Data->SetNumberField(TEXT("progress"), State->Progress);
    Data->SetStringField(TEXT("prompt"), State->Prompt);
    Data->SetStringField(TEXT("created_at"), State->CreatedAt);
    Data->SetStringField(TEXT("completed_at"), State->CompletedAt);

    TArray<TSharedPtr<FJsonValue>> PathsArray;
    for (const FString& Path : State->ImportedAssetPaths)
    {
        PathsArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Data->SetArrayField(TEXT("asset_paths"), PathsArray);

    if (!State->ErrorMessage.IsEmpty())
    {
        Data->SetStringField(TEXT("error"), State->ErrorMessage);
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleListJobs(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    FString StatusFilter;
    int32 Limit = 0;

    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("status"), StatusFilter);
        double LimitVal = 0.0;
        if (Params->TryGetNumberField(TEXT("limit"), LimitVal))
        {
            Limit = static_cast<int32>(LimitVal);
        }
    }

    TArray<FCortexGenJobState> Jobs = JobManager->ListJobs(StatusFilter, Limit);

    TArray<TSharedPtr<FJsonValue>> JobsArray;
    for (const FCortexGenJobState& Job : Jobs)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("job_id"), Job.JobId);
        Obj->SetStringField(TEXT("status"),
            StaticEnum<ECortexGenJobStatus>()->GetNameStringByValue(
                static_cast<int64>(Job.Status)).ToLower());
        Obj->SetStringField(TEXT("provider"), Job.Provider);
        Obj->SetNumberField(TEXT("progress"), Job.Progress);
        Obj->SetStringField(TEXT("prompt"), Job.Prompt);
        Obj->SetStringField(TEXT("created_at"), Job.CreatedAt);
        Obj->SetStringField(TEXT("completed_at"), Job.CompletedAt);
        JobsArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("jobs"), JobsArray);
    Data->SetNumberField(TEXT("count"), Jobs.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleCancelJob(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    FString JobId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("cancel_job requires 'job_id'"));
    }

    FString OutError;
    if (!JobManager->CancelJob(JobId, OutError))
    {
        FString ErrorCode = OutError.Contains(TEXT("not found"))
            ? CortexErrorCodes::JobNotFound
            : CortexErrorCodes::InvalidOperation;
        return FCortexCommandRouter::Error(ErrorCode, OutError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), JobId);
    Data->SetStringField(TEXT("status"), TEXT("cancelled"));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleRetryImport(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    FString JobId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("retry_import requires 'job_id'"));
    }

    FString OutError;
    if (!JobManager->RetryImport(JobId, OutError))
    {
        FString ErrorCode = OutError.Contains(TEXT("not found"))
            ? CortexErrorCodes::JobNotFound
            : CortexErrorCodes::JobNotRetryable;
        return FCortexCommandRouter::Error(ErrorCode, OutError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), JobId);
    Data->SetStringField(TEXT("status"), TEXT("retrying"));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleListProviders(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    TArray<TSharedPtr<ICortexGenProvider>> Providers = JobManager->GetProviders();

    TArray<TSharedPtr<FJsonValue>> ProvidersArray;
    for (const TSharedPtr<ICortexGenProvider>& Provider : Providers)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("id"), Provider->GetProviderId().ToString());
        Obj->SetStringField(TEXT("name"), Provider->GetDisplayName().ToString());

        TArray<TSharedPtr<FJsonValue>> CapsArray;
        ECortexGenCapability Caps = Provider->GetCapabilities();
        if (EnumHasAnyFlags(Caps, ECortexGenCapability::MeshFromText))
        {
            CapsArray.Add(MakeShared<FJsonValueString>(TEXT("mesh_from_text")));
        }
        if (EnumHasAnyFlags(Caps, ECortexGenCapability::MeshFromImage))
        {
            CapsArray.Add(MakeShared<FJsonValueString>(TEXT("mesh_from_image")));
        }
        if (EnumHasAnyFlags(Caps, ECortexGenCapability::ImageFromText))
        {
            CapsArray.Add(MakeShared<FJsonValueString>(TEXT("image_from_text")));
        }
        if (EnumHasAnyFlags(Caps, ECortexGenCapability::Texturing))
        {
            CapsArray.Add(MakeShared<FJsonValueString>(TEXT("texturing")));
        }
        Obj->SetArrayField(TEXT("capabilities"), CapsArray);

        ProvidersArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("providers"), ProvidersArray);
    Data->SetNumberField(TEXT("count"), Providers.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleDeleteJob(const TSharedPtr<FJsonObject>& Params)
{
    check(JobManager.IsValid());

    FString JobId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("delete_job requires 'job_id'"));
    }

    FString OutError;
    if (!JobManager->DeleteJob(JobId, OutError))
    {
        FString ErrorCode = OutError.Contains(TEXT("not found"))
            ? CortexErrorCodes::JobNotFound
            : CortexErrorCodes::InvalidOperation;
        return FCortexCommandRouter::Error(ErrorCode, OutError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), JobId);
    Data->SetBoolField(TEXT("deleted"), true);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGenCommandHandler::HandleGetConfig(const TSharedPtr<FJsonObject>& Params)
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (Settings)
    {
        Data->SetStringField(TEXT("default_provider"), Settings->DefaultProvider);
        Data->SetStringField(TEXT("default_mesh_destination"), Settings->DefaultMeshDestination);
        Data->SetStringField(TEXT("default_texture_destination"), Settings->DefaultTextureDestination);
        Data->SetNumberField(TEXT("poll_interval_seconds"), Settings->PollIntervalSeconds);
        Data->SetNumberField(TEXT("max_concurrent_jobs"), Settings->MaxConcurrentJobs);
        Data->SetNumberField(TEXT("max_job_history"), Settings->MaxJobHistory);
    }
    else
    {
        Data->SetStringField(TEXT("default_provider"), TEXT("meshy"));
    }

    return FCortexCommandRouter::Success(Data);
}
