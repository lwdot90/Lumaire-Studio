#include "canvas.h"
#include "core/checkerboard.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPlatformSurfaceEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QLoggingCategory>
#include <string_view>

Q_LOGGING_CATEGORY(canvasLog,"compositor.canvas",QtWarningMsg)

namespace compositor {
Canvas::Canvas(QVulkanInstance* instance,bool cpu,QString deviceName,QWindow* parent,std::shared_ptr<RuntimeResources> resources)
    :QWindow(parent),instance_(instance),resources_(resources ? std::move(resources) : defaultRuntimeResources()),cpu_(cpu || !instance || !instance->isValid()) {
    setTitle("Canvas — Space-drag to pan; wheel to zoom");
    setObjectName("documentCanvas");
    if(cpu_) {
        setSurfaceType(QSurface::RasterSurface); backing_=std::make_unique<QBackingStore>(this);
        reason_=cpu ? "CPU canvas requested" : "Vulkan instance unavailable; CPU canvas";
    } else {
        setSurfaceType(QSurface::VulkanSurface); setVulkanInstance(instance_);
        reason_="Selecting Vulkan device";
        renderer_=std::make_unique<VulkanRenderer>(instance_->vkInstance(),
            [this](std::string message,bool fatal) {
                QMetaObject::invokeMethod(this,[this,message=QString::fromStdString(message),fatal]{
                    if(fatal) forceCpu(message); else { reason_=message; emit diagnostic(message); }
                },Qt::QueuedConnection);
            },
            [this](FrameTiming frame) {
                QMetaObject::invokeMethod(this,[this,frame]{
                    ++frames_; emit frameCompleted(frames_);
                    QJsonObject entry{{"event","vulkan_frame"},{"requested_ns",qint64(frame.requestedNs)},
                        {"submitted_ns",qint64(frame.submittedNs)},{"completion_observed_ns",qint64(frame.completedNs)},
                        {"gpu_duration_ns",frame.gpuNs<0 ? QJsonValue():QJsonValue(frame.gpuNs)},
                        {"presentation_feedback_ns",QJsonValue()}};
                    emit timing(entry);
                },Qt::QueuedConnection);
            },
            [this](bool before) {
                // Qt's renderer hooks perform platform synchronization. Window
                // lifetime is protected by detach/join before surface teardown.
                if(before) instance_->presentAboutToBeQueued(this); else instance_->presentQueued(this);
            },deviceName.toStdString(),resources_->limits,resources_->memory);
    }
    connect(this,&QWindow::screenChanged,this,[this](QScreen*){requestFrame();});
}
Canvas::~Canvas() { cpuWorker_.reset(); renderer_.reset(); }
QImage Canvas::captureCpuPresentation() const {
    return cpu_ && !surfaceDying_ && acceptedCpuGeneration_==renderGeneration_ ? cpuImage_ : QImage{};
}
void Canvas::setDocument(engine::DocumentPtr document,bool fitView) {
    document_=std::move(document);
    viewport_.documentWidth=document_->width; viewport_.documentHeight=document_->height;
    if(fitView) { viewport_.followsFit=true; viewport_.fit(); }
    if(!cpu_ && (document_->selection || std::any_of(document_->layers().begin(),document_->layers().end(),
       [](const auto& layer){return layer.mask && layer.maskEnabled;}))) {
        forceCpu("CPU canvas for masks and selection outlines");return;
    }
    if(!cpu_) { requestFrame();return; }
    if(!cpuWorker_) cpuWorker_=std::make_unique<CpuWorker>([this](std::uint64_t generation,QImage image,QString error) {
        QMetaObject::invokeMethod(this,[this,generation,image=std::move(image),error=std::move(error)]() mutable {
            if(generation!=renderGeneration_) return;
            if(!error.isEmpty()) { emit diagnostic(error); return; }
            cpuImage_=std::move(image); acceptedCpuGeneration_=generation; cpuDirty_=true; requestUpdate();
    },Qt::QueuedConnection);
    },resources_);
    requestFrame();
}
void Canvas::forceCpu(QString reason) {
    if(cpu_ || !renderer_) return;
    reason_=QString("CPU fallback: %1").arg(reason);
    surfaceDying_=true;
    renderer_.reset(); // Worker releases swapchain before native window changes.
    // The host replaces the QWindow and its container so the raster backend
    // starts with its own native surface and a single, unambiguous owner.
    emit diagnostic(reason_); emit fallbackRequested(reason_);
}
void Canvas::setStrokePreview(std::vector<engine::Coordinate> points,engine::Pixel encoded,double diameter,bool erasing) {
    if(points.size()>8192 || !std::isfinite(diameter) || diameter<=0 || diameter>2048)
        throw std::invalid_argument("Stroke preview exceeds bounds");
    for(const auto& point:points) if(!std::isfinite(point.x) || !std::isfinite(point.y))
        throw std::invalid_argument("Nonfinite stroke preview point");
    for(float value:{encoded.r,encoded.g,encoded.b,encoded.a}) if(!std::isfinite(value) || value<0 || value>1)
        throw std::invalid_argument("Invalid stroke preview color");
    strokePreview_=std::move(points);previewColor_=encoded;previewDiameter_=diameter;previewErasing_=erasing;
    if(!cpu_) {forceCpu("CPU canvas for editing preview");return;}
    cpuDirty_=true;requestUpdate();
}
void Canvas::clearStrokePreview() {
    if(strokePreview_.empty()) return;
    strokePreview_.clear();
    if(cpu_) {cpuDirty_=true;requestUpdate();}
}
void Canvas::setBrushCursor(double diameter,bool active) {
    if(!std::isfinite(diameter) || diameter<=0 || diameter>2048) throw std::invalid_argument("Invalid brush cursor diameter");
    if(brushCursorActive_==active && brushCursorDiameter_==diameter) return;
    brushCursorActive_=active;brushCursorDiameter_=diameter;
    if(cpu_) {cpuDirty_=true;requestUpdate();}
}
void Canvas::setBrushCursorPosition(QPointF logical) {
    if(!std::isfinite(logical.x()) || !std::isfinite(logical.y())) return;
    cursor_={logical.x(),logical.y()};showCursor_=true;
    if(cpu_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}
}
void Canvas::restoreViewport(const Viewport& viewport,QString reason) {
    viewport_=viewport; initializedView_=true; reason_=std::move(reason); requestFrame();
}
void Canvas::fit() { viewport_.resize(width(),height(),devicePixelRatio()); viewport_.fit(); initializedView_=true; requestFrame(); }
void Canvas::actualPixels() { viewport_.zoom=1; viewport_.followsFit=false; initializedView_=true; requestFrame(); }
void Canvas::redraw() { requestFrame(); }
void Canvas::activateCanvas() {
    // Embedded native windows also need their QWidget container in Qt's focus
    // chain; requesting compositor activation alone can focus only the parent.
    emit focusRequested();requestActivate();
}
bool Canvas::eventFilter(QObject*,QEvent* event) {
    // Some compositors retain native focus on the top-level QWidgetWindow.
    // Its focused canvas container still receives widget key events. Route
    // canvas-owned Space handling without intercepting application shortcuts.
    if(event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Space) {
            if(event->type()==QEvent::KeyPress) keyPressEvent(key);else keyReleaseEvent(key);
            return true;
        }
    } else if(event->type()==QEvent::FocusOut) {space_=false;panning_=false;}
    return false;
}
void Canvas::requestFrame() {
    qCDebug(canvasLog)<<"damage"<<size()<<isExposed()<<devicePixelRatio();
    // Resize/DPI delivery can precede the corresponding expose. Keep damage
    // until submission instead of discarding it while temporarily unexposed.
    needsFrame_=true;
    if(surfaceDying_ || !isExposed() || width()<=0 || height()<=0) return;
    needsFrame_=false;
    viewport_.resize(width(),height(),devicePixelRatio());
    if(!initializedView_) { viewport_.fit(); initializedView_=true; }
    if(cpu_) {
        if(document_ && cpuWorker_) cpuWorker_->request(++renderGeneration_,document_,viewport_,qRound(width()*viewport_.dpr),qRound(height()*viewport_.dpr));
        else { cpuDirty_=true; requestUpdate(); }
        return;
    }
    const auto surface=QVulkanInstance::surfaceForWindow(this);
    if(!surface) { forceCpu("Qt could not create a Vulkan surface"); return; }
    FrameRequest request;
    request.surface=surface; request.width=qRound(width()*devicePixelRatio()); request.height=qRound(height()*devicePixelRatio());
    request.view=viewport_; request.cursor=cursor_; request.showCursor=showCursor_; request.requestedNs=monotonicNs();
    request.document=document_;
    renderer_->request(request);
}
void Canvas::paintCpu() {
    if(!cpuDirty_ || !isExposed() || size().isEmpty()) return;
    cpuDirty_=false;
    const auto start=monotonicNs(); backing_->resize(size());
    const QRegion region(QRect(QPoint(0,0),size())); backing_->beginPaint(region);
    QPainter painter(backing_->paintDevice()); painter.fillRect(QRect(QPoint(0,0),size()),QColor::fromRgbF(.12F,.12F,.12F));
    painter.scale(1/viewport_.dpr,1/viewport_.dpr);
    if(document_ && !cpuImage_.isNull()) painter.drawImage(0,0,cpuImage_);
    else for(auto r:checkerboard(viewport_,qRound(width()*viewport_.dpr),qRound(height()*viewport_.dpr),cursor_,showCursor_))
        painter.fillRect(r.x,r.y,r.width,r.height,QColor::fromRgbF(r.value,r.value,r.value));
    if(document_) {
        const auto origin=viewport_.toDevice({0,0});
        const QRectF canvasBounds(origin.x,origin.y,document_->width*viewport_.zoom,document_->height*viewport_.zoom);
        painter.save();painter.setClipRect(canvasBounds);
        if(document_->selection) {
            const auto& selection=*document_->selection;
            QPainterPath outline;
            if(selection.bounds.width>0 && selection.bounds.height>0) {
                const auto point=viewport_.toDevice({static_cast<double>(selection.bounds.x),static_cast<double>(selection.bounds.y)});
                const QRectF bounds(point.x,point.y,static_cast<double>(selection.bounds.width)*viewport_.zoom,static_cast<double>(selection.bounds.height)*viewport_.zoom);
                if(selection.shape==engine::SelectionShape::Ellipse) outline.addEllipse(bounds);else outline.addRect(bounds);
            }
            if(selection.inverted) outline.addRect(canvasBounds.adjusted(1,1,-1,-1));
            painter.setBrush(Qt::NoBrush);painter.setPen(QPen(Qt::white,1));painter.drawPath(outline);
            painter.setPen(QPen(Qt::black,1,Qt::DashLine));painter.drawPath(outline);
        }
        if(!strokePreview_.empty()) {
            QPainterPath path;const auto first=viewport_.toDevice({strokePreview_.front().x,strokePreview_.front().y});
            path.moveTo(first.x,first.y);
            for(std::size_t i=1;i<strokePreview_.size();++i) {
                const auto point=viewport_.toDevice({strokePreview_[i].x,strokePreview_[i].y});path.lineTo(point.x,point.y);
            }
            const auto color=previewErasing_ ? QColor(255,255,255,120)
                : QColor::fromRgbF(previewColor_.r,previewColor_.g,previewColor_.b,std::min(.5f,previewColor_.a));
            painter.setRenderHint(QPainter::Antialiasing);painter.setBrush(color);
            const auto diameter=previewDiameter_*viewport_.zoom;
            painter.setPen(QPen(color,diameter,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            if(strokePreview_.size()==1) {painter.setPen(Qt::NoPen);painter.drawEllipse(QPointF(first.x,first.y),diameter/2,diameter/2);}
            else {painter.setBrush(Qt::NoBrush);painter.drawPath(path);}
        }
        painter.restore();
    }
    if(document_ && brushCursorVisible()) {
        const QPointF center(cursor_.x*viewport_.dpr,cursor_.y*viewport_.dpr);
        const auto radius=brushCursorDiameter_*viewport_.zoom/2;
        painter.setRenderHint(QPainter::Antialiasing);painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::black,3*viewport_.dpr));painter.drawEllipse(center,radius,radius);
        painter.setPen(QPen(Qt::white,viewport_.dpr));painter.drawEllipse(center,radius,radius);
    }
    painter.end(); backing_->endPaint(); backing_->flush(region);
    ++frames_; emit frameCompleted(frames_);
    emit timing(QJsonObject{{"event","cpu_frame"},{"start_ns",qint64(start)},{"end_ns",qint64(monotonicNs())},{"presentation_feedback_ns",QJsonValue()}});
}
bool Canvas::event(QEvent* event) {
    qCDebug(canvasLog)<<"event"<<event->type();
    if(event->type()==QEvent::PlatformSurface) {
        const auto* surface=static_cast<QPlatformSurfaceEvent*>(event);
        if(surface->surfaceEventType()==QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            surfaceDying_=true; exposed_=false; if(renderer_) renderer_->detach();
            if(cpuWorker_) {++renderGeneration_;cpuWorker_->suspend();cpuImage_={};}
        } else surfaceDying_=false;
    } else if(event->type()==QEvent::UpdateRequest) {
        // Wayland's presentation hook can generate UpdateRequest after each
        // present. GPU damage is submitted explicitly; do not feed it back.
        if(cpu_) paintCpu();
        return true;
    } else if(event->type()==QEvent::DevicePixelRatioChange) {
        requestFrame();
    } else if(event->type()==QEvent::Leave) {
        showCursor_=false;if(cpu_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}
        if(!document_) requestFrame();
    } else if(event->type()==QEvent::FocusOut) {
        space_=false;panning_=false;showCursor_=false;
        if(cpu_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}
    }
    return QWindow::event(event);
}
void Canvas::exposeEvent(QExposeEvent*) {
    const bool now=isExposed();
    if(now && (!exposed_ || needsFrame_)) {
        requestFrame();
        // A frame callback queued before hiding can stay throttled on Wayland.
        // Re-exposure must attach a fresh raster buffer without waiting on it.
        if(cpu_) paintCpu();
    }
    else if(!now && exposed_) {
        if(renderer_) renderer_->detach();
        if(cpuWorker_) {++renderGeneration_;cpuWorker_->suspend();cpuImage_={};}
    }
    exposed_=now;
}
void Canvas::resizeEvent(QResizeEvent*) { requestFrame(); }
void Canvas::pointer(QPointF position,const char* phase,std::int64_t receipt) {
    cursor_={position.x(),position.y()}; showCursor_=true;
    const auto document=viewport_.toDocument(cursor_);
    emit inputPosition(QPointF(document.x,document.y),viewport_.zoom);
    emit timing(QJsonObject{{"event","input"},{"phase",phase},{"receipt_ns",qint64(receipt)},
        {"logical_x",position.x()},{"logical_y",position.y()}, {"document_x",document.x},{"document_y",document.y},
        {"dpr",viewport_.dpr},{"zoom",viewport_.zoom}});
    // Hover overlays reuse the accepted image and never cancel a document render.
    if(cpu_ && document_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}
    if(!document_ || panning_ || std::string_view(phase)=="wheel") requestFrame();
}
void Canvas::mousePressEvent(QMouseEvent* e) {
    const auto receipt=monotonicNs();
    activateCanvas(); panning_=e->button()==Qt::MiddleButton || (space_ && e->button()==Qt::LeftButton);
    last_={e->position().x(),e->position().y()}; pointer(e->position(),"down",receipt); e->accept();
}
void Canvas::mouseMoveEvent(QMouseEvent* e) {
    const auto receipt=monotonicNs();
    Point now{e->position().x(),e->position().y()};
    if(panning_) viewport_.pan({now.x-last_.x,now.y-last_.y});
    last_=now;
    pointer(e->position(),"move",receipt); e->accept();
}
void Canvas::mouseReleaseEvent(QMouseEvent* e) { const auto receipt=monotonicNs(); panning_=false; pointer(e->position(),"up",receipt); e->accept(); }
void Canvas::wheelEvent(QWheelEvent* e) {
    const auto receipt=monotonicNs();
    double steps=e->angleDelta().y()/120.0;
    if(steps==0 && !e->pixelDelta().isNull()) steps=e->pixelDelta().y()/120.0;
    viewport_.zoomAt({e->position().x(),e->position().y()},std::pow(1.2,steps)); pointer(e->position(),"wheel",receipt); e->accept();
}
void Canvas::keyPressEvent(QKeyEvent* e) {
    if(e->key()==Qt::Key_Space) {
        space_=true;if(cpu_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}e->accept();
    }
    else QWindow::keyPressEvent(e);
}
void Canvas::keyReleaseEvent(QKeyEvent* e) {
    if(e->key()==Qt::Key_Space && !e->isAutoRepeat()) {
        space_=false;panning_=false;if(cpu_ && brushCursorActive_) {cpuDirty_=true;requestUpdate();}e->accept();
    }
    else QWindow::keyReleaseEvent(e);
}
}
