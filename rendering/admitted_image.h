#pragma once
#include "core/memory_admission.h"
#include <QImage>

namespace compositor {
// Transfer a unique rendered RGB32 image and its exact pixel-byte reservation.
// The existing pixels are shared without copying; ordinary QImage copies retain
// the charge until their last backing reference is destroyed. A detached copy
// owns different pixels and needs separate admission before an allocating edit.
QImage admittedImage(QImage image,MemoryAdmission::Reservation memory);
}
