// Source/CortexCore/Public/CortexCoreDelegates.h
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Generic domain progress notification.
 * Domains fire this with their name and a JSON payload.
 * UI modules subscribe and filter by domain name.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(
    FOnCortexDomainProgress,
    const FName& /* DomainName */,
    const TSharedPtr<FJsonObject>& /* ProgressData */
);
