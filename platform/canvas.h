#pragma once
#include "vulkan/renderer.h"
#include "rendering/cpu_worker.h"
#include <QWindow>
#include <QBackingStore>
#include <QJsonObject>
#include <QVulkanInstance>

namespace compositor {
class Canvas final: public QWindow {
    Q_OBJECT
public:
    explicit Canvas(QVulkanInstance* instance,bool cpu,QString deviceName={},QWindow* parent=nullptr,
                    std::shared_ptr<RuntimeResources> resources=defaultRuntimeResources());
    ~Canvas() override;
    bool cpu() const { return cpu_; }
    bool spaceHeld() const { return space_; }
    quint64 frameCount() const { return frames_; }
    // GUI-thread observation of the current accepted CPU display render. Null
    // while a newer document/view render is pending. Shares
    // existing image backing; excludes selection/stroke/cursor overlays and does not
    // certify that a compositor has presented this image on screen.
    QImage captureCpuPresentation() const;
    const Viewport& viewport() const { return viewport_; }
    QString backendReason() const { return reason_; }
    void fit();
    void actualPixels();
    void forceCpu(QString reason);
    void restoreViewport(const Viewport& viewport,QString reason);
    void redraw();
    void activateCanvas();
    void setDocument(engine::DocumentPtr document,bool fit=false);
    void setStrokePreview(std::vector<engine::Coordinate> points,engine::Pixel encoded,double diameter,bool erasing);
    void clearStrokePreview();
    void setBrushCursor(double diameter,bool active);
    void setBrushCursorPosition(QPointF logical);
    bool brushCursorVisible() const {return cpu_ && document_ && !surfaceDying_ && brushCursorActive_ && showCursor_ && !space_ && !panning_;}
signals:
    void focusRequested();
    void diagnostic(QString message);
    void inputPosition(QPointF document,double zoom);
    void timing(QJsonObject event);
    void frameCompleted(quint64 count);
    void fallbackRequested(QString reason);
protected:
    bool eventFilter(QObject* watched,QEvent* event) override;
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
private:
    void requestFrame();
    void paintCpu();
    void pointer(QPointF position,const char* phase,std::int64_t receipt);
    QVulkanInstance* instance_;
    std::shared_ptr<RuntimeResources> resources_;
    bool cpu_,space_=false,panning_=false,showCursor_=false,initializedView_=false,surfaceDying_=false,exposed_=false,cpuDirty_=false,needsFrame_=true;
    Point cursor_,last_;
    Viewport viewport_;
    QString reason_;
    std::unique_ptr<VulkanRenderer> renderer_;
    std::unique_ptr<QBackingStore> backing_;
    engine::DocumentPtr document_;
    std::unique_ptr<CpuWorker> cpuWorker_;
    QImage cpuImage_;
    std::vector<engine::Coordinate> strokePreview_;
    engine::Pixel previewColor_;
    double previewDiameter_=1;
    bool previewErasing_=false,brushCursorActive_=false;
    double brushCursorDiameter_=1;
    std::uint64_t renderGeneration_=0,acceptedCpuGeneration_=0;
    quint64 frames_=0;
};
}
