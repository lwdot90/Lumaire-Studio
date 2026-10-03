#pragma once
#include "core/document.h"
#include <QString>
#include <stop_token>

namespace compositor::io {
struct ImportedImage { engine::DocumentPtr document; QString information; };
ImportedImage importImage(const QString& path, engine::TileStore& tiles, std::stop_token stop={});
}
