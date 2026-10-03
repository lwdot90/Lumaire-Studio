#include "app/tool_controller.h"
#include "platform/canvas.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QToolBar>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWidget>
#include <iostream>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
void mouse(Canvas& canvas,QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
    QMouseEvent event(type,point,point,button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
void press(Canvas& canvas,QPointF point) {mouse(canvas,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);}
void move(Canvas& canvas,QPointF point) {mouse(canvas,QEvent::MouseMove,point,Qt::NoButton,Qt::LeftButton);}
void release(Canvas& canvas,QPointF point) {mouse(canvas,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,1);
        auto runtime=std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};});
        auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,16,16});
        LayerNode layer{Id::generate()};layer.raster=source;
        auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),16,16,72,std::vector<LayerNode>{layer});
        QWidget window;Canvas canvas(nullptr,true,{},nullptr,runtime);
        canvas.setDocument(document);canvas.restoreViewport({16,16,1,1,{8,8},false,16,16},"controller fixture");
        std::vector<std::vector<Coordinate>> strokes;std::vector<BrushSettings> settings;
        std::vector<std::optional<Selection>> selections;std::vector<TransformParameters> moves;std::vector<QString> errors;
        ToolController::Callbacks callbacks;
        callbacks.onStroke=[&](auto points,auto brush){strokes.push_back(std::move(points));settings.push_back(brush);};
        callbacks.onSelection=[&](auto selection){selections.push_back(selection);};
        callbacks.onMove=[&](auto transform){moves.push_back(transform);};
        callbacks.onError=[&](auto message){errors.push_back(std::move(message));};
        ToolController controller(&window,std::move(callbacks));controller.attach(&canvas);controller.setDocument(document);controller.setEnabled(true);

        QAction transformCommand("Transform",&window),cropCommand("Crop",&window),deselectCommand("Deselect",&window),invertCommand("Invert",&window);
        int transforms=0,crops=0,deselects=0,inverts=0;
        QObject::connect(&transformCommand,&QAction::triggered,[&]{++transforms;});
        QObject::connect(&cropCommand,&QAction::triggered,[&]{++crops;});
        QObject::connect(&deselectCommand,&QAction::triggered,[&]{++deselects;});
        QObject::connect(&invertCommand,&QAction::triggered,[&]{++inverts;});
        transformCommand.setEnabled(false); // Simulate a stale unopened menu.
        controller.bindCommandActions(&transformCommand,&cropCommand,&deselectCommand,&invertCommand);
        auto* transformOption=controller.brushToolbar()->findChild<QAction*>("optionsTransform");
        auto* deselectOption=controller.brushToolbar()->findChild<QAction*>("optionsDeselect");
        auto* invertOption=controller.brushToolbar()->findChild<QAction*>("optionsInvert");
        auto* toolLabel=controller.brushToolbar()->findChild<QLabel*>("optionsToolLabel");
        auto* sizeControl=controller.brushToolbar()->findChild<QDoubleSpinBox*>("brushDiameter");
        expect(transformOption && deselectOption && invertOption && toolLabel && sizeControl,"Context options did not bind actual commands");
        controller.brushToolbar()->resize(1000,40);
        controller.setMode(ToolController::Mode::Move);
        QApplication::processEvents();
        expect(toolLabel->text()=="Move" && transformOption->isVisible() && !deselectOption->isVisible() &&
            !invertOption->isVisible() && sizeControl->isHidden(),"Move context displayed brush/selection controls");
        transformOption->trigger();expect(transforms==1,"Options Transform failed to invoke the real command");
        controller.setSelection(Selection{{0,0,2,2},SelectionShape::Rectangle,false});
        controller.setMode(ToolController::Mode::RectangleSelection);
        QApplication::processEvents();
        expect(deselectOption->isVisible() && invertOption->isVisible() && !transformOption->isVisible() && sizeControl->isHidden(),
            "Selection context displayed irrelevant controls or hid real selection commands");
        deselectOption->trigger();invertOption->trigger();cropCommand.trigger();
        expect(deselects==1 && inverts==1 && crops==1 && transformCommand.isVisible() && deselectCommand.isVisible(),
            "Context proxies changed menu command visibility or failed to forward activation");
        controller.setSelection({});
        expect(!deselectCommand.isEnabled() && !invertCommand.isEnabled(),"Cleared selection left command shortcuts enabled");
        controller.setMode(ToolController::Mode::Brush);
        QApplication::processEvents();
        expect(toolLabel->text()=="Brush" && !sizeControl->isHidden() && !transformOption->isVisible() && !deselectOption->isVisible(),
            "Brush context did not restore its actual brush settings");
        expect(controller.toolbar()->width()==36 && controller.toolbar()->actions().front()->objectName()=="toolMove",
            "Compact tool rail lost its width or Move-first ordering");
        std::vector<QColor> colorChanges;
        controller.setForegroundChangedCallback([&](QColor color){colorChanges.push_back(color);controller.setForegroundColor(color);});
        controller.setForegroundColor(Qt::black);expect(colorChanges.empty(),"Unchanged foreground emitted a feedback notification");
        controller.setForegroundColor(QColor("#d75568"));controller.setForegroundColor(QColor("#d75568"));controller.setForegroundColor(QColor{});
        expect(colorChanges.size()==1 && controller.foregroundColor()==QColor("#d75568"),"External foreground changes emitted duplicates or accepted invalid color");
        controller.resetColors();expect(colorChanges.size()==2,"Reset missed foreground change notification");
        controller.resetColors();expect(colorChanges.size()==2,"Repeated defaults emitted a foreground change");
        controller.swapColors();expect(controller.foregroundColor()==QColor(Qt::white) && controller.backgroundColor()==QColor(Qt::black),
            "Foreground/background swap did not change the brush color pair");
        controller.resetColors();expect(controller.foregroundColor()==QColor(Qt::black) && controller.backgroundColor()==QColor(Qt::white),
            "Default color command did not restore black/white");
        auto* swapShortcut=controller.findChild<QAction*>("swapForegroundBackground");
        auto* defaultsShortcut=controller.findChild<QAction*>("defaultForegroundBackground");
        expect(swapShortcut && defaultsShortcut && swapShortcut->shortcut()==QKeySequence("X") && defaultsShortcut->shortcut()==QKeySequence("D"),
            "Color pair shortcuts are missing");
        swapShortcut->trigger();expect(controller.foregroundColor()==QColor(Qt::white),"X command did not swap foreground");
        defaultsShortcut->trigger();
        press(canvas,{2.25,3.75});move(canvas,{4.25,3.75});
        expect(strokes.empty(),"Brush published before release");
        release(canvas,{5.25,3.75});
        expect(strokes.size()==1 && strokes[0].size()==3,"Released brush did not produce its full stroke");
        expect(strokes[0][0].x==2.5 && strokes[0][0].y==3.5,"Logical coordinates did not map to document pixel centers");
        controller.setMode(ToolController::Mode::Erase);controller.setMaskContext(true);
        press(canvas,{3,3});release(canvas,{3,3});
        expect(strokes.size()==2 && strokes.back().size()==1 && settings.back().erasing && settings.back().paintMask,
            "Erase/mask flags or duplicate point handling changed");

        controller.setMode(ToolController::Mode::RectangleSelection);
        press(canvas,{2,3});release(canvas,{5,7});
        expect(selections.size()==1 && selections.back() && selections.back()->bounds==Extent{2,3,4,5} &&
            selections.back()->shape==SelectionShape::Rectangle,"Rectangle selection lost inclusive pixel bounds");
        controller.setMode(ToolController::Mode::EllipseSelection);
        press(canvas,{2,3});release(canvas,{5,7});
        expect(selections.size()==2 && selections.back() && selections.back()->shape==SelectionShape::Ellipse,
            "Ellipse gesture did not select ellipse shape");
        controller.setMode(ToolController::Mode::Move);
        press(canvas,{4,4});release(canvas,{7,6});
        expect(moves.size()==1 && moves.back().dx==3 && moves.back().dy==2 && moves.back().scaleX==1 &&
            moves.back().rotationDegrees==0,"Move emitted incorrect document-space translation");

        controller.setMode(ToolController::Mode::Brush);
        QKeyEvent spaceDown(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier);QApplication::sendEvent(&canvas,&spaceDown);
        const auto center=canvas.viewport().center;
        press(canvas,{4,4});move(canvas,{7,6});release(canvas,{7,6});
        QKeyEvent spaceUp(QEvent::KeyRelease,Qt::Key_Space,Qt::NoModifier);QApplication::sendEvent(&canvas,&spaceUp);
        expect(strokes.size()==2 && canvas.viewport().center.x==center.x-3 && canvas.viewport().center.y==center.y-2,
            "Space-left gesture was captured as editing instead of Canvas panning");
        const auto middleCenter=canvas.viewport().center;
        mouse(canvas,QEvent::MouseButtonPress,{4,4},Qt::MiddleButton,Qt::MiddleButton);
        mouse(canvas,QEvent::MouseMove,{5,5},Qt::NoButton,Qt::MiddleButton);
        mouse(canvas,QEvent::MouseButtonRelease,{5,5},Qt::MiddleButton,Qt::NoButton);
        expect(strokes.size()==2 && canvas.viewport().center.x==middleCenter.x-1 && canvas.viewport().center.y==middleCenter.y-1,
            "Middle-button panning was captured as an editing gesture");
        canvas.restoreViewport({16,16,2,2,{8,8},false,16,16},"fractional coordinate fixture");
        press(canvas,{2.25,3.75});release(canvas,{2.25,3.75});
        expect(strokes.size()==3 && strokes.back()[0].x==2.25 && strokes.back()[0].y==3.75,
            "DPR pointer conversion changed the physical-pixel grid");
        canvas.restoreViewport({16,16,1,1,{8,8},false,16,16},"bounded stroke fixture");
        press(canvas,{0,0});
        for(int i=1;i<=8192;++i) move(canvas,{double(i%128),double(i/128)});
        release(canvas,{0,0});
        expect(strokes.size()==3 && errors.size()==1 && errors.front().contains("8192"),
            "Oversized stroke published a silently truncated command or rejected below the advertised point limit");
        press(canvas,{1,1});release(canvas,{1,1});
        expect(strokes.size()==4,"Rejected stroke prevented the next valid gesture");
        press(canvas,{2,2});controller.setEnabled(false);release(canvas,{3,3});
        expect(strokes.size()==4,"Disabling controller published an unfinished stroke");
        controller.setEnabled(true);controller.setMode(ToolController::Mode::Brush);
        auto* maskChoice=controller.brushToolbar()->findChild<QCheckBox*>("paintMask");
        auto* brushAction=controller.toolbar()->findChild<QAction*>("toolBrush");
        auto* rectangleAction=controller.toolbar()->findChild<QAction*>("toolRectangleSelection");
        expect(maskChoice && brushAction && rectangleAction,"Editing toolbar controls lost their public identifiers");
        controller.setMaskContext(true);controller.setRasterTargetAvailable(false);
        expect(controller.mode()==ToolController::Mode::RectangleSelection && !brushAction->isEnabled() &&
            rectangleAction->isEnabled() && !maskChoice->isEnabled() && maskChoice->isChecked() && !canvas.brushCursorVisible(),
            "No-raster target disabled selection or overwrote the explicit paint-mask choice");
        press(canvas,{1,1});release(canvas,{3,4});
        expect(selections.size()==3 && selections.back() && selections.back()->bounds==Extent{1,1,3,4},
            "Document selection stopped working without a raster target");
        controller.setMode(ToolController::Mode::Brush);
        expect(controller.mode()==ToolController::Mode::RectangleSelection,"Unavailable raster tool was activated programmatically");
        controller.setRasterTargetAvailable(true);controller.setMode(ToolController::Mode::Brush);
        expect(maskChoice->isEnabled() && maskChoice->isChecked(),"Restoring a raster target discarded mask preference");
        press(canvas,{1,1});controller.attach(&canvas);move(canvas,{2,1});release(canvas,{3,1});
        expect(strokes.size()==5 && strokes.back().size()==3 && settings.back().paintMask,
            "Same-canvas refresh discarded a live gesture or its explicit mask preference");
        mouse(canvas,QEvent::MouseMove,{5,5},Qt::NoButton,Qt::NoButton);
        expect(canvas.brushCursorVisible(),"Brush hover outline did not follow pointer movement");
        sizeControl->setValue(37);expect(canvas.brushCursorVisible(),"Size changes removed the active brush outline");
        controller.setMode(ToolController::Mode::Move);expect(!canvas.brushCursorVisible(),"Move mode retained a brush outline");
        controller.setMode(ToolController::Mode::Erase);mouse(canvas,QEvent::MouseMove,{5,5},Qt::NoButton,Qt::NoButton);
        expect(canvas.brushCursorVisible(),"Eraser hover outline did not follow pointer movement");
        QEvent leave(QEvent::Leave);QApplication::sendEvent(&canvas,&leave);
        expect(!canvas.brushCursorVisible(),"Leaving canvas retained a brush outline");
        controller.setMode(ToolController::Mode::Brush);
        mouse(canvas,QEvent::MouseMove,{5,5},Qt::NoButton,Qt::NoButton);
        press(canvas,{2,2});controller.setForegroundColor(QColor("#33aacc"));release(canvas,{3,3});
        expect(strokes.size()==5 && controller.foregroundColor()==QColor("#33aacc"),"External color change failed to cancel an unfinished stroke");
        controller.setForegroundChangedCallback({});
        // A closed tab may destroy Canvas before controller context is cleared.
        // The document remains valid until explicit no-document context arrives.
        {
            ToolController closed(&window,{});
            auto closingCanvas=std::make_unique<Canvas>(nullptr,true,QString{},nullptr,runtime);
            auto closingSource=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,4,4});
            std::weak_ptr<const RasterSnapshot> closingRaster=closingSource;
            LayerNode closingLayer{Id::generate()};closingLayer.raster=std::move(closingSource);
            std::vector<LayerNode> closingLayers;closingLayers.push_back(std::move(closingLayer));
            auto closingDocument=std::make_shared<const DocumentSnapshot>(Id::generate(),4,4,72,std::move(closingLayers));
            std::weak_ptr<const DocumentSnapshot> closingSnapshot=closingDocument;
            closed.attach(closingCanvas.get());closed.setDocument(closingDocument);closingCanvas->setDocument(closingDocument);
            closingDocument.reset();closingCanvas.reset();
            expect(!closingSnapshot.expired(),"Controller failed to preserve its live document context");
            closed.setEnabled(false);closed.attach(nullptr);closed.setDocument({});closed.setSelection({});
            expect(closingSnapshot.expired() && closingRaster.expired(),"Closed controller context retained document/raster storage");
        }
        expect(errors.size()==1,"Controller produced unexpected gesture errors");
        std::cout<<"stroke release, erase/mask, selection, move, Space panning, DPR, stroke bound, raster availability, mask preference, context release and cancellation passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
