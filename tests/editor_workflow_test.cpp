#include "app/window.h"
#include "io/image_export.h"
#include "core/editing_types.h"
#include "io/spill_store.h"
#include <QApplication>
#include <QAction>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QDockWidget>
#include <QInputDialog>
#include <QToolButton>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidgetItemIterator>
#include <QPainter>
#include <QRegion>
#include <QDir>
#include <QImageReader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;
namespace {
QString buildDirectory;
QByteArray canonicalRaster(const engine::RasterSnapshot& raster) {
    QByteArray result;
    for(auto y=raster.extent.y;y<raster.extent.y+raster.extent.height;++y) for(auto x=raster.extent.x;x<raster.extent.x+raster.extent.width;++x) {
        const auto packed=engine::pack(raster.pixel(x,y));
        for(const auto sample:packed) {
            result.append(static_cast<char>(sample&255));
            result.append(static_cast<char>(sample>>8));
        }
    }
    return result;
}
QByteArray canonicalRaster(const engine::DocumentPtr& document) {return canonicalRaster(*document->singleLayer().raster);}
std::shared_ptr<RuntimeResources> resourcesIn(const QString& directory) {
    io::SpillLimits storageLimits;
    storageLimits.maxBytes=64*1024*1024;storageLimits.maxPayloadBytes=2*1024*1024;
    storageLimits.maxEntries=256;storageLimits.maxIoOperations=1;
    auto spill=std::make_shared<io::SpillStore>(directory.toStdString(),storageLimits);
    auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,2);
    return std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};},spill);
}
}

