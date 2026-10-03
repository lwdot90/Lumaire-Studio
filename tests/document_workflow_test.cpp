#include "app/window.h"
#include "app/layer_tree.h"
#include <QSignalSpy>
#include <QDoubleSpinBox>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;
class DocumentWorkflowTest:public QObject {
    Q_OBJECT
private slots:
    void lastDocumentReleasesThumbnailBacking() {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        QImage input(256,256,QImage::Format_RGB32);input.fill(Qt::red);
        input.setPixel(0,0,qRgb(0,0,255));
        const auto imagePath=directory.filePath("thumbnail.png");QVERIFY(input.save(imagePath));
        auto limits=ResourceLimits::forMachine(8ull*1024*1024*1024,2);
        auto resources=std::make_shared<RuntimeResources>(limits,[]{return MemorySample{4ull*1024*1024*1024,64ull*1024*1024};});
        MainWindow window(nullptr,true,{},resources);window.show();
        QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(imagePath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);QVERIFY(finished.last()[0].toBool());
        window.closeDocument(0);QCOMPARE(window.tabs()->count(),1);
        window.saveCurrentTo(directory.filePath("thumbnail.cproj"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000);QVERIFY(finished.last()[0].toBool());
        auto* tree=window.findChild<QTreeWidget*>("layerTree");QVERIFY(tree);
        window.setLayerSampling(engine::Sampling::Lanczos);
        QTRY_VERIFY_WITH_TIMEOUT(tree->currentItem() && !tree->currentItem()->icon(0).isNull(),10000);
        std::weak_ptr<const engine::RasterSnapshot> source=window.currentDocument()->singleLayer().raster;
        QVERIFY(resources->tiles->usedBytes()>0);
        window.undoCurrent();QVERIFY(!window.currentDirty());
        window.closeDocument(0);QCOMPARE(window.tabs()->count(),0);
        QTRY_VERIFY_WITH_TIMEOUT(source.expired(),10000);
        QTRY_COMPARE_WITH_TIMEOUT(resources->tiles->usedBytes(),std::size_t(0),10000);
        QTRY_COMPARE_WITH_TIMEOUT(resources->memory->snapshot().committedCpu,std::uint64_t(0),10000);
        QCOMPARE(resources->memory->snapshot().pendingCpu,std::uint64_t(0));
        QCOMPARE(tree->topLevelItemCount(),0);
    }
    void layerPlacement() {
        MainWindow window(nullptr,true);window.show();
        const auto bottom=*window.activeLayer();
        window.addLayer();const auto middle=*window.activeLayer();
        window.addLayer();const auto top=*window.activeLayer();
        const auto initial=window.currentDocument();
        QVERIFY(window.placeLayer(bottom,{},2));
        QCOMPARE(window.currentDocument()->layer(bottom).siblingOrder,2);
        QCOMPARE(window.currentDocument()->layer(middle).siblingOrder,0);
        QCOMPARE(window.currentDocument()->layer(top).siblingOrder,1);
        QVERIFY(window.activeLayer()==bottom);
        QVERIFY(window.currentDocument()->layer(bottom).raster==initial->layer(bottom).raster);
        window.undoCurrent();QVERIFY(window.currentDocument()==initial);
        window.redoCurrent();const auto moved=window.currentDocument();
        QVERIFY(window.placeLayer(bottom,{},2));QVERIFY(window.currentDocument()==moved);
        window.addLayer(true);const auto folder=*window.activeLayer();
        window.addLayer(true);const auto nested=*window.activeLayer();
        window.addLayer();const auto child=*window.activeLayer();
        auto* tree=static_cast<LayerTree*>(window.findChild<QTreeWidget*>("layerTree"));QVERIFY(tree);
        auto captured=tree->captureMove(QString::fromStdString(top.text()));QVERIFY(bool(captured));
        QVERIFY(captured(QString::fromStdString(nested.text()),0));
        QVERIFY(window.currentDocument()->layer(top).parent==nested);
        QCOMPARE(window.currentDocument()->layer(child).siblingOrder,1);
        const auto inserted=window.currentDocument();
        QVERIFY(!captured({},0));QVERIFY(window.currentDocument()==inserted); // stale immutable revision
        QVERIFY(!window.placeLayer(folder,nested,0)); // descendant cycle
        QVERIFY(!window.placeLayer(nested,nested,0)); // self cycle
        QVERIFY(!window.placeLayer(top,child,0)); // raster parent
        QVERIFY(!window.placeLayer(top,{},99));
        QVERIFY(!window.placeLayer(top,{},-1));
        QVERIFY(!window.placeLayer(engine::Id::generate(),{},0));
        QVERIFY(window.currentDocument()==inserted);
        const auto placement=window.currentDocument()->layer(child).localToDocument;
        QVERIFY(window.placeLayer(nested,{},0));
        QVERIFY(!window.currentDocument()->layer(nested).parent);
        QVERIFY(window.currentDocument()->layer(child).parent==nested);
        QVERIFY(window.currentDocument()->layer(child).localToDocument==placement);
        window.undoCurrent();QVERIFY(window.currentDocument()==inserted);
        auto tabGuard=tree->captureMove(QString::fromStdString(top.text()));
        window.addDocument();QVERIFY(!tabGuard({},0));
        window.tabs()->setCurrentIndex(0);QVERIFY(window.currentDocument()==inserted);
        window.undoCurrent();QVERIFY(!window.currentDocument()->layer(top).parent);
    }
    void thumbnailVisibleRows() {
        MainWindow window(nullptr,true);window.show();
        auto* tree=window.findChild<QTreeWidget*>("layerTree");QVERIFY(tree);
        for(int i=0;i<70;++i) window.addLayer();
        QTRY_VERIFY_WITH_TIMEOUT(!tree->topLevelItem(0)->icon(0).isNull(),10000);
        auto* first=tree->topLevelItem(0);
        tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum());
        auto* last=tree->topLevelItem(tree->topLevelItemCount()-1);
        QTRY_VERIFY_WITH_TIMEOUT(!last->icon(0).isNull(),10000);
        QVERIFY(first->icon(0).isNull());
        int icons=0;for(QTreeWidgetItemIterator it(tree);*it;++it) if(!(*it)->icon(0).isNull()) ++icons;
        QVERIFY(icons>0 && icons<=64);
    }
    void thumbnailLifecycle() {
        MainWindow window(nullptr,true);window.show();
        auto* tree=window.findChild<QTreeWidget*>("layerTree");QVERIFY(tree);
        QSignalSpy operations(&window,&MainWindow::operationFinished);
        auto color=[&] {
            if(!tree->currentItem() || tree->currentItem()->icon(0).isNull()) return QColor{};
            const auto image=tree->currentItem()->icon(0).pixmap(QSize(36,36)).toImage();
            return image.pixelColor(image.width()/2,image.height()/2);
        };
        window.fillCurrent({1,0,0,1});QTRY_COMPARE_WITH_TIMEOUT(operations.count(),1,10000);
        QVERIFY(operations.last()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(color().isValid() && color().red()>250 && color().blue()<5,10000);
        const auto redDocument=window.currentDocument();
        window.setLayerAppearance(false,.25f,engine::BlendMode::Multiply);
        QTRY_VERIFY_WITH_TIMEOUT(color().isValid() && color().red()>250,10000);
        // Pixel edits can finish for a background tab. Its thumbnail must not
        // land in the newly selected document's row.
        window.fillCurrent({0,0,1,1});window.addDocument();
        QTRY_COMPARE_WITH_TIMEOUT(operations.count(),2,10000);QVERIFY(operations.last()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(color().isValid() && color().red()<100 && color().blue()<100,10000);
        window.tabs()->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(color().isValid() && color().blue()>250 && color().red()<5,10000);
        window.undoCurrent();window.undoCurrent();
        QVERIFY(window.currentDocument()==redDocument);
        QTRY_VERIFY_WITH_TIMEOUT(color().isValid() && color().red()>250 && color().blue()<5,10000);
        if(const auto path=qEnvironmentVariable("COMPOSITOR_THUMBNAIL_CAPTURE");!path.isEmpty()) QVERIFY(window.grab().save(path));
    }
    void layeredWorkflow() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MainWindow window(nullptr,true); window.show(); QSignalSpy finished(&window,&MainWindow::operationFinished);
        const auto background=window.currentDocument()->singleLayer().id;
        window.fillCurrent({0,0,1,1}); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000); QVERIFY(finished.last()[0].toBool());
        window.duplicateLayer(); QCOMPARE(window.currentDocument()->layers().size(),std::size_t(2));
        const auto duplicate=*window.activeLayer();
        QVERIFY(window.currentDocument()->layer(duplicate).raster==window.currentDocument()->layer(background).raster);
        window.fillCurrent({1,0,0,1}); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000); QVERIFY(finished.last()[0].toBool());
        QCOMPARE(window.currentDocument()->layer(background).raster->pixel(0,0).b,1.f);
        QCOMPARE(window.currentDocument()->layer(duplicate).raster->pixel(0,0).r,1.f);
        window.setLayerAppearance(true,.5f,engine::BlendMode::Normal);
        QVERIFY(window.currentDocument()->stack.evaluate(.5,.5)==(engine::Pixel{.5f,0,.5f,1}));
        window.moveLayer(-1); QCOMPARE(window.currentDocument()->layer(duplicate).siblingOrder,0);
        window.undoCurrent(); QCOMPARE(window.currentDocument()->layer(duplicate).siblingOrder,1);
        window.addLayer(true); const auto folder=*window.activeLayer();
        window.addLayer(); const auto child=*window.activeLayer(); QVERIFY(window.currentDocument()->layer(child).parent==folder);
        auto* tree=window.findChild<QTreeWidget*>("layerTree"); QVERIFY(tree); QCOMPARE(tree->topLevelItemCount(),3);
        tree->currentItem()->setText(0,"Renamed child");
        QTRY_COMPARE(window.currentDocument()->layer(child).name,std::string("Renamed child"));
        window.selectLayer(folder);
        auto* opacity=window.findChild<QDoubleSpinBox*>("layerOpacity");QVERIFY(opacity);
        auto* blend=window.findChild<QComboBox*>("layerBlend");QVERIFY(blend);
        auto* folderSampling=window.findChild<QComboBox*>("layerSampling");QVERIFY(folderSampling);
        QVERIFY(opacity->isEnabled());QVERIFY(!blend->isEnabled());QVERIFY(!folderSampling->isEnabled());
        QCOMPARE(blend->count(),static_cast<int>(engine::blendModeCount));
        const auto beforeFolderOpacity=window.currentDocument();
        opacity->setValue(37.5);
        QCOMPARE(window.currentDocument()->layer(folder).opacity,.375f);
        const auto afterFolderOpacity=window.currentDocument();
        window.undoCurrent();QVERIFY(window.currentDocument()==beforeFolderOpacity);
        window.redoCurrent();QVERIFY(window.currentDocument()==afterFolderOpacity);QCOMPARE(opacity->value(),37.5);
        window.setLayerAppearance(false,.375f,engine::BlendMode::Normal);
        window.selectLayer(child);
        const auto beforeSampling=window.currentDocument();
        auto* sampling=window.findChild<QComboBox*>("layerSampling");QVERIFY(sampling);
        QVERIFY(QMetaObject::invokeMethod(sampling,"activated",Q_ARG(int,2)));
        QCOMPARE(window.currentDocument()->layer(child).sampling,engine::Sampling::Lanczos);
        window.undoCurrent();QVERIFY(window.currentDocument()==beforeSampling);
        window.redoCurrent();QCOMPARE(sampling->currentIndex(),2);
        const auto afterSampling=window.currentDocument();window.setLayerSampling(engine::Sampling::Lanczos);
        QVERIFY(window.currentDocument()==afterSampling); // No-op must not create history.
        window.selectLayer(duplicate); window.setLayerAppearance(true,.375f,engine::BlendMode::Screen);
        const auto saved=window.currentDocument(); const auto path=dir.filePath("layers.cproj");
        window.saveCurrentTo(path); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000); QVERIFY(finished.last()[0].toBool()); QVERIFY(!window.currentDirty());
        window.openPath(path); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000); QVERIFY(finished.last()[0].toBool());
        QCOMPARE(window.currentDocument()->layers().size(),std::size_t(4));
        QVERIFY(window.currentDocument()->layer(child).parent==folder); QVERIFY(!window.currentDocument()->layer(folder).visible);
        QCOMPARE(window.currentDocument()->layer(folder).opacity,.375f);
        QCOMPARE(window.currentDocument()->layer(child).sampling,engine::Sampling::Lanczos);
        QVERIFY(window.currentDocument()->stack.evaluate(.5,.5)==saved->stack.evaluate(.5,.5));
        window.selectLayer(folder); window.deleteLayer(); QCOMPARE(window.currentDocument()->layers().size(),std::size_t(2));
        window.undoCurrent(); QCOMPARE(window.currentDocument()->layers().size(),std::size_t(4)); QVERIFY(!window.currentDirty());
        QVERIFY(window.activeLayer()==folder); // Undo restores selection as in Swift history.
        window.selectLayer(child); window.reparentLayer({}); QVERIFY(!window.currentDocument()->layer(child).parent);
        window.undoCurrent(); QVERIFY(window.currentDocument()->layer(child).parent==folder); QVERIFY(!window.currentDirty());
        window.selectLayer(folder); const auto beforeInvalid=window.currentDocument(); window.reparentLayer(folder);
        QVERIFY(window.currentDocument()==beforeInvalid); QVERIFY(!window.currentDirty());
        QImage image(3,5,QImage::Format_RGBA8888); image.fill(Qt::green); const auto imagePath=dir.filePath("layer.png"); QVERIFY(image.save(imagePath));
        window.importLayer(imagePath); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),5,10000); QVERIFY(finished.last()[0].toBool());
        const auto inserted=*window.activeLayer(); const auto& insertedNode=window.currentDocument()->layer(inserted);
        QVERIFY(insertedNode.parent==folder); QCOMPARE(insertedNode.localToDocument.tx,510.0); QCOMPARE(insertedNode.localToDocument.ty,509.0);
        window.undoCurrent(); QVERIFY(window.currentDocument()==beforeInvalid); QVERIFY(window.activeLayer()==folder); QVERIFY(!window.currentDirty());
        window.close();
    }
    void workflow() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto imagePath=dir.filePath("input.png"),projectPath=dir.filePath("output.cproj");
        QImage image(257,258,QImage::Format_RGBA8888); image.fill(QColor(20,70,160,128)); QVERIFY(image.save(imagePath));
        MainWindow window(nullptr,true); window.show(); QSignalSpy finished(&window,&MainWindow::operationFinished);
        window.openPath(imagePath); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000); QVERIFY(finished.last()[0].toBool());
        QCOMPARE(window.currentDocument()->width,257); QVERIFY(window.currentDirty()); QVERIFY(!window.currentBusy());
        auto imported=window.currentDocument();
        QTRY_VERIFY_WITH_TIMEOUT(window.currentCanvas()->frameCount()>0,10000);
        const auto oldFrames=window.currentCanvas()->frameCount();
        window.fillCurrent({1,0,0,.5}); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,10000); QVERIFY(finished.last()[0].toBool());
        const auto edited=window.currentDocument(); QVERIFY(edited!=imported); QCOMPARE(edited->singleLayer().raster->pixel(0,0).r,.5f);
        window.undoCurrent(); QVERIFY(window.currentDocument()==imported);
        window.redoCurrent(); QVERIFY(window.currentDocument()==edited);
        QTRY_VERIFY_WITH_TIMEOUT(window.currentCanvas()->frameCount()>oldFrames,10000);
        window.saveCurrentTo(projectPath); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),3,10000); QVERIFY(finished.last()[0].toBool()); QVERIFY(!window.currentDirty());
        window.openPath(projectPath); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),4,10000); QVERIFY(finished.last()[0].toBool()); QVERIFY(!window.currentDirty());
        const auto reopened=window.currentDocument(); QCOMPARE(reopened->id.text(),edited->id.text());
        for(const auto& [coord,tile]:edited->singleLayer().raster->tiles) QVERIFY(reopened->singleLayer().raster->tiles.at(coord)->canonicalBytes()==tile->canonicalBytes());
        // A save failure does not mark the edited document clean or discard it.
        window.fillCurrent({0,1,0,1}); QTRY_COMPARE_WITH_TIMEOUT(finished.count(),5,10000);
        auto unsaved=window.currentDocument(); window.saveCurrentTo(dir.filePath("missing/out.cproj"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),6,10000); QVERIFY(!finished.last()[0].toBool()); QVERIFY(window.currentDirty()); QVERIFY(window.currentDocument()==unsaved);
        window.undoCurrent(); QVERIFY(!window.currentDirty());
        // Completing an open in a background tab must not install into the
        // current tab or replace its document.
        window.openPath(projectPath); const int loading=window.tabs()->currentIndex();
        window.addDocument(); const auto foreground=window.currentDocument();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),7,10000); QVERIFY(finished.last()[0].toBool()); QVERIFY(window.currentDocument()==foreground);
        window.tabs()->setCurrentIndex(loading); QCOMPARE(window.currentDocument()->id.text(),edited->id.text());
        window.close();
    }
};
QTEST_MAIN(DocumentWorkflowTest)
#include "document_workflow_test.moc"
