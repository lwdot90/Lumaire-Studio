#include "rendering/admitted_image.h"
#include <QColorSpace>
#include <memory>
#include <stdexcept>
#include <utility>

namespace compositor {
namespace {
struct ImageBacking {
    // Destruction frees the original pixels before releasing their charge.
    MemoryAdmission::Reservation memory;
    QImage image;
};
void releaseImage(void* backing) {delete static_cast<ImageBacking*>(backing);}
}
QImage admittedImage(QImage image,MemoryAdmission::Reservation memory) {
    if(image.isNull()) return {};
    if(image.format()!=QImage::Format_RGB32 || !image.isDetached() ||
       memory.bytes()!=static_cast<std::uint64_t>(image.sizeInBytes()))
        throw std::invalid_argument("Admitted image needs unique RGB32 backing and an exact pixel charge");
    auto owner=std::make_unique<ImageBacking>(ImageBacking{std::move(memory),std::move(image)});
    QImage shared(owner->image.bits(),owner->image.width(),owner->image.height(),owner->image.bytesPerLine(),
                  owner->image.format(),releaseImage,owner.get());
    if(shared.isNull()) throw std::bad_alloc();
    auto* backing=owner.release();
    shared.setDevicePixelRatio(backing->image.devicePixelRatio());
    shared.setDotsPerMeterX(backing->image.dotsPerMeterX());
    shared.setDotsPerMeterY(backing->image.dotsPerMeterY());
    shared.setOffset(backing->image.offset());
    shared.setColorSpace(backing->image.colorSpace());
    for(const auto& key:backing->image.textKeys()) shared.setText(key,backing->image.text(key));
    backing->memory.commit();
    return shared;
}
}