class EditorWorkflowTest:public QObject {
    Q_OBJECT
private slots:
    void importSaveReopenAndExport() {
        QTemporaryDir directory(QDir(buildDirectory).filePath("editor-delivery-XXXXXX"));
        QVERIFY(directory.isValid());
        QImage input(64,48,QImage::Format_ARGB32);
        input.fill(qRgba(0,0,0,0));
        for(int y=8;y<40;++y) for(int x=8;x<56;++x) input.setPixel(x,y,qRgba(180,50,30,128));
        input.setPixel(20,20,qRgba(0,255,0,255));
        const auto inputPath=directory.filePath("input.png");QVERIFY(input.save(inputPath));
        auto resources=resourcesIn(directory.path());
        MainWindow window(nullptr,true,{},resources);window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(inputPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
        QCOMPARE(window.currentDocument()->width,64);QCOMPARE(window.currentDocument()->height,48);
        const auto imported=window.currentDocument();const auto original=canonicalRaster(imported);
        auto* diagnosticsDock=window.findChild<QDockWidget*>("diagnosticsDock");QVERIFY(diagnosticsDock);
        QVERIFY(!diagnosticsDock->isVisible());
        auto* toolRail=window.findChild<QToolBar*>("editingToolbar");QVERIFY(toolRail);
        QVERIFY(toolRail->isVisible());QCOMPARE(window.toolBarArea(toolRail),Qt::LeftToolBarArea);
        QCOMPARE(toolRail->orientation(),Qt::Vertical);
        auto* initialBrushSettings=window.findChild<QToolBar*>("brushSettingsToolbar");QVERIFY(initialBrushSettings);
        QVERIFY(initialBrushSettings->isVisible());QCOMPARE(initialBrushSettings->orientation(),Qt::Horizontal);
        QCOMPARE(window.toolBarArea(initialBrushSettings),Qt::TopToolBarArea);
        auto* initialMoveAction=window.findChild<QAction*>("toolMove");QVERIFY(initialMoveAction);
        QVERIFY(initialMoveAction->isChecked());
        auto* initialDiameter=window.findChild<QDoubleSpinBox*>("brushDiameter");QVERIFY(initialDiameter);
        auto* initialHardness=window.findChild<QDoubleSpinBox*>("brushHardness");QVERIFY(initialHardness);
        auto* initialOpacity=window.findChild<QDoubleSpinBox*>("brushOpacity");QVERIFY(initialOpacity);
        QVERIFY(!initialDiameter->isVisible());QVERIFY(!initialHardness->isVisible());QVERIFY(!initialOpacity->isVisible());
        auto* optionsTransform=window.findChild<QAction*>("optionsTransform");QVERIFY(optionsTransform);
        QVERIFY(optionsTransform->isVisible());
        auto* optionsDeselect=window.findChild<QAction*>("optionsDeselect");QVERIFY(optionsDeselect);
        auto* optionsInvert=window.findChild<QAction*>("optionsInvert");QVERIFY(optionsInvert);
        QVERIFY(!optionsDeselect->isVisible());QVERIFY(!optionsInvert->isVisible());


        auto* layersDock=window.findChild<QDockWidget*>("layersDock");QVERIFY(layersDock);
        QVERIFY(layersDock->isVisible());QCOMPARE(window.dockWidgetArea(layersDock),Qt::RightDockWidgetArea);
        QVERIFY(layersDock->width()<window.width()*0.4);
        QVERIFY(window.currentCanvas()->width()>window.width()*0.5);
        auto* colorDock=window.findChild<QDockWidget*>("colorDock");QVERIFY(colorDock);
        QVERIFY(colorDock->isVisible());QCOMPARE(window.dockWidgetArea(colorDock),Qt::RightDockWidgetArea);
        auto* colorHex=window.findChild<QLineEdit*>("colorHexEdit");QVERIFY(colorHex);
        QTRY_VERIFY_WITH_TIMEOUT(!colorHex->visibleRegion().isEmpty(),1000);
        auto* initialPhotoDock=window.findChild<QDockWidget*>("photoEditingDock");QVERIFY(initialPhotoDock);
        QCOMPARE(window.dockWidgetArea(initialPhotoDock),Qt::RightDockWidgetArea);


        // Select existing folder/raster rows: target-only changes must update
        // editing controls without changing the immutable document.
        const auto rasterId=*window.activeLayer();window.addLayer(true);
        const auto folderId=*window.activeLayer();const auto withFolder=window.currentDocument();
        auto* layerTree=window.findChild<QTreeWidget*>("layerTree");QVERIFY(layerTree);
        const auto itemFor=[&](const engine::Id& id) -> QTreeWidgetItem* {
            for(QTreeWidgetItemIterator item(layerTree);*item;++item)
                if((*item)->data(0,Qt::UserRole).toString()==QString::fromStdString(id.text())) return *item;
            return nullptr;
        };
        auto* rasterItem=itemFor(rasterId);auto* folderItem=itemFor(folderId);QVERIFY(rasterItem);QVERIFY(folderItem);
        auto* targetBrush=window.findChild<QAction*>("toolBrush");QVERIFY(targetBrush);
        auto* targetErase=window.findChild<QAction*>("toolErase");QVERIFY(targetErase);
        auto* targetMove=window.findChild<QAction*>("toolMove");QVERIFY(targetMove);
        auto* targetExposure=window.findChild<QAction*>("exposureAdjustmentAction");QVERIFY(targetExposure);
        auto* targetTransform=window.findChild<QAction*>("transformLayerAction");QVERIFY(targetTransform);
        layerTree->setCurrentItem(rasterItem);
        QVERIFY(window.activeLayer()==rasterId);QVERIFY(targetBrush->isEnabled());QVERIFY(targetErase->isEnabled());
        QVERIFY(targetMove->isEnabled());QVERIFY(targetExposure->isEnabled());QVERIFY(targetTransform->isEnabled());
        layerTree->setCurrentItem(folderItem);
        QVERIFY(window.activeLayer()==folderId);QVERIFY(!targetBrush->isEnabled());QVERIFY(!targetErase->isEnabled());
        QVERIFY(!targetMove->isEnabled());QVERIFY(!targetExposure->isEnabled());QVERIFY(!targetTransform->isEnabled());
        layerTree->setCurrentItem(rasterItem);
        QVERIFY(targetBrush->isEnabled());QVERIFY(targetErase->isEnabled());QVERIFY(targetMove->isEnabled());
        QVERIFY(targetExposure->isEnabled());QVERIFY(targetTransform->isEnabled());
        QVERIFY(window.currentDocument()==withFolder);window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        initialMoveAction->trigger();
        QVERIFY(optionsTransform->isVisible());QVERIFY(optionsTransform->isEnabled());

        qsizetype operations=finished.count();
#define COMPLETE_OPERATION(command) do { \
    ++operations;command; \
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000); \
    QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString())); \
    QVERIFY(!window.currentBusy()); \
} while(false)
        auto* photoDock=window.findChild<QDockWidget*>("photoEditingDock");QVERIFY(photoDock);
        photoDock->show();photoDock->raise();
        QTRY_VERIFY_WITH_TIMEOUT(photoDock->isVisible(),1000);
        for(const auto* name:{"exposureAdjustmentActionButton","brightnessAdjustmentActionButton",
                             "contrastAdjustmentActionButton","saturationAdjustmentActionButton"}) {
            auto* button=photoDock->findChild<QToolButton*>(name);QVERIFY(button);
            QTRY_VERIFY_WITH_TIMEOUT(QRegion(button->rect()).subtracted(button->visibleRegion()).isEmpty(),1000);
        }

        auto* exposureAction=window.findChild<QAction*>("exposureAdjustmentAction");QVERIFY(exposureAction);
        QToolButton* exposureButton=nullptr;
        for(auto* button:photoDock->findChildren<QToolButton*>())
            if(button->defaultAction()==exposureAction) exposureButton=button;
        QVERIFY(exposureButton);QVERIFY(exposureButton->isEnabled());
        QVERIFY(!exposureButton->visibleRegion().isEmpty());
        bool acceptedExposureDialog=false;
        QTimer::singleShot(0,&window,[&] {
            if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setDoubleValue(1);acceptedExposureDialog=true;dialog->accept();
            } else if(auto* otherDialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) otherDialog->reject();
        });
        ++operations;
        QTest::mouseClick(exposureButton,Qt::LeftButton);
        QVERIFY(acceptedExposureDialog);QVERIFY(window.currentBusy());
        QVERIFY(!exposureButton->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000);
        QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
        QVERIFY(!window.currentBusy());QVERIFY(exposureButton->isEnabled());
        QVERIFY(window.currentDocument()!=imported);
        QVERIFY(canonicalRaster(window.currentDocument())!=original);
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        QCOMPARE(canonicalRaster(window.currentDocument()),original);

        // Drive the same toolbar and canvas gesture used by a person, rather
        // than relying exclusively on command-method invocation.
        auto* brushAction=window.findChild<QAction*>("toolBrush");QVERIFY(brushAction);
        brushAction->trigger();
        auto* diameter=window.findChild<QDoubleSpinBox*>("brushDiameter");QVERIFY(diameter);
        auto* hardness=window.findChild<QDoubleSpinBox*>("brushHardness");QVERIFY(hardness);
        auto* brushSettings=window.findChild<QToolBar*>("brushSettingsToolbar");QVERIFY(brushSettings);
        QVERIFY(brushSettings->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!diameter->visibleRegion().isEmpty(),1000);
        QTRY_VERIFY_WITH_TIMEOUT(!hardness->visibleRegion().isEmpty(),1000);
        auto* opacity=window.findChild<QDoubleSpinBox*>("brushOpacity");QVERIFY(opacity);
        QTRY_VERIFY_WITH_TIMEOUT(!opacity->visibleRegion().isEmpty(),1000);
        auto* colorButton=window.findChild<QToolButton*>("foregroundSwatch");QVERIFY(colorButton);
        QTRY_VERIFY_WITH_TIMEOUT(!colorButton->visibleRegion().isEmpty(),1000);
        QVERIFY(!optionsTransform->isVisible());QVERIFY(!optionsDeselect->isVisible());QVERIFY(!optionsInvert->isVisible());
        diameter->setValue(8);hardness->setValue(100);
        colorDock->show();colorDock->raise();
        QTRY_VERIFY_WITH_TIMEOUT(!colorHex->visibleRegion().isEmpty(),1000);
        colorHex->setFocus();colorHex->selectAll();QTest::keyClicks(colorHex,"#d75568");
        QTest::keyClick(colorHex,Qt::Key_Return);
        QCOMPARE(colorHex->text().toLower(),QString("#d75568"));
        QVERIFY(window.currentDocument()==imported);QCOMPARE(finished.count(),operations);

        auto* canvas=window.currentCanvas();QVERIFY(canvas);QVERIFY(canvas->cpu());
        const auto previousFrame=canvas->frameCount();canvas->fit();
        QTRY_VERIFY_WITH_TIMEOUT(canvas->frameCount()>previousFrame,10000);
        const auto screenshot=qEnvironmentVariable("COMPOSITOR_UI_SCREENSHOT");
        if(!screenshot.isEmpty()) {
            QVERIFY2(QDir::isAbsolutePath(screenshot),"COMPOSITOR_UI_SCREENSHOT must be an absolute path");
            // Cursor/overlay paints also advance frameCount. Wait for actual
            // accepted CPU image backing before taking the renderer capture.
            QTRY_VERIFY_WITH_TIMEOUT(!canvas->captureCpuPresentation().isNull(),10000);
            const auto image=canvas->captureCpuPresentation();QVERIFY(!image.isNull());
            // QWidget::grab omits the native QWindow surface. Composite the
            // shared CPU presentation into the widgets: this is renderer-plus-
            // widget capture, not a desktop screenshot.
            auto capture=window.grab();QVERIFY(!capture.isNull());
            const auto origin=window.mapFromGlobal(canvas->mapToGlobal(QPoint(0,0)));
            QPainter painter(&capture);QVERIFY(painter.isActive());
            painter.drawImage(QRect(origin,canvas->size()),image);painter.end();
            QVERIFY2(capture.save(screenshot),qPrintable("Cannot save UI screenshot: "+screenshot));
        }
        const auto start=canvas->viewport().toLogical({24,24});
        const auto end=canvas->viewport().toLogical({34,24});
        const QPoint startPoint(qRound(start.x),qRound(start.y)),endPoint(qRound(end.x),qRound(end.y));
        ++operations;
        QTest::mousePress(canvas,Qt::LeftButton,{},startPoint);
        QTest::mouseMove(canvas,endPoint,1);
        QTest::mouseRelease(canvas,Qt::LeftButton,{},endPoint);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),operations,10000);
        QVERIFY2(finished.last()[0].toBool(),qPrintable(finished.last()[1].toString()));
        QVERIFY(window.currentDocument()!=imported);QVERIFY(canonicalRaster(window.currentDocument())!=original);
        const auto pointerColor=window.currentDocument()->singleLayer().raster->pixel(29,24);
        QVERIFY(pointerColor.a>.99f);QVERIFY(pointerColor.r>pointerColor.b && pointerColor.b>pointerColor.g);
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        auto* moveAction=window.findChild<QAction*>("toolMove");QVERIFY(moveAction);moveAction->trigger();
        QTRY_VERIFY(!diameter->isVisible());QTRY_VERIFY(!hardness->isVisible());QTRY_VERIFY(!opacity->isVisible());
        auto* selectionTool=window.findChild<QAction*>("toolRectangleSelection");QVERIFY(selectionTool);
        selectionTool->trigger();QVERIFY(selectionTool->isChecked());
        QVERIFY(!optionsTransform->isVisible());QVERIFY(optionsDeselect->isVisible());QVERIFY(optionsInvert->isVisible());
        QTRY_VERIFY(!diameter->isVisible());QTRY_VERIFY(!hardness->isVisible());QTRY_VERIFY(!opacity->isVisible());
        QVERIFY(window.currentDocument()==imported);moveAction->trigger();
        QVERIFY(optionsTransform->isVisible());QVERIFY(!optionsDeselect->isVisible());QVERIFY(!optionsInvert->isVisible());


        engine::BrushSettings brush;brush.diameter=10;brush.color={0,0,1,1};
        COMPLETE_OPERATION(window.applyStroke({{24,24},{34,24}},brush));
        const auto painted=window.currentDocument();QVERIFY(painted!=imported);
        QVERIFY(canonicalRaster(painted)!=original);QVERIFY(window.currentDirty());
        window.undoCurrent();QVERIFY(window.currentDocument()==imported);
        QCOMPARE(canonicalRaster(window.currentDocument()),original);
        window.redoCurrent();QVERIFY(window.currentDocument()==painted);
        const auto beforeErase=painted->singleLayer().raster->pixel(29,24).a;
        brush.erasing=true;
        COMPLETE_OPERATION(window.applyStroke({{29,24}},brush));
        QVERIFY(window.currentDocument()->singleLayer().raster->pixel(29,24).a<beforeErase);
        window.undoCurrent();QVERIFY(window.currentDocument()==painted);

        engine::TransformParameters transform;transform.dx=2;transform.dy=3;
        COMPLETE_OPERATION(window.transformCurrent(transform));
        QVERIFY(window.currentDocument()->singleLayer().localToDocument!=painted->singleLayer().localToDocument);
        COMPLETE_OPERATION(window.cropCurrent(engine::Extent{4,4,56,40}));
        QCOMPARE(window.currentDocument()->width,56);QCOMPARE(window.currentDocument()->height,40);
        COMPLETE_OPERATION(window.resizeCurrent(48,32));
        QCOMPARE(window.currentDocument()->width,48);QCOMPARE(window.currentDocument()->height,32);
        const engine::Selection selection{engine::Extent{8,8,24,16},engine::SelectionShape::Rectangle,false};
        COMPLETE_OPERATION(window.setSelectionCurrent(selection));
        QVERIFY(window.currentDocument()->selection==selection);
        COMPLETE_OPERATION(window.createMaskCurrent(true));
        const auto masked=window.currentDocument();QVERIFY(masked->singleLayer().mask);
        const auto maskBytes=canonicalRaster(*masked->singleLayer().mask);
        COMPLETE_OPERATION(window.enableMaskCurrent(false));
        QVERIFY(!window.currentDocument()->singleLayer().maskEnabled);
        window.undoCurrent();QVERIFY(window.currentDocument()==masked);
        COMPLETE_OPERATION(window.removeMaskCurrent());QVERIFY(!window.currentDocument()->singleLayer().mask);
        window.undoCurrent();QVERIFY(window.currentDocument()==masked);
        engine::AdjustmentParameters adjustment;adjustment.kind=engine::AdjustmentKind::Exposure;adjustment.value=.5;
        const auto beforeAdjustment=canonicalRaster(window.currentDocument());
        COMPLETE_OPERATION(window.applyAdjustment(adjustment));
        QVERIFY(canonicalRaster(window.currentDocument())!=beforeAdjustment);
        const auto edited=window.currentDocument();const auto editedPixels=canonicalRaster(edited);
        const auto nativePath=directory.filePath("edited.cproj");
        COMPLETE_OPERATION(window.saveCurrentTo(nativePath));
        QVERIFY(!window.currentDirty());QCOMPARE(canonicalRaster(window.currentDocument()),editedPixels);
        const auto saved=window.currentDocument();

        // Metadata commands cross the saved revision without changing source
        // pixels. Undo returns to the clean savepoint; redo restores the same
        // immutable command result and its dirty state.
        auto inverted=selection;inverted.inverted=true;
        COMPLETE_OPERATION(window.setSelectionCurrent(inverted));
        const auto changedSelection=window.currentDocument();QVERIFY(window.currentDirty());
        QVERIFY(changedSelection->selection==inverted);QCOMPARE(canonicalRaster(changedSelection),editedPixels);
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());
        window.redoCurrent();QVERIFY(window.currentDocument()==changedSelection);QVERIFY(window.currentDirty());
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());

        engine::TransformParameters moved;moved.dx=1;
        COMPLETE_OPERATION(window.transformCurrent(moved));
        const auto changedPlacement=window.currentDocument();QVERIFY(window.currentDirty());
        QVERIFY(changedPlacement->singleLayer().localToDocument!=saved->singleLayer().localToDocument);
        QCOMPARE(canonicalRaster(changedPlacement),editedPixels);
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());
        window.redoCurrent();QVERIFY(window.currentDocument()==changedPlacement);QVERIFY(window.currentDirty());
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());

        COMPLETE_OPERATION(window.enableMaskCurrent(false));
        const auto disabledMask=window.currentDocument();QVERIFY(window.currentDirty());
        QVERIFY(!disabledMask->singleLayer().maskEnabled);QVERIFY(disabledMask->singleLayer().mask==saved->singleLayer().mask);
        QCOMPARE(canonicalRaster(disabledMask),editedPixels);
        const auto dirtyExportPath=directory.filePath("dirty.png");
        COMPLETE_OPERATION(window.exportCurrentTo(dirtyExportPath));
        QVERIFY(window.currentDocument()==disabledMask);QVERIFY(window.currentDirty());
        const QImage dirtyExport(dirtyExportPath);QVERIFY(!dirtyExport.isNull());QCOMPARE(dirtyExport.size(),QSize(48,32));
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());
        window.redoCurrent();QVERIFY(window.currentDocument()==disabledMask);QVERIFY(window.currentDirty());
        window.undoCurrent();QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());

        const auto pngPath=directory.filePath("before.png"),jpegPath=directory.filePath("edited.jpg");
        COMPLETE_OPERATION(window.exportCurrentTo(pngPath));
        QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());
        COMPLETE_OPERATION(window.exportCurrentTo(jpegPath));
        QVERIFY(window.currentDocument()==saved);QVERIFY(!window.currentDirty());
        QImage png(pngPath),jpeg(jpegPath);QVERIFY(!png.isNull());QVERIFY(!jpeg.isNull());
        QCOMPARE(png.size(),QSize(48,32));QCOMPARE(jpeg.size(),QSize(48,32));
        QCOMPARE(png.pixelColor(0,0).alpha(),0);QCOMPARE(jpeg.pixelColor(0,0).alpha(),255);
        bool transparent=false,partial=false,visible=false;
        for(int y=0;y<png.height();++y) for(int x=0;x<png.width();++x) {
            const int alpha=png.pixelColor(x,y).alpha();transparent=transparent || alpha==0;
            partial=partial || (alpha>0 && alpha<255);visible=visible || alpha>0;
            QCOMPARE(jpeg.pixelColor(x,y).alpha(),255);
        }
        QVERIFY(transparent);QVERIFY(partial);QVERIFY(visible);

        COMPLETE_OPERATION(window.openPath(nativePath));
        QCOMPARE(canonicalRaster(window.currentDocument()),editedPixels);QVERIFY(!window.currentDirty());
        QVERIFY(window.currentDocument()->singleLayer().mask);
        QCOMPARE(canonicalRaster(*window.currentDocument()->singleLayer().mask),maskBytes);
        QVERIFY(window.currentDocument()->singleLayer().maskEnabled);
        QVERIFY(window.currentDocument()->singleLayer().localToDocument==edited->singleLayer().localToDocument);
        const auto reopenedPath=directory.filePath("after.png");COMPLETE_OPERATION(window.exportCurrentTo(reopenedPath));
        const QImage reopened(reopenedPath);QVERIFY(!reopened.isNull());
        QCOMPARE(reopened.convertToFormat(QImage::Format_RGBA8888),png.convertToFormat(QImage::Format_RGBA8888));
#undef COMPLETE_OPERATION
    }
};

int main(int argc,char** argv) {
    if(argc<2) {qCritical("Pass the existing disk-backed build directory");return 2;}
    buildDirectory=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QApplication app(argc,argv);EditorWorkflowTest test;
    return QTest::qExec(&test,argc-1,argv+1);
}
#include "editor_workflow_test.moc"
