#pragma once
#include "core/document.h"
#include "core/editing_types.h"
#include <QObject>
#include <QPointer>
#include <QColor>
#include <QPointF>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QToolBar;
class QWidget;
class QLabel;
class QToolButton;
namespace compositor {
class Canvas;
class ToolController final:public QObject {
public:
    enum class Mode {Brush,Erase,Move,RectangleSelection,EllipseSelection,Clone,Heal};
    struct Callbacks {
        std::function<void(std::vector<engine::Coordinate>,engine::BrushSettings)> onStroke;
        std::function<void(std::optional<engine::Selection>)> onSelection;
        std::function<void(engine::TransformParameters)> onMove;
        std::function<void(QString)> onError;
        std::function<void(std::vector<engine::Coordinate>,engine::BrushSettings,engine::Coordinate,bool,double)> onRetouch;
    };
    explicit ToolController(QWidget* parentWindow,Callbacks callbacks);
    ~ToolController() override;
    QToolBar* toolbar() const {return toolbar_;}
    QToolBar* brushToolbar() const {return brushToolbar_;}
    void bindCommandActions(QAction* transform,QAction* crop,QAction* deselect,QAction* invert);
    QColor foregroundColor() const {return foreground_;}
    void setForegroundColor(QColor color);
    void setForegroundChangedCallback(std::function<void(QColor)> callback);
    QColor backgroundColor() const {return background_;}
    void swapColors();
    void resetColors();
    void attach(Canvas* canvas);
    void setEnabled(bool enabled);
    void setRasterTargetAvailable(bool available);
    void setPaintTargetsAvailable(bool colorPixels,bool layerMask);
    void setRetouchTarget(std::optional<engine::Id> target);
    void setDocument(engine::DocumentPtr document);
    void setSelection(std::optional<engine::Selection> selection);
    void setMaskContext(bool paintingMask);
    void setMode(Mode mode);
    Mode mode() const {return mode_;}
protected:
    bool eventFilter(QObject* watched,QEvent* event) override;
private:
    engine::Coordinate position(QPointF logical) const;
    engine::BrushSettings settings() const;
    void append(engine::Coordinate point);
    void cancel();
    void release(engine::Coordinate point);
    void error(QString message);
    void updateOptions();
    void updateSwatches();
    void updateBrushCursor();
    bool paintTargetAvailable() const;
    Callbacks callbacks_;
    std::function<void(QColor)> foregroundChanged_;
    QPointer<Canvas> canvas_;
    QToolBar* toolbar_;
    QToolBar* brushToolbar_;
    QDoubleSpinBox *diameter_,*hardness_,*opacity_;
    QDoubleSpinBox* retouchRadius_;
    QLabel* retouchHintLabel_;
    QAction *retouchHint_,*retouchRadiusOption_,*retouchRadiusLabel_;
    QCheckBox* paintMask_;
    QComboBox* maskMode_;
    QAction* maskModeOption_;
    QAction* colorAction_;
    std::vector<QAction*> modeActions_,brushOptions_,selectionOptions_,shortcutActions_;
    QLabel* toolLabel_;
    QAction *moveHint_,*transformOption_=nullptr,*swatchAction_;
    QPointer<QAction> transformCommand_,cropCommand_,deselectCommand_,invertCommand_;
    QToolButton *foregroundSwatch_,*backgroundSwatch_;
    QColor foreground_{Qt::black},background_{Qt::white};
    engine::DocumentPtr document_;
    std::optional<engine::Selection> selection_;
    Mode mode_=Mode::Brush;
    bool rasterAvailable_=true;
    bool colorPixelsAvailable_=true,maskAvailable_=true;
    bool enabled_=false,space_=false,dragging_=false,rejected_=false;
    engine::Coordinate anchor_;
    std::optional<engine::Coordinate> sourceAnchor_;
    std::optional<engine::Id> retouchTarget_;
    std::vector<engine::Coordinate> points_;
};
}
