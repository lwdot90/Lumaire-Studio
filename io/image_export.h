#pragma once
#include "io/project_store.h"

namespace compositor::io {
enum class ExportStage { Row, Encode, Replace };
struct ExportOptions {
    std::shared_ptr<MemoryAdmission> memory;
    std::stop_token stop;
    int quality=90; // JPEG 1..100.
    double resolution=0; // Zero preserves the document DPI; no image rescaling.
    std::optional<FileIdentity> expected; // Missing means destination must not exist.
    std::size_t mipBytes=8*1024*1024;
    std::function<void(ExportStage)> checkpoint; // Test failure/cancellation injection.
};
// Worker-owned streamed full-resolution flattened sRGB PNG8/JPEG8 export.
// PNG preserves straight alpha; JPEG composites over linear white.
void exportImage(const QString& path,const engine::DocumentPtr& document,const ExportOptions& options={});
}
