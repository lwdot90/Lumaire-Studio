#include "app/editing_menus.h"
#include "app/window.h"
#include "app/photo_adjustment_dialog.h"
#include "core/editor_commands.h"
#include "core/retouch.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QPainter>
#include <QPainterPath>
#include <QInputDialog>
#include <QMenuBar>
#include <QMenu>
#include <QSpinBox>
#include <algorithm>
#include <utility>

namespace compositor {
namespace {
QIcon adjustmentIcon(engine::AdjustmentKind kind) {
    QPixmap image(48,48);image.setDevicePixelRatio(2);image.fill(Qt::transparent);
    QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);
    const QColor ink(205,205,205);painter.setPen(QPen(ink,1.5));
    switch(kind) {
        case engine::AdjustmentKind::Exposure:
            painter.drawRoundedRect(QRectF(3,3,18,18),1,1);
            painter.drawLine(QPointF(3,21),QPointF(21,3));
            painter.drawLine(QPointF(6,8),QPointF(10,8));painter.drawLine(QPointF(8,6),QPointF(8,10));
            painter.drawLine(QPointF(14,16),QPointF(18,16));break;
        case engine::AdjustmentKind::Brightness:
            painter.drawEllipse(QPointF(12,12),4,4);
            for(int i=0;i<8;++i) {
                painter.save();painter.translate(12,12);painter.rotate(i*45);
                painter.drawLine(QPointF(0,-7),QPointF(0,-10));painter.restore();
            }
            break;
        case engine::AdjustmentKind::Contrast:
            painter.drawEllipse(QRectF(4,4,16,16));painter.save();
            painter.setClipRect(QRectF(4,4,8,16));painter.setBrush(ink);painter.drawEllipse(QRectF(4,4,16,16));painter.restore();break;
        case engine::AdjustmentKind::Saturation: {
            QPainterPath drop;drop.moveTo(12,3);drop.cubicTo(10,7,5,11,5,15);
            drop.cubicTo(5,24,19,24,19,15);drop.cubicTo(19,11,14,7,12,3);drop.closeSubpath();
            painter.drawPath(drop);painter.drawArc(QRectF(8,11,8,8),210*16,80*16);break;
        }
        case engine::AdjustmentKind::Levels:
            for(int i=0;i<6;++i) painter.drawLine(QPointF(4+i*3,20),QPointF(4+i*3,18-(i%3)*5));
            painter.drawLine(QPointF(3,21),QPointF(22,21));break;
        case engine::AdjustmentKind::Curves: {
            painter.drawRect(QRectF(3,3,18,18));
            QPainterPath curve;curve.moveTo(3,21);curve.cubicTo(15,21,9,3,21,3);painter.drawPath(curve);break;
        }
        case engine::AdjustmentKind::ColorBalance:
            painter.drawLine(QPointF(12,3),QPointF(12,21));painter.drawLine(QPointF(3,8),QPointF(21,8));
            painter.drawLine(QPointF(6,8),QPointF(3,16));painter.drawLine(QPointF(6,8),QPointF(9,16));
            painter.drawLine(QPointF(18,8),QPointF(15,16));painter.drawLine(QPointF(18,8),QPointF(21,16));
            painter.drawLine(QPointF(3,16),QPointF(9,16));painter.drawLine(QPointF(15,16),QPointF(21,16));break;
    }
    painter.end();return QIcon(image);
}
bool ready(MainWindow& window,bool raster=false) {
    const auto document=window.currentDocument();
    if(!document || window.currentBusy()) return false;
    const auto selected=window.activeLayer();
    return !raster || (selected && !document->layer(*selected).folder && document->layer(*selected).raster);
}
bool pixelReady(MainWindow& window) {
    return ready(window,true) && !window.currentDocument()->layer(*window.activeLayer()).adjustments && !window.currentDocument()->layer(*window.activeLayer()).retouch;
}
bool exposureReady(MainWindow& window) {
    if(!ready(window,true)) return false;
    const auto& stack=window.currentDocument()->layer(*window.activeLayer()).adjustments;
    return !stack || (stack->operations.size()==1 && stack->operations.front().kind==engine::AdjustmentKind::Exposure);
}
QAction* action(QMenu* menu,const QString& text,const char* name,const QKeySequence& shortcut={}) {
    auto* result=menu->addAction(text);result->setObjectName(QString::fromLatin1(name));
    if(!shortcut.isEmpty()) result->setShortcut(shortcut);
    return result;
}
QDoubleSpinBox* real(QFormLayout* form,const QString& label,const char* name,double value,double minimum,double maximum) {
    auto* field=new QDoubleSpinBox(form->parentWidget());field->setObjectName(QString::fromLatin1(name));
    field->setDecimals(3);field->setRange(minimum,maximum);field->setValue(value);field->setKeyboardTracking(false);
    form->addRow(label,field);return field;
}
QSpinBox* integer(QFormLayout* form,const QString& label,const char* name,int value,int minimum,int maximum) {
    auto* field=new QSpinBox(form->parentWidget());field->setObjectName(QString::fromLatin1(name));
    field->setRange(minimum,maximum);field->setValue(value);field->setKeyboardTracking(false);
    form->addRow(label,field);return field;
}
void buttons(QDialog& dialog,QFormLayout* form) {
    auto* controls=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    QObject::connect(controls,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    QObject::connect(controls,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    form->addRow(controls);
}
std::optional<engine::Extent> rectangle(MainWindow& window,const QString& title) {
    const auto document=window.currentDocument();if(!ready(window)) return {};
    const auto initial=document->selection ? document->selection->bounds : engine::Extent{0,0,document->width,document->height};
    QDialog dialog(&window);dialog.setWindowTitle(title);dialog.setObjectName("geometryBoundsDialog");
    auto* form=new QFormLayout(&dialog);
    auto* x=integer(form,"Left", "boundsLeft",static_cast<int>(initial.x),0,document->width-1);
    auto* y=integer(form,"Top", "boundsTop",static_cast<int>(initial.y),0,document->height-1);
    auto* width=integer(form,"Width", "boundsWidth",static_cast<int>(initial.width),1,document->width-x->value());
    auto* height=integer(form,"Height", "boundsHeight",static_cast<int>(initial.height),1,document->height-y->value());
    QObject::connect(x,&QSpinBox::valueChanged,width,[width,document](int value){width->setMaximum(document->width-value);});
    QObject::connect(y,&QSpinBox::valueChanged,height,[height,document](int value){height->setMaximum(document->height-value);});
    buttons(dialog,form);
    if(dialog.exec()!=QDialog::Accepted || document!=window.currentDocument() || !ready(window)) return {};
    return engine::Extent{x->value(),y->value(),width->value(),height->value()};
}
}
void installEditingMenus(MainWindow& window) {
    auto* image=window.menuBar()->addMenu("&Image");image->setObjectName("imageMenu");
    auto* transform=action(image,"Move, scale and rotate…","transformLayerAction",QKeySequence("Ctrl+T"));
    auto* crop=action(image,"Crop…","cropDocumentAction");
    auto* resize=action(image,"Canvas size…","resizeDocumentAction",QKeySequence("Ctrl+Alt+C"));
    QObject::connect(image,&QMenu::aboutToShow,&window,[&window,transform,crop,resize]{
        transform->setEnabled(ready(window,true));crop->setEnabled(ready(window));resize->setEnabled(ready(window));
    });
    QObject::connect(transform,&QAction::triggered,&window,[&window]{
        if(!ready(window,true)) return;
        const auto document=window.currentDocument();const auto selected=window.activeLayer();
        QDialog dialog(&window);dialog.setWindowTitle("Move, scale and rotate");dialog.setObjectName("layerTransformDialog");
        auto* form=new QFormLayout(&dialog);
        auto* dx=real(form,"Move X (pixels)","transformDx",0,-1000000,1000000);
        auto* dy=real(form,"Move Y (pixels)","transformDy",0,-1000000,1000000);
        auto* sx=real(form,"Scale X (%)","transformScaleX",100,.1,30000000);
        auto* sy=real(form,"Scale Y (%)","transformScaleY",100,.1,30000000);
        auto* rotation=real(form,"Clockwise rotation (degrees)","transformRotation",0,-36000,36000);
        buttons(dialog,form);
        if(dialog.exec()==QDialog::Accepted && document==window.currentDocument() && selected==window.activeLayer() && ready(window,true))
            window.transformCurrent({dx->value(),dy->value(),sx->value()/100,sy->value()/100,rotation->value()});
    });
    QObject::connect(crop,&QAction::triggered,&window,[&window]{if(const auto bounds=rectangle(window,"Crop canvas")) window.cropCurrent(*bounds);});
    QObject::connect(resize,&QAction::triggered,&window,[&window]{
        if(!ready(window)) return;
        const auto document=window.currentDocument();
        QDialog dialog(&window);dialog.setWindowTitle("Resize document");dialog.setObjectName("documentResizeDialog");
        auto* form=new QFormLayout(&dialog);
        auto* width=integer(form,"Width (pixels)","resizeWidth",document->width,1,30000);
        auto* height=integer(form,"Height (pixels)","resizeHeight",document->height,1,30000);
        height->setMaximum(std::min(30000,100000000/width->value()));
        QObject::connect(width,&QSpinBox::valueChanged,height,[height](int value){height->setMaximum(std::min(30000,100000000/value));});
        buttons(dialog,form);
        if(dialog.exec()==QDialog::Accepted && document==window.currentDocument() && ready(window)) window.resizeCurrent(width->value(),height->value());
    });

    auto* resample=action(image,"Image size…","resampleDocumentAction",QKeySequence("Ctrl+Alt+I"));
    QObject::connect(image,&QMenu::aboutToShow,&window,[&window,resample]{resample->setEnabled(ready(window));});
    QObject::connect(resample,&QAction::triggered,&window,[&window]{
        if(!ready(window)) return;
        const auto document=window.currentDocument();
        QDialog dialog(&window);dialog.setWindowTitle("Image size");dialog.setObjectName("imageResampleDialog");
        auto* form=new QFormLayout(&dialog);
        auto* width=integer(form,"Width (pixels)","resampleWidth",document->width,1,30000);
        auto* height=integer(form,"Height (pixels)","resampleHeight",document->height,1,30000);
        height->setMaximum(std::min(30000,100000000/width->value()));
        QObject::connect(width,&QSpinBox::valueChanged,height,[height](int value){height->setMaximum(std::min(30000,100000000/value));});
        auto* filter=new QComboBox(&dialog);filter->setObjectName("resampleFilter");filter->addItems({"Nearest neighbor","Bilinear","Lanczos"});filter->setCurrentIndex(2);form->addRow("Resampling",filter);
        buttons(dialog,form);
        if(dialog.exec()==QDialog::Accepted && document==window.currentDocument() && ready(window))
            window.resampleCurrent(width->value(),height->value(),static_cast<engine::Sampling>(filter->currentIndex()));
    });

    auto* retouchStrength=action(image,"Retouch strength…","retouchStrengthAction");
    QObject::connect(image,&QMenu::aboutToShow,&window,[&window,retouchStrength]{const auto document=window.currentDocument();const auto id=window.activeLayer();retouchStrength->setEnabled(ready(window,true) && id && document->layer(*id).retouch);});
    QObject::connect(retouchStrength,&QAction::triggered,&window,[&window]{
        if(!ready(window,true)) return;
        const auto document=window.currentDocument();const auto id=window.activeLayer();if(!id || !document->layer(*id).retouch) return;
        const auto& strokes=document->layer(*id).retouch->strokes;
        QDialog dialog(&window);dialog.setWindowTitle("Retouch strength");dialog.setObjectName("retouchStrengthDialog");auto* form=new QFormLayout(&dialog);
        auto* choice=new QComboBox(&dialog);choice->setObjectName("retouchStrokeChoice");
        for(std::size_t i=0;i<strokes.size();++i) choice->addItem(QString("%1 — %2").arg(i+1).arg(strokes[i].kind==engine::RetouchKind::Heal ? "Healing" : "Clone"));
        form->addRow("Stroke",choice);auto* strength=real(form,"Strength (%)","retouchStrength",strokes.front().opacity*100,0,100);
        QObject::connect(choice,&QComboBox::currentIndexChanged,strength,[strength,&strokes](int index){strength->setValue(strokes.at(static_cast<std::size_t>(index)).opacity*100);});buttons(dialog,form);
        if(dialog.exec()==QDialog::Accepted && document==window.currentDocument() && id==window.activeLayer() && ready(window,true)) window.reviseRetouchStrength(static_cast<std::size_t>(choice->currentIndex()),strength->value()/100);
    });

    auto* select=window.menuBar()->addMenu("&Select");select->setObjectName("selectionMenu");
    auto* rectangleSelection=action(select,"Rectangle…","rectangleSelectionAction");
    auto* ellipseSelection=action(select,"Ellipse…","ellipseSelectionAction");
    auto* all=action(select,"All","selectAllAction",QKeySequence::SelectAll);
    auto* clear=action(select,"Deselect","deselectAction",QKeySequence("Ctrl+D"));
    auto* invert=action(select,"Invert","invertSelectionAction",QKeySequence("Ctrl+Shift+I"));
    QObject::connect(select,&QMenu::aboutToShow,&window,[&window,rectangleSelection,ellipseSelection,all,clear,invert]{
        const bool enabled=ready(window);for(auto* item:{rectangleSelection,ellipseSelection,all}) item->setEnabled(enabled);
        const bool selected=enabled && window.currentDocument()->selection.has_value();clear->setEnabled(selected);invert->setEnabled(selected);
    });
    for(const auto [item,shape]:{std::pair{rectangleSelection,engine::SelectionShape::Rectangle},std::pair{ellipseSelection,engine::SelectionShape::Ellipse}})
        QObject::connect(item,&QAction::triggered,&window,[&window,shape]{
            if(const auto bounds=rectangle(window,shape==engine::SelectionShape::Rectangle ? "Rectangle selection" : "Ellipse selection"))
                window.setSelectionCurrent(engine::Selection{*bounds,shape,false});
        });
    QObject::connect(all,&QAction::triggered,&window,[&window]{if(ready(window)){const auto document=window.currentDocument();window.setSelectionCurrent(engine::Selection{{0,0,document->width,document->height}});}});
    QObject::connect(clear,&QAction::triggered,&window,[&window]{if(ready(window)) window.setSelectionCurrent({});});
    QObject::connect(invert,&QAction::triggered,&window,[&window]{if(ready(window) && window.currentDocument()->selection){auto selection=*window.currentDocument()->selection;selection.inverted=!selection.inverted;window.setSelectionCurrent(selection);}});

    auto* featherSelection=action(select,"Feather…","featherSelectionAction");
    QObject::connect(select,&QMenu::aboutToShow,&window,[&window,featherSelection]{featherSelection->setEnabled(ready(window) && window.currentDocument()->selection.has_value());});
    QObject::connect(featherSelection,&QAction::triggered,&window,[&window]{
        if(!ready(window) || !window.currentDocument()->selection) return;
        const auto document=window.currentDocument();auto selection=*document->selection;
        bool accepted=false;const auto radius=QInputDialog::getDouble(&window,"Feather selection","Radius (document pixels)",selection.featherRadius,0,256,2,&accepted);
        if(accepted && document==window.currentDocument() && ready(window)) {selection.featherRadius=radius;window.setSelectionCurrent(selection);}
    });

    auto* masks=window.menuBar()->addMenu("Layer &Mask");masks->setObjectName("layerMaskMenu");
    auto* reveal=action(masks,"Reveal all","createRevealMaskAction");
    auto* fromSelection=action(masks,"From selection","createSelectionMaskAction");
    auto* remove=action(masks,"Remove mask","removeLayerMaskAction");
    auto* enabled=action(masks,"Enable mask","enableLayerMaskAction");enabled->setCheckable(true);
    QObject::connect(masks,&QMenu::aboutToShow,&window,[&window,reveal,fromSelection,remove,enabled]{
        const bool raster=ready(window,true);const auto document=window.currentDocument();const auto id=window.activeLayer();
        const bool hasMask=raster && document->layer(*id).mask;
        reveal->setEnabled(raster);fromSelection->setEnabled(raster && document->selection.has_value());
        remove->setEnabled(hasMask);enabled->setEnabled(hasMask);enabled->setChecked(hasMask && document->layer(*id).maskEnabled);
    });
    auto* featherMask=action(masks,"Feather mask…","featherLayerMaskAction");
    QObject::connect(masks,&QMenu::aboutToShow,&window,[&window,featherMask]{const auto document=window.currentDocument();const auto id=window.activeLayer();featherMask->setEnabled(ready(window,true) && id && document->layer(*id).mask);});
    QObject::connect(featherMask,&QAction::triggered,&window,[&window]{
        if(!ready(window,true)) return;
        const auto document=window.currentDocument();const auto id=window.activeLayer();if(!id || !document->layer(*id).mask) return;
        bool accepted=false;const auto radius=QInputDialog::getDouble(&window,"Feather layer mask","Radius (source pixels)",4,0,32,2,&accepted);
        if(accepted && document==window.currentDocument() && id==window.activeLayer() && ready(window,true)) window.featherMaskCurrent(radius);
    });
    QObject::connect(reveal,&QAction::triggered,&window,[&window]{if(ready(window,true)) window.createMaskCurrent(false);});
    QObject::connect(fromSelection,&QAction::triggered,&window,[&window]{if(ready(window,true) && window.currentDocument()->selection) window.createMaskCurrent(true);});
    QObject::connect(remove,&QAction::triggered,&window,[&window]{if(ready(window,true)) window.removeMaskCurrent();});
    QObject::connect(enabled,&QAction::triggered,&window,[&window](bool value){if(ready(window,true)) window.enableMaskCurrent(value);});

    auto* adjust=window.menuBar()->addMenu("&Adjust");adjust->setObjectName("adjustmentMenu");
    auto* photoDock=new QDockWidget("Adjustments",&window);
    photoDock->setObjectName("photoEditingDock");
    auto* photoPanel=new QWidget(photoDock);
    photoPanel->setObjectName("photoEditingPanel");
    auto* photoLayout=new QVBoxLayout(photoPanel);
    photoLayout->setContentsMargins(8,6,8,6);photoLayout->setSpacing(4);

    auto* guidance=new QLabel("Adjust selected layer",photoPanel);guidance->setObjectName("mutedLabel");
    photoLayout->addWidget(guidance);
    auto* grid=new QGridLayout;grid->setContentsMargins(0,0,0,0);grid->setHorizontalSpacing(4);grid->setVerticalSpacing(4);
    photoLayout->addLayout(grid);
    photoDock->setWidget(photoPanel);
    window.addDockWidget(Qt::RightDockWidgetArea,photoDock);
    for(auto* menu:window.menuBar()->findChildren<QMenu*>()) if(menu->title()=="&View") menu->addAction(photoDock->toggleViewAction());
    struct AdjustmentMenu {const char* label;const char* name;const char* description;const char* valueLabel;engine::AdjustmentKind kind;double minimum,maximum,neutral;};
    int adjustmentIndex=0;
    for(const auto entry:{AdjustmentMenu{"Exposure…","exposureAdjustmentAction","Light","Exposure (stops)",engine::AdjustmentKind::Exposure,-8,8,0},
                         AdjustmentMenu{"Brightness…","brightnessAdjustmentAction","Tonal intensity","Brightness (0 unchanged)",engine::AdjustmentKind::Brightness,-1,1,0},
                         AdjustmentMenu{"Contrast…","contrastAdjustmentAction","Tonal separation","Contrast (0 unchanged)",engine::AdjustmentKind::Contrast,-.95,4,0},
                         AdjustmentMenu{"Saturation…","saturationAdjustmentAction","Color intensity","Saturation (1 unchanged)",engine::AdjustmentKind::Saturation,0,2,1},
                         AdjustmentMenu{"Levels…","levelsAdjustmentAction","Black point, midtones and white point","",engine::AdjustmentKind::Levels,0,0,0},
                         AdjustmentMenu{"Curves…","curvesAdjustmentAction","Edit the tonal response curve","",engine::AdjustmentKind::Curves,0,0,0},
                         AdjustmentMenu{"Color Balance…","colorBalanceAdjustmentAction","Warmth and green–magenta tint","",engine::AdjustmentKind::ColorBalance,0,0,0}}) {
        auto* item=action(adjust,QString::fromUtf8(entry.label),entry.name);
        item->setEnabled(pixelReady(window));
        item->setIcon(adjustmentIcon(entry.kind));
        auto* button=new QToolButton(photoPanel);button->setObjectName(QString::fromLatin1(entry.name)+"Button");
        button->setProperty("class","adjustmentButton");button->setProperty("role","adjustmentButton");
        button->setDefaultAction(item);button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);button->setIconSize({22,22});
        auto label=QString::fromUtf8(entry.label);label.remove(QChar(0x2026));button->setText(label);
        QObject::connect(item,&QAction::changed,button,[button,label]{button->setText(label);});
        button->setToolTip(QString::fromUtf8(entry.description));
        button->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
        grid->addWidget(button,adjustmentIndex/2,adjustmentIndex%2,Qt::AlignHCenter);++adjustmentIndex;
        const auto update=[&window,item]{item->setEnabled(pixelReady(window));};
        QObject::connect(adjust,&QMenu::aboutToShow,&window,update);
        QObject::connect(&window,&MainWindow::editingContextChanged,&window,update);
        QObject::connect(item,&QAction::triggered,&window,[&window,entry]{
            if(!pixelReady(window)) return;
            const auto document=window.currentDocument();const auto selected=window.activeLayer();
            if(entry.kind==engine::AdjustmentKind::Levels || entry.kind==engine::AdjustmentKind::Curves || entry.kind==engine::AdjustmentKind::ColorBalance) {
                PhotoAdjustmentDialog dialog(entry.kind,&window);
                if(dialog.exec()==QDialog::Accepted && document==window.currentDocument() && selected==window.activeLayer() && pixelReady(window))
                    window.applyAdjustment(dialog.parameters());
                return;
            }
            bool accepted=false;
            const auto value=QInputDialog::getDouble(&window,QString::fromUtf8(entry.label),QString::fromUtf8(entry.valueLabel),entry.neutral,entry.minimum,entry.maximum,3,&accepted);
            if(accepted && document==window.currentDocument() && selected==window.activeLayer() && pixelReady(window)) window.applyAdjustment({entry.kind,value});
        });
    }
    auto* revisableStack=action(adjust,"Revisable adjustments…","revisableAdjustmentsAction");
    revisableStack->setIcon(adjustmentIcon(engine::AdjustmentKind::Curves));
    revisableStack->setToolTip("Preview, reorder and revise retained photo adjustments after reopening");
    auto* stackButton=new QToolButton(photoPanel);stackButton->setObjectName("revisableAdjustmentsActionButton");
    stackButton->setDefaultAction(revisableStack);stackButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);stackButton->setIconSize({22,22});
    stackButton->setProperty("class","adjustmentButton");stackButton->setProperty("role","adjustmentButton");
    stackButton->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    photoLayout->insertWidget(1,stackButton);
    QObject::connect(revisableStack,&QAction::triggered,&window,&MainWindow::editRevisableAdjustments);
    auto* revisable=action(adjust,"Revisable Exposure…","revisableExposureAction");
    revisable->setIcon(adjustmentIcon(engine::AdjustmentKind::Exposure));
    auto* revisableButton=new QToolButton(photoPanel);revisableButton->setObjectName("revisableExposureActionButton");
    revisableButton->setDefaultAction(revisable);revisableButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);revisableButton->setIconSize({22,22});
    revisableButton->setProperty("class","adjustmentButton");revisableButton->setProperty("role","adjustmentButton");
    revisableButton->setText("Revisable\nExposure");revisableButton->setToolTip("Preview and retain whole-layer exposure for revision after reopening");
    QObject::connect(revisable,&QAction::changed,revisableButton,[revisableButton]{revisableButton->setText("Revisable\nExposure");});
    grid->addWidget(revisableButton,adjustmentIndex/2,adjustmentIndex%2,Qt::AlignHCenter);
    QObject::connect(revisable,&QAction::triggered,&window,&MainWindow::editRevisableExposure);
    adjust->addSeparator();
    auto* rasterize=action(adjust,"Rasterize retained edits","rasterizeAdjustmentsAction");
    rasterize->setToolTip("Keep displayed pixels and discard retained adjustment and retouch settings; undo restores them");
    QObject::connect(rasterize,&QAction::triggered,&window,&MainWindow::rasterizeCurrentAdjustments);
    const auto updateRevisable=[&window,revisableStack,revisable,rasterize] {
        revisableStack->setEnabled(ready(window,true));
        revisable->setEnabled(exposureReady(window));
        rasterize->setEnabled(ready(window,true) && bool(window.currentDocument()->layer(*window.activeLayer()).adjustments || window.currentDocument()->layer(*window.activeLayer()).retouch));
    };
    updateRevisable();QObject::connect(adjust,&QMenu::aboutToShow,&window,updateRevisable);
    QObject::connect(&window,&MainWindow::editingContextChanged,&window,updateRevisable);
    photoLayout->addStretch();
}
}
