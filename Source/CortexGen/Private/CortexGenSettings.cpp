#include "CortexGenSettings.h"

void UCortexGenSettings::PostInitProperties()
{
    Super::PostInitProperties();

    if (!bModelRegistryInitialized && !HasAnyFlags(RF_ClassDefaultObject))
    {
        PopulateDefaultModelRegistry();
        bModelRegistryInitialized = true;
    }
}

void UCortexGenSettings::PopulateDefaultModelRegistry()
{
    // Image models
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/flux/schnell");
        M.DisplayName = TEXT("FLUX.1 Schnell (Fast)");
        M.Provider = TEXT("fal");
        M.Category = TEXT("image");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);
        M.MaxBatchSize = 4;
        M.PricingNote = TEXT("~$0.003/megapixel");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/flux/dev");
        M.DisplayName = TEXT("FLUX.1 Dev");
        M.Provider = TEXT("fal");
        M.Category = TEXT("image");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);
        M.MaxBatchSize = 4;
        M.PricingNote = TEXT("~$0.025/megapixel");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/flux-2-pro");
        M.DisplayName = TEXT("FLUX.2 Pro");
        M.Provider = TEXT("fal");
        M.Category = TEXT("image");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);
        M.MaxBatchSize = 1;
        M.PricingNote = TEXT("~$0.03/megapixel");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/fast-sdxl");
        M.DisplayName = TEXT("Stable Diffusion XL");
        M.Provider = TEXT("fal");
        M.Category = TEXT("image");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);
        M.MaxBatchSize = 4;
        M.PricingNote = TEXT("~$0.01/image");
        ModelRegistry.Add(M);
    }
    // Mesh models
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/hyper3d/rodin");
        M.DisplayName = TEXT("Hyper3D Rodin");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText)
            | static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage);
        M.PricingNote = TEXT("~$0.10/gen");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/meshy/v6/text-to-3d");
        M.DisplayName = TEXT("Meshy 6 (Text-to-3D)");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText);
        M.PricingNote = TEXT("~20-30 credits");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/hunyuan-3d/v3.1/rapid/text-to-3d");
        M.DisplayName = TEXT("Hunyuan3D 3.1 (Text)");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText);
        M.PricingNote = TEXT("~$0.225/gen");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/hunyuan-3d/v3.1/rapid/image-to-3d");
        M.DisplayName = TEXT("Hunyuan3D 3.1 (Image)");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage);
        M.PricingNote = TEXT("~$0.225/gen");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/trellis");
        M.DisplayName = TEXT("Trellis");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage);
        M.PricingNote = TEXT("TBD");
        ModelRegistry.Add(M);
    }
    {
        FCortexGenModelConfig M;
        M.ModelId = TEXT("fal-ai/triposr");
        M.DisplayName = TEXT("TripoSR");
        M.Provider = TEXT("fal");
        M.Category = TEXT("mesh");
        M.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage);
        M.PricingNote = TEXT("TBD");
        ModelRegistry.Add(M);
    }
}
