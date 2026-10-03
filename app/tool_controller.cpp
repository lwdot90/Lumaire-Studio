#include "app/tool_controller.h"
#include "platform/canvas.h"
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <limits>

namespace compositor {
namespace {
QIcon toolIcon(int tool) {
    QPixmap pixels(48,48);pixels.setDevicePixelRatio(2);pixels.fill(Qt::transparent);
    QPainter painter(&pixels);painter.setRenderHint(QPainter::Antialiasing);
    const QColor ink("#dfe5ef");
    painter.setPen(QPen(ink,1.7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if(tool==0) {
        painter.drawLine(QPointF(9,15),QPointF(18,6));
        painter.drawLine(QPointF(11,17),QPointF(20,8));
        QPainterPath tip;tip.moveTo(9,15);tip.lineTo(12,18);tip.quadTo(9,22,3,21);tip.quadTo(6,20,6,18);tip.closeSubpath();
        painter.fillPath(tip,ink);
    } else if(tool==1) {
        QPainterPath edge;edge.moveTo(4,14);edge.lineTo(13,5);edge.lineTo(21,13);edge.lineTo(12,22);edge.closeSubpath();
        painter.drawPath(edge);painter.drawLine(QPointF(8,18),QPointF(17,9));
    } else if(tool==2) {
        painter.drawLine(QPointF(12,3),QPointF(12,21));painter.drawLine(QPointF(3,12),QPointF(21,12));
        for(int rotation:{0,90,180,270}) {
            painter.save();painter.translate(12,12);painter.rotate(rotation);
            painter.drawLine(QPointF(-3,-6),QPointF(0,-9));painter.drawLine(QPointF(3,-6),QPointF(0,-9));painter.restore();
        }
    } else if(tool==6) {
        painter.drawLine(QPointF(7,3),QPointF(7,17));painter.drawLine(QPointF(3,7),QPointF(17,7));
        painter.drawLine(QPointF(17,7),QPointF(17,21));painter.drawLine(QPointF(7,17),QPointF(21,17));
    } else {
        painter.setPen(QPen(ink,1.5,Qt::DashLine,Qt::RoundCap));
        if(tool==4) painter.drawEllipse(QRectF(4,4,16,16));else painter.drawRect(QRectF(4,4,16,16));
        if(tool==5) {
            painter.setPen(QPen(ink,1.7,Qt::SolidLine,Qt::RoundCap));
            painter.drawLine(QPointF(8,8),QPointF(16,16));painter.drawLine(QPointF(16,8),QPointF(8,16));
        }
    }
    painter.end();return QIcon(pixels);
}
QIcon colorSwatch(QColor color) {
    QPixmap pixels(40,40);pixels.setDevicePixelRatio(2);pixels.fill(Qt::transparent);
    QPainter painter(&pixels);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#a6b1c2"),1));painter.setBrush(color);painter.drawRect(QRectF(1,1,18,18));
    painter.end();return QIcon(pixels);
}
}
ToolController::ToolController(QWidget* parentWindow,Callbacks callbacks)
    :QObject(parentWindow),callbacks_(std::move(callbacks)),toolbar_(new QToolBar("Editing",parentWindow)) {
    toolbar_->setObjectName("editingToolbar");toolbar_->setAccessibleName("Editing tools");
    toolbar_->setAllowedAreas(Qt::LeftToolBarArea | Qt::RightToolBarArea);
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);toolbar_->setIconSize(QSize(20,20));
    toolbar_->setFixedWidth(36);toolbar_->setFloatable(false);toolbar_->setMovable(false);
    brushToolbar_=new QToolBar("Options",parentWindow);brushToolbar_->setObjectName("brushSettingsToolbar");
    brushToolbar_->setAccessibleName("Current tool options");brushToolbar_->setIconSize(QSize(18,18));
    brushToolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    brushToolbar_->setAllowedAreas(Qt::TopToolBarArea | Qt::BottomToolBarArea);brushToolbar_->setMovable(false);
    auto* group=new QActionGroup(this);group->setExclusive(true);
    const char* names[]{"Brush","Erase","Move","Rectangle selection","Ellipse selection"};
    const char* keys[]{"B","E","V","M",""};
    const char* objects[]{"toolBrush","toolErase","toolMove","toolRectangleSelection","toolEllipseSelection"};
    for(int i=0;i<5;++i) {
        auto* action=new QAction(toolIcon(i),names[i],toolbar_);action->setObjectName(objects[i]);action->setCheckable(true);
        action->setToolTip(*keys[i] ? QString("%1 (%2)").arg(names[i],keys[i]) : QString(names[i]));group->addAction(action);
        if(*keys[i]) action->setShortcut(QKeySequence(keys[i]));
        modeActions_.push_back(action);
        connect(action,&QAction::triggered,this,[this,i]{setMode(static_cast<Mode>(i));});
    }
    for(int index:{2,3,4,0,1}) toolbar_->addAction(modeActions_[index]);
    modeActions_[0]->setChecked(true);
    toolLabel_=new QLabel(brushToolbar_);toolLabel_->setObjectName("optionsToolLabel");brushToolbar_->addWidget(toolLabel_);
    brushToolbar_->addSeparator();
    const auto control=[&](const char* label,double low,double high,double value,double step) {
        auto* text=new QLabel(label,brushToolbar_);brushOptions_.push_back(brushToolbar_->addWidget(text));
        auto* spin=new QDoubleSpinBox(brushToolbar_);spin->setRange(low,high);spin->setValue(value);spin->setSingleStep(step);
        spin->setMaximumWidth(86);spin->setButtonSymbols(QAbstractSpinBox::NoButtons);text->setBuddy(spin);brushOptions_.push_back(brushToolbar_->addWidget(spin));return spin;
    };
    diameter_=control("Size",1,512,24,1);diameter_->setSuffix(" px");diameter_->setObjectName("brushDiameter");
    diameter_->setToolTip("Brush diameter in document pixels ([ smaller, ] larger)");
    diameter_->setDecimals(1);diameter_->setAccessibleName("Brush size in document pixels");
    hardness_=control("Hardness",0,100,80,5);hardness_->setSuffix("%");hardness_->setObjectName("brushHardness");
    hardness_->setDecimals(0);hardness_->setAccessibleName("Brush hardness percent");
    opacity_=control("Opacity",0,100,100,5);opacity_->setSuffix("%");opacity_->setObjectName("brushOpacity");
    opacity_->setDecimals(0);opacity_->setAccessibleName("Brush opacity percent");
    colorAction_=brushToolbar_->addAction("Color");colorAction_->setToolTip("Choose foreground color");brushOptions_.push_back(colorAction_);
    paintMask_=new QCheckBox("Paint mask",brushToolbar_);paintMask_->setObjectName("paintMask");
    paintMask_->setAccessibleName("Paint layer mask");paintMask_->setToolTip("Paint the layer mask instead of color pixels");
    brushOptions_.push_back(brushToolbar_->addWidget(paintMask_));
    moveHint_=brushToolbar_->addWidget(new QLabel("Drag to move layer",brushToolbar_));
    for(int index:{3,4}) {
        auto* button=new QToolButton(brushToolbar_);button->setDefaultAction(modeActions_[index]);button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        selectionOptions_.push_back(brushToolbar_->addWidget(button));
    }
    toolbar_->addSeparator();
    auto* swatches=new QWidget(toolbar_);swatches->setObjectName("colorSwatches");swatches->setFixedSize(32,40);
    backgroundSwatch_=new QToolButton(swatches);backgroundSwatch_->setObjectName("backgroundSwatch");
    backgroundSwatch_->setGeometry(10,12,22,22);backgroundSwatch_->setIconSize(QSize(20,20));backgroundSwatch_->setToolTip("Background color (X swaps, D resets)");
    foregroundSwatch_=new QToolButton(swatches);foregroundSwatch_->setObjectName("foregroundSwatch");
    foregroundSwatch_->setGeometry(0,2,22,22);foregroundSwatch_->setIconSize(QSize(20,20));foregroundSwatch_->setToolTip("Foreground color (X swaps, D resets)");
    foregroundSwatch_->setAccessibleName("Foreground color");backgroundSwatch_->setAccessibleName("Background color");
    swatchAction_=toolbar_->addWidget(swatches);
    const auto chooseForeground=[this,parentWindow] {
        const auto color=QColorDialog::getColor(foreground_,parentWindow,"Foreground color");
        if(color.isValid()) setForegroundColor(color);
    };
    connect(colorAction_,&QAction::triggered,this,chooseForeground);
    connect(foregroundSwatch_,&QToolButton::clicked,this,chooseForeground);
    connect(backgroundSwatch_,&QToolButton::clicked,this,[this,parentWindow] {
        const auto color=QColorDialog::getColor(background_,parentWindow,"Background color");
        if(color.isValid()) {cancel();background_=color;updateSwatches();}
    });
    const auto shortcut=[&](const char* name,const char* key,const auto& callback) {
        auto* action=new QAction(this);action->setObjectName(name);action->setShortcut(QKeySequence(key));parentWindow->addAction(action);
        connect(action,&QAction::triggered,this,callback);shortcutActions_.push_back(action);
    };
    shortcut("swapForegroundBackground","X",[this]{swapColors();});shortcut("defaultForegroundBackground","D",[this]{resetColors();});
    shortcut("decreaseBrushSize","[",[this]{if(mode_==Mode::Brush || mode_==Mode::Erase) {cancel();diameter_->setValue(diameter_->value()-1);}});
    shortcut("increaseBrushSize","]",[this]{if(mode_==Mode::Brush || mode_==Mode::Erase) {cancel();diameter_->setValue(diameter_->value()+1);}});
    connect(diameter_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{updateBrushCursor();});
    updateSwatches();updateOptions();setEnabled(false);
}
void ToolController::updateSwatches() {
    foregroundSwatch_->setIcon(colorSwatch(foreground_));backgroundSwatch_->setIcon(colorSwatch(background_));colorAction_->setIcon(colorSwatch(foreground_));
}
void ToolController::setForegroundChangedCallback(std::function<void(QColor)> callback) {foregroundChanged_=std::move(callback);}
void ToolController::setForegroundColor(QColor color) {
    if(!color.isValid()) return;
    color=color.toRgb();if(color==foreground_) return;
    cancel();foreground_=color;updateSwatches();
    if(foregroundChanged_) foregroundChanged_(foreground_);
}
void ToolController::swapColors() {
    const auto oldForeground=foreground_;const auto newForeground=background_;
    background_=oldForeground;setForegroundColor(newForeground);updateSwatches();
}
void ToolController::resetColors() {background_=Qt::white;setForegroundColor(Qt::black);updateSwatches();}
void ToolController::updateBrushCursor() {
    if(canvas_) canvas_->setBrushCursor(diameter_->value(),enabled_ && rasterAvailable_ && bool(document_) &&
        (mode_==Mode::Brush || mode_==Mode::Erase) && !space_ && !canvas_->spaceHeld());
}
void ToolController::updateOptions() {
    const bool ready=enabled_ && bool(document_);
    if(transformCommand_) transformCommand_->setEnabled(ready && rasterAvailable_);
    if(cropCommand_) cropCommand_->setEnabled(ready);
    if(deselectCommand_) deselectCommand_->setEnabled(ready && selection_.has_value());
    if(invertCommand_) invertCommand_->setEnabled(ready && selection_.has_value());
    const bool painting=mode_==Mode::Brush || mode_==Mode::Erase;
    const bool selection=mode_==Mode::RectangleSelection || mode_==Mode::EllipseSelection;
    const char* names[]{"Brush","Erase","Move","Rectangle selection","Ellipse selection"};toolLabel_->setText(names[static_cast<int>(mode_)]);
    const auto showOption=[this](QAction* action,bool visible) {
        action->setVisible(visible);
        // Qt applies toolbar layout changes later. Keep the actual option
        // widget in step with the tool immediately, including hidden toolbars.
        if(auto* widget=brushToolbar_->widgetForAction(action)) widget->setVisible(visible);
    };
    for(auto* action:brushOptions_) showOption(action,painting);
    showOption(moveHint_,mode_==Mode::Move);if(transformOption_) showOption(transformOption_,mode_==Mode::Move);
    for(auto* action:selectionOptions_) showOption(action,selection);
    for(std::size_t i=0;i<shortcutActions_.size();++i) shortcutActions_[i]->setEnabled(enabled_ && (i<2 || (painting && rasterAvailable_)));
    updateBrushCursor();
}
void ToolController::bindCommandActions(QAction* transform,QAction* crop,QAction* deselect,QAction* invert) {
    transformCommand_=transform;cropCommand_=crop;deselectCommand_=deselect;invertCommand_=invert;
    const auto proxy=[&](QAction* command,const char* name,const char* label) -> QAction* {
        if(!command) return nullptr;
        auto* action=brushToolbar_->addAction(label);action->setObjectName(name);action->setEnabled(command->isEnabled());
        connect(action,&QAction::triggered,command,[command]{command->trigger();});
        connect(command,&QAction::changed,action,[action,command]{action->setEnabled(command->isEnabled());});return action;
    };
    transformOption_=proxy(transform,"optionsTransform","Transform…");
    for(auto* action:{proxy(deselect,"optionsDeselect","Deselect"),proxy(invert,"optionsInvert","Invert")}) if(action) selectionOptions_.push_back(action);
    if(crop) {crop->setIcon(toolIcon(6));crop->setToolTip("Crop canvas…");toolbar_->insertAction(swatchAction_,crop);}
    updateOptions();
}
ToolController::~ToolController() {cancel();if(canvas_) {canvas_->setBrushCursor(diameter_->value(),false);canvas_->removeEventFilter(this);}}
void ToolController::attach(Canvas* canvas) {
    if(canvas_==canvas) return;
    cancel();if(canvas_) {canvas_->setBrushCursor(diameter_->value(),false);canvas_->removeEventFilter(this);}
    canvas_=canvas;space_=false;
    if(canvas_) canvas_->installEventFilter(this);
    updateBrushCursor();
}
void ToolController::setEnabled(bool enabled) {enabled_=enabled;toolbar_->setEnabled(enabled);brushToolbar_->setEnabled(enabled);if(!enabled) cancel();updateOptions();}
void ToolController::setRasterTargetAvailable(bool available) {
    rasterAvailable_=available;
    for(std::size_t i=0;i<3;++i) modeActions_[i]->setEnabled(available);
    diameter_->setEnabled(available);hardness_->setEnabled(available);opacity_->setEnabled(available);
    colorAction_->setEnabled(available);paintMask_->setEnabled(available);
    if(!available && (mode_==Mode::Brush || mode_==Mode::Erase || mode_==Mode::Move))
        setMode(Mode::RectangleSelection);
    updateOptions();
}
void ToolController::setDocument(engine::DocumentPtr document) {if(document_!=document) cancel();document_=std::move(document);updateOptions();}
void ToolController::setSelection(std::optional<engine::Selection> selection) {selection_=std::move(selection);updateOptions();}
void ToolController::setMaskContext(bool paintingMask) {paintMask_->setChecked(paintingMask);}
void ToolController::setMode(Mode mode) {
    if(!rasterAvailable_ && (mode==Mode::Brush || mode==Mode::Erase || mode==Mode::Move)) mode=Mode::RectangleSelection;
    cancel();mode_=mode;modeActions_.at(static_cast<std::size_t>(mode))->setChecked(true);updateOptions();
}
engine::Coordinate ToolController::position(QPointF logical) const {
    const auto& view=canvas_->viewport();
    const double x=std::floor(logical.x()*view.dpr),y=std::floor(logical.y()*view.dpr);
    if(!std::isfinite(x) || !std::isfinite(y) || x<std::numeric_limits<int>::min() || x>std::numeric_limits<int>::max() ||
       y<std::numeric_limits<int>::min() || y>std::numeric_limits<int>::max()) throw std::invalid_argument("Invalid pointer coordinate");
    const auto point=view.documentPixel(static_cast<int>(x),static_cast<int>(y));return {point.x,point.y};
}
void ToolController::error(QString message) {if(callbacks_.onError) callbacks_.onError(std::move(message));}
void ToolController::cancel() {
    if(canvas_) {canvas_->clearStrokePreview();if(dragging_) canvas_->setMouseGrabEnabled(false);}
    dragging_=false;rejected_=false;points_.clear();
}
void ToolController::append(engine::Coordinate point) {
    if(rejected_) return;
    if(!points_.empty() && points_.back().x==point.x && points_.back().y==point.y) return;
    if(points_.size()==8192) {
        rejected_=true;points_.clear();canvas_->clearStrokePreview();
        error("Stroke exceeds 8192 points. Release the pointer and draw a shorter stroke.");return;
    }
    points_.push_back(point);
    const engine::Pixel encoded{float(foreground_.redF()),float(foreground_.greenF()),float(foreground_.blueF()),float(opacity_->value()/100)};
    canvas_->setStrokePreview(points_,encoded,diameter_->value(),mode_==Mode::Erase);
}
engine::BrushSettings ToolController::settings() const {
    engine::BrushSettings brush;
    brush.diameter=diameter_->value();brush.hardness=hardness_->value()/100;brush.opacity=opacity_->value()/100;
    brush.color={float(foreground_.redF()),float(foreground_.greenF()),float(foreground_.blueF()),1};
    brush.erasing=mode_==Mode::Erase;brush.paintMask=paintMask_->isChecked();return brush;
}
void ToolController::release(engine::Coordinate point) {
    if(mode_==Mode::Brush || mode_==Mode::Erase) {
        append(point);const bool rejected=rejected_;auto stroke=std::move(points_);const auto brush=settings();cancel();
        if(!rejected && !stroke.empty() && callbacks_.onStroke) callbacks_.onStroke(std::move(stroke),brush);
    } else if(mode_==Mode::Move) {
        engine::TransformParameters transform;transform.dx=point.x-anchor_.x;transform.dy=point.y-anchor_.y;cancel();
        if((transform.dx!=0 || transform.dy!=0) && callbacks_.onMove) callbacks_.onMove(transform);
    } else {
        const double left=std::floor(std::min(anchor_.x,point.x)),top=std::floor(std::min(anchor_.y,point.y));
        const double right=std::floor(std::max(anchor_.x,point.x))+1,bottom=std::floor(std::max(anchor_.y,point.y))+1;
        const auto x=static_cast<std::int64_t>(std::clamp(left,0.,double(document_->width)));
        const auto y=static_cast<std::int64_t>(std::clamp(top,0.,double(document_->height)));
        const auto endX=static_cast<std::int64_t>(std::clamp(right,0.,double(document_->width)));
        const auto endY=static_cast<std::int64_t>(std::clamp(bottom,0.,double(document_->height)));
        const auto shape=mode_==Mode::EllipseSelection ? engine::SelectionShape::Ellipse : engine::SelectionShape::Rectangle;
        cancel();
        if(endX>x && endY>y) selection_=engine::Selection{{x,y,endX-x,endY-y},shape,false};
        else selection_.reset();
        updateOptions();if(callbacks_.onSelection) callbacks_.onSelection(selection_);
    }
}
bool ToolController::eventFilter(QObject* watched,QEvent* event) {
    if(watched!=canvas_) return false;
    if(event->type()==QEvent::FocusOut || event->type()==QEvent::Hide) {space_=false;cancel();canvas_->setBrushCursor(diameter_->value(),false);return false;}
    if(event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Space) {
            if(event->type()==QEvent::KeyPress) {space_=true;cancel();}
            else if(!key->isAutoRepeat()) space_=false;
            updateBrushCursor();return false;
        }
        if(key->key()==Qt::Key_Escape && event->type()==QEvent::KeyPress && dragging_) {cancel();return true;}
    }
    if(event->type()==QEvent::Wheel && dragging_) {cancel();return false;}
    if(!enabled_ || !document_ || !canvas_) return false;
    try {
        if(event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseButtonRelease) {
            updateBrushCursor();canvas_->setBrushCursorPosition(static_cast<QMouseEvent*>(event)->position());
        }
        if(event->type()==QEvent::MouseButtonPress) {
            auto* mouse=static_cast<QMouseEvent*>(event);
            if(mouse->button()!=Qt::LeftButton || space_ || canvas_->spaceHeld()) {
                if(mouse->button()==Qt::MiddleButton || space_ || canvas_->spaceHeld()) cancel();
                return false;
            }
            canvas_->activateCanvas();anchor_=position(mouse->position());dragging_=true;rejected_=false;points_.clear();
            canvas_->setMouseGrabEnabled(true);
            if(mode_==Mode::Brush || mode_==Mode::Erase) append(anchor_);
            mouse->accept();return true;
        }
        if(event->type()==QEvent::MouseMove && dragging_) {
            auto* mouse=static_cast<QMouseEvent*>(event);
            if(mode_==Mode::Brush || mode_==Mode::Erase) append(position(mouse->position()));
            mouse->accept();return true;
        }
        if(event->type()==QEvent::MouseButtonRelease && dragging_) {
            auto* mouse=static_cast<QMouseEvent*>(event);
            if(mouse->button()!=Qt::LeftButton) return false;
            release(position(mouse->position()));mouse->accept();return true;
        }
    } catch(const std::exception& failure) {cancel();error(QString::fromUtf8(failure.what()));return true;}
    return false;
}
}
