#include "app/editing_menus.h"
#include "core/soft_mask_edits.h"
#include "core/image_resize.h"
#include "core/retouch.h"
#include "app/color_panel.h"
#include "app/job_progress_widget.h"
#include "app/revisable_exposure_dialog.h"
#include "app/revisable_adjustments_dialog.h"
#include "app/adjustment_preview.h"
#include "app/application_icon.h"
#include "app/editor_theme.h"
#include "app/panel_icons.h"
#include "app/layer_visibility_delegate.h"
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QToolButton>
#include <QInputDialog>
#include "window.h"
#include "app/layer_tree.h"
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QColorDialog>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QStyle>
#include <set>
#include <QtConcurrent/QtConcurrentRun>
#include "io/image_import.h"
#include <stdexcept>

namespace compositor {
MainWindow::MainWindow(QVulkanInstance* instance,bool cpu,QString deviceName,std::shared_ptr<RuntimeResources> resources)
    :instance_(instance),cpu_(cpu),deviceName_(std::move(deviceName)),tabs_(new QTabWidget),diagnostics_(new QPlainTextEdit),
     resources_(resources ? std::move(resources) : defaultRuntimeResources()),tileStore_(resources_->tiles) {
    applyEditorTheme();
    jobs_.setMaxThreadCount(1);
    setWindowTitle("Lumaire Studio");setWindowIcon(applicationIcon());resize(1100,760);
    tabs_->setObjectName("documentTabs"); tabs_->setAccessibleName("Documents"); tabs_->setTabsClosable(true); setCentralWidget(tabs_);
    connect(tabs_,&QTabWidget::tabCloseRequested,this,&MainWindow::closeDocument);
    connect(tabs_,&QTabWidget::currentChanged,this,[this](int){
        queueThumbnails();
        emit editingContextChanged();
        if(!currentCanvas() && tools_) {tools_->setEnabled(false);tools_->attach(nullptr);tools_->setDocument({});tools_->setSelection({});}
        QTimer::singleShot(0,this,[this]{if(auto* c=currentCanvas()) { refresh(tabs_->currentWidget()); c->activateCanvas(); c->redraw(); }});
    });
    auto* file=menuBar()->addMenu("&File");
    auto* create=file->addAction("&New canvas"); create->setShortcut(QKeySequence::New); connect(create,&QAction::triggered,this,&MainWindow::addDocument);
    auto* open=file->addAction("&Open image or project…"); open->setShortcut(QKeySequence::Open);
    connect(open,&QAction::triggered,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"Open image or project",{},"Images and projects (*.png *.jpg *.jpeg *.cproj)"); if(!path.isEmpty()) openPath(path);});
    auto* save=file->addAction("&Save project"); save->setShortcut(QKeySequence::Save); connect(save,&QAction::triggered,this,[this]{chooseSave(false);});
    auto* saveAs=file->addAction("Save project &as…"); saveAs->setShortcut(QKeySequence::SaveAs); connect(saveAs,&QAction::triggered,this,[this]{chooseSave(true);});
    auto* exportAction=file->addAction("Export PNG or JPEG…"); exportAction->setObjectName("exportImage"); exportAction->setShortcut(QKeySequence("Ctrl+Shift+E"));
    connect(exportAction,&QAction::triggered,this,[this]{
        if(currentBusy() || !currentDocument()) return;
        QString filter="PNG image (*.png)";
        auto path=QFileDialog::getSaveFileName(this,"Export image",{},"PNG image (*.png);;JPEG image (*.jpg *.jpeg)",&filter);
        if(path.isEmpty()) return;
        if(QFileInfo(path).suffix().isEmpty()) {
            path+=filter.startsWith("PNG") ? ".png" : ".jpg";
            if(QFileInfo::exists(path) && QMessageBox::question(this,"Replace image?","The destination already exists. Replace it?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        }
        io::ExportOptions options;
        if(QFileInfo(path).suffix().compare("png",Qt::CaseInsensitive)!=0) {bool ok=false;options.quality=QInputDialog::getInt(this,"JPEG quality","Quality (1–100)",90,1,100,1,&ok);if(!ok) return;}
        options.expected=io::fileIdentity(path);
        exportCurrentTo(path,options);
    });
    auto* close=file->addAction("&Close canvas"); close->setShortcut(QKeySequence::Close); connect(close,&QAction::triggered,this,[this]{closeDocument(tabs_->currentIndex());});
    file->addSeparator(); auto* quit=file->addAction("&Quit"); quit->setShortcut(QKeySequence::Quit); connect(quit,&QAction::triggered,this,&QWidget::close);
    auto* edit=menuBar()->addMenu("&Edit");
    undoAction_=edit->addAction("&Undo"); undoAction_->setShortcut(QKeySequence::Undo); connect(undoAction_,&QAction::triggered,this,&MainWindow::undoCurrent);
    redoAction_=edit->addAction("&Redo"); redoAction_->setShortcut(QKeySequence::Redo); connect(redoAction_,&QAction::triggered,this,&MainWindow::redoCurrent);
    edit->addSeparator();
    auto* fill=edit->addAction("&Fill layer…"); connect(fill,&QAction::triggered,this,[this]{
        if(currentBusy()) return;
        const auto color=QColorDialog::getColor(Qt::white,this,"Fill layer",QColorDialog::ShowAlphaChannel);
        if(color.isValid()) fillCurrent({color.redF(),color.greenF(),color.blueF(),color.alphaF()});
    });
    auto* clear=edit->addAction("&Clear layer"); connect(clear,&QAction::triggered,this,[this]{fillCurrent({});});
    auto* layerMenu=menuBar()->addMenu("&Layer");
    auto* newLayerAction=layerMenu->addAction("New blank layer");
    connect(newLayerAction,&QAction::triggered,this,[this]{addLayer();});
    auto* newFolderAction=layerMenu->addAction("New folder");
    connect(newFolderAction,&QAction::triggered,this,[this]{addLayer(true);});
    connect(layerMenu->addAction("Import image as layer…"),&QAction::triggered,this,[this]{
        if(currentBusy()) return;
        const auto path=QFileDialog::getOpenFileName(this,"Import layer",{},"Images (*.png *.jpg *.jpeg)"); if(!path.isEmpty()) importLayer(path);
    });
    auto* duplicateLayerAction=layerMenu->addAction("Duplicate raster layer");
    connect(duplicateLayerAction,&QAction::triggered,this,&MainWindow::duplicateLayer);
    auto* deleteLayerAction=layerMenu->addAction("Delete layer / folder contents");
    connect(deleteLayerAction,&QAction::triggered,this,&MainWindow::deleteLayer);
    connect(layerMenu->addAction("Move up"),&QAction::triggered,this,[this]{moveLayer(1);});
    connect(layerMenu->addAction("Move down"),&QAction::triggered,this,[this]{moveLayer(-1);});
    auto* view=menuBar()->addMenu("&View");view->setObjectName("viewMenu");
    auto* fit=view->addAction("&Fit canvas"); fit->setShortcut(QKeySequence("Ctrl+0")); connect(fit,&QAction::triggered,this,[this]{if(auto* c=currentCanvas()) c->fit();});
    auto* actual=view->addAction("&Actual pixels"); actual->setShortcut(QKeySequence("Ctrl+1")); connect(actual,&QAction::triggered,this,[this]{if(auto* c=currentCanvas()) c->actualPixels();});
    tools_=std::make_unique<ToolController>(this,ToolController::Callbacks{
        [this](auto points,auto settings){applyStroke(std::move(points),settings);},
        [this](auto selection){setSelectionCurrent(std::move(selection));},
        [this](auto transform){transformCurrent(transform);},
        [this](QString error){statusBar()->showMessage(error);diagnostics_->appendPlainText(error);},
        [this](auto points,auto settings,auto anchor,bool heal,double radius){applyRetouchStroke(std::move(points),settings,anchor,heal,radius);}
    });
    tools_->setMode(ToolController::Mode::Move);
    addToolBar(Qt::LeftToolBarArea,tools_->toolbar());
    for(const auto* name:{"toolBrush","toolErase","toolClone","toolHeal"}) if(auto* action=findChild<QAction*>(name)) connect(action,&QAction::triggered,this,[this]{if(auto* canvas=currentCanvas();canvas && !canvas->cpu()) canvas->forceCpu("Interactive painting uses the CPU editor");});
    installEditingMenus(*this);
    tools_->bindCommandActions(findChild<QAction*>("transformLayerAction"),findChild<QAction*>("cropDocumentAction"),
        findChild<QAction*>("deselectAction"),findChild<QAction*>("invertSelectionAction"));
    addToolBar(tools_->brushToolbar());
    tools_->brushToolbar()->setMovable(false);tools_->toolbar()->setMovable(false);
    view->addSeparator();
    view->addAction(tools_->toolbar()->toggleViewAction());
    view->addAction(tools_->brushToolbar()->toggleViewAction());
    auto* layers=new QDockWidget("Layers",this); layers->setObjectName("layersDock");
    auto* layerPanel=new QWidget(layers); auto* layerLayout=new QVBoxLayout(layerPanel);
    layerLayout->setContentsMargins(6,6,6,4);layerLayout->setSpacing(5);
    layerInfo_=new QLabel(layerPanel);layerInfo_->setProperty("mutedLabel",true); layerInfo_->setTextFormat(Qt::PlainText);
    auto* movableTree=new LayerTree(layerPanel);layerTree_=movableTree;
    movableTree->captureMove=[this](QString key)->LayerTree::Move {
        const auto input=currentDocument();if(!input || currentBusy()) return {};
        const engine::Id id(key.toStdString());
        return [this,input,id](QString parent,int order) {
            if(currentDocument()!=input || currentBusy()) return false;
            return placeLayer(id,parent.isEmpty() ? std::optional<engine::Id>{} : engine::Id(parent.toStdString()),order);
        };
    };
    layerTree_->setItemDelegate(new LayerVisibilityDelegate(layerTree_));
    layerTree_->setObjectName("layerTree"); layerTree_->setHeaderHidden(true);
    layerTree_->setIconSize({32,32});layerTree_->setUniformRowHeights(true);layerTree_->viewport()->installEventFilter(this);
    thumbnailTimer_=new QTimer(this);thumbnailTimer_->setSingleShot(true);thumbnailTimer_->setInterval(20);
    connect(thumbnailTimer_,&QTimer::timeout,this,&MainWindow::requestThumbnails);
    connect(layerTree_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{queueThumbnails();});
    connect(layerTree_,&QTreeWidget::itemExpanded,this,[this]{queueThumbnails();});
    connect(layerTree_,&QTreeWidget::itemCollapsed,this,[this]{queueThumbnails();});
    thumbnails_=std::make_unique<ThumbnailWorker>([this](auto generation,auto document,auto results) {
        QMetaObject::invokeMethod(this,[this,generation,document=std::move(document),results=std::move(results)] {
            if(generation!=thumbnailGeneration_ || document!=currentDocument()) return;
            const QSignalBlocker treeSignals(layerTree_);
            for(const auto& result:results) {
                if(!result.error.isEmpty()) {diagnostics_->appendPlainText("Thumbnail: "+result.error);continue;}
                const auto item=layerItems_.find(result.id.text());
                const auto old=thumbnailImageKeys_.find(result.id.text());
                if(item!=layerItems_.end() && (old==thumbnailImageKeys_.end() || old->second!=result.image.cacheKey())) {
                    item->second->setIcon(0,QPixmap::fromImage(result.image));
                    thumbnailImageKeys_[result.id.text()]=result.image.cacheKey();
                }
            }
        },Qt::QueuedConnection);
    },resources_);
    auto* layerButtons=new QHBoxLayout;layerButtons->setSpacing(2);
    layerButtons->addStretch();
    int layerButtonIndex=0;
    auto* addMaskAction=findChild<QAction*>("createRevealMaskAction");
    for(auto* item:{newLayerAction,newFolderAction,duplicateLayerAction,deleteLayerAction,addMaskAction}) {
        auto* button=new QToolButton(layerPanel);button->setIcon(layerPanelIcon(layerButtonIndex));
        button->setIconSize({18,18});button->setToolTip(item->text());button->setAccessibleName(item->text());
        connect(button,&QToolButton::clicked,item,&QAction::trigger);
        connect(item,&QAction::changed,button,[button,item]{button->setEnabled(item->isEnabled());});
        layerButtons->addWidget(button);++layerButtonIndex;
    }
    connect(this,&MainWindow::editingContextChanged,this,[this,newLayerAction,newFolderAction,duplicateLayerAction,deleteLayerAction,addMaskAction,save,exportAction]{
        const auto document=currentDocument();const auto selected=activeLayer();
        const bool editable=document && !currentBusy();
        newLayerAction->setEnabled(editable);newFolderAction->setEnabled(editable);save->setEnabled(editable);exportAction->setEnabled(editable);
        duplicateLayerAction->setEnabled(editable && selected && !document->layer(*selected).folder);
        deleteLayerAction->setEnabled(editable && selected.has_value());
        addMaskAction->setEnabled(editable && selected && !document->layer(*selected).folder);
    });
    auto* appearance=new QHBoxLayout;appearance->setSpacing(5);
    blend_=new QComboBox(layerPanel);blend_->setObjectName("layerBlend");blend_->setAccessibleName("Layer blend mode");
    for(std::size_t i=0;i<engine::blendModeCount;++i) {
        auto name=QString::fromStdString(std::string(engine::blendIdentifier(static_cast<engine::BlendMode>(i))));
        name.replace('-', ' ');name.replace('_',' ');if(!name.isEmpty()) name[0]=name[0].toUpper();blend_->addItem(name);
    }
    opacity_=new QDoubleSpinBox(layerPanel);opacity_->setObjectName("layerOpacity");opacity_->setAccessibleName("Layer opacity");
    opacity_->setRange(0,100);opacity_->setDecimals(2);opacity_->setSuffix("%");opacity_->setKeyboardTracking(false);
    opacity_->setButtonSymbols(QAbstractSpinBox::NoButtons);opacity_->setFixedWidth(76);
    appearance->addWidget(blend_,1);appearance->addWidget(new QLabel("Opacity",layerPanel));appearance->addWidget(opacity_);
    layerLayout->addLayout(appearance);
    layerLayout->addWidget(layerTree_,1);layerTree_->setMinimumHeight(100);
    layerLayout->addLayout(layerButtons);
    layers->setWidget(layerPanel);addDockWidget(Qt::RightDockWidgetArea,layers);
    auto* properties=new QDockWidget("Properties",this);properties->setObjectName("propertiesDock");
    auto* propertiesPanel=new QWidget(properties);auto* propertiesForm=new QFormLayout(propertiesPanel);
    propertiesForm->setContentsMargins(8,8,8,8);propertiesForm->addRow(layerInfo_);
    sampling_=new QComboBox(propertiesPanel);sampling_->setObjectName("layerSampling");sampling_->setAccessibleName("Layer sampling");
    sampling_->addItems({"Nearest", "Bilinear", "Lanczos-3"});propertiesForm->addRow("Sampling",sampling_);
    parent_=new QComboBox(propertiesPanel);parent_->setObjectName("layerParent");parent_->setAccessibleName("Parent folder");propertiesForm->addRow("Parent",parent_);
    properties->setWidget(propertiesPanel);addDockWidget(Qt::RightDockWidgetArea,properties);
    auto* colorDock=new QDockWidget("Color",this);colorDock->setObjectName("colorDock");
    auto* colorPanel=new ColorPanel(colorDock);colorDock->setWidget(colorPanel);addDockWidget(Qt::RightDockWidgetArea,colorDock);
    colorPanel->setColor(tools_->foregroundColor());
    tools_->setForegroundChangedCallback([colorPanel](QColor color){colorPanel->setColor(color);});
    connect(colorPanel,&ColorPanel::colorChanged,this,[this](QColor color){tools_->setForegroundColor(color);});
    if(auto* photo=findChild<QDockWidget*>("photoEditingDock")) {
        splitDockWidget(photo,layers,Qt::Vertical);tabifyDockWidget(photo,properties);tabifyDockWidget(photo,colorDock);colorDock->raise();
        for(auto* panel:{photo,properties,colorDock}) {auto* title=new QWidget(panel);title->setFixedHeight(0);panel->setTitleBarWidget(title);}
        resizeDocks({colorDock,layers},{310,350},Qt::Vertical);
    }
    view->addAction(colorDock->toggleViewAction());
    setTabPosition(Qt::RightDockWidgetArea,QTabWidget::North);
    resizeDocks({layers},{250},Qt::Horizontal);
    view->addAction(layers->toggleViewAction());view->addAction(properties->toggleViewAction());
    connect(layerTree_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item,QTreeWidgetItem*) {
        if(item && !currentBusy()) selectLayer(engine::Id(item->data(0,Qt::UserRole).toString().toStdString()));
    });
    connect(layerTree_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* item,int) {
        if(currentBusy() || !currentDocument()) return;
        auto nodes=currentDocument()->layers(); const engine::Id id(item->data(0,Qt::UserRole).toString().toStdString());
        for(auto& node:nodes) if(node.id==id) {
            const auto name=item->text(0).trimmed().toStdString(); if(!name.empty()) node.name=name;
            node.visible=item->checkState(0)==Qt::Checked;
        }
        // Defer rebuilding the tree until Qt finishes delivering itemChanged.
        QTimer::singleShot(0,this,[this,nodes=std::move(nodes),input=currentDocument(),id]() mutable {
            if(currentDocument()==input) commitLayers(std::move(nodes),"Layer properties",id);
        });
    });
    connect(opacity_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if(const auto id=activeLayer()) { const auto& node=currentDocument()->layer(*id); setLayerAppearance(node.visible,static_cast<float>(value/100),node.blend); }
    });
    connect(blend_,&QComboBox::activated,this,[this](int index) {
        if(const auto id=activeLayer()) { const auto& node=currentDocument()->layer(*id); setLayerAppearance(node.visible,node.opacity,static_cast<engine::BlendMode>(index)); }
    });
    connect(sampling_,&QComboBox::activated,this,[this](int index){setLayerSampling(static_cast<engine::Sampling>(index));});
    connect(parent_,&QComboBox::activated,this,[this](int index) {
        const auto key=parent_->itemData(index).toString();
        reparentLayer(key.isEmpty() ? std::optional<engine::Id>{} : engine::Id(key.toStdString()));
    });
    auto* dock=new QDockWidget("Rendering diagnostics",this); dock->setObjectName("diagnosticsDock");
    diagnostics_->setReadOnly(true); diagnostics_->setAccessibleName("Rendering diagnostics"); diagnostics_->setMaximumBlockCount(500);
    dock->setWidget(diagnostics_);addDockWidget(Qt::BottomDockWidgetArea,dock);dock->hide();
    view->addAction(dock->toggleViewAction());
    diagnostics_->appendPlainText(QString("Platform: %1; Vulkan validation: %2").arg(QGuiApplication::platformName(),
        instance_ && instance_->layers().contains("VK_LAYER_KHRONOS_validation") ? "enabled" : "unavailable / disabled"));
    diagnostics_->appendPlainText(QString("Resource profile: %1; shared CPU slots: %2; canonical tile ceiling: %3 MiB; GPU frame ceiling: %4 MiB")
        .arg(resources_->limits.lowMemory ? "low memory" : "standard").arg(resources_->limits.computeWorkers)
        .arg(resources_->limits.canonicalTiles/(1024*1024)).arg(resources_->limits.gpuFrame/(1024*1024)));
    if(!resources_->storageError.empty()) diagnostics_->appendPlainText(QString("Disk spill unavailable: %1").arg(QString::fromStdString(resources_->storageError)));
    auto* progress=new JobProgressWidget(this);
    statusBar()->addPermanentWidget(progress);
    connect(this,&MainWindow::editingContextChanged,this,[this,progress,activePage=static_cast<QWidget*>(nullptr)]() mutable {
        auto* page=tabs_->currentWidget();
        if(page!=activePage) {progress->setState(false);activePage=page;}
        const auto found=sessions_.find(page);
        progress->setState(found!=sessions_.end() && found->second->busy,
            found!=sessions_.end() && found->second->stop.stop_requested());
    });
    connect(progress,&JobProgressWidget::cancelRequested,this,[this,progress] {
        const auto found=sessions_.find(tabs_->currentWidget());
        if(found==sessions_.end() || !found->second->busy) return;
        found->second->stop.request_stop();progress->setState(true,true);
        statusBar()->showMessage("Cancelling current operation…");
        emit editingContextChanged();
    });
    zoomInfo_=new QLabel("100%",this);zoomInfo_->setMinimumWidth(48);zoomInfo_->setAlignment(Qt::AlignCenter);
    statusBar()->addPermanentWidget(zoomInfo_);
    for(const auto entry:{std::pair{fit,"Fit"},std::pair{actual,"100%"}}) {
        auto* button=new QToolButton(this);button->setText(entry.second);button->setToolTip(entry.first->text());
        button->setAccessibleName(entry.first->text());connect(button,&QToolButton::clicked,entry.first,&QAction::trigger);
        statusBar()->addPermanentWidget(button);
    }
    statusBar()->showMessage("Wheel to zoom · Space-drag to pan");
    addDocument();
}
MainWindow::~MainWindow() {
    thumbnails_.reset();
    for(const auto& [page,session]:sessions_) session->stop.request_stop();
    jobs_.waitForDone();
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if(layerTree_ && watched==layerTree_->viewport() && (event->type()==QEvent::Resize || event->type()==QEvent::Show)) queueThumbnails();
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::queueThumbnails() {
    ++thumbnailGeneration_;
    if(thumbnailTimer_) thumbnailTimer_->start();
}
void MainWindow::requestThumbnails() {
    if(!thumbnails_) return;
    auto document=currentDocument();
    if(!document) {
        thumbnails_->suspend();
        const QSignalBlocker treeSignals(layerTree_);
        thumbnailImageKeys_.clear();thumbnailItems_.clear();layerItems_.clear();layerTree_->clear();
        return;
    }
    std::vector<engine::Id> ids;
    auto* item=layerTree_->itemAt(QPoint(1,0));
    if(!item && layerTree_->topLevelItemCount()) item=layerTree_->topLevelItem(0);
    for(;layerTree_->isVisible() && item && ids.size()<64;item=layerTree_->itemBelow(item)) {
        const auto rectangle=layerTree_->visualItemRect(item);
        if(rectangle.top()>=layerTree_->viewport()->height()) break;
        if(!rectangle.intersects(layerTree_->viewport()->rect())) continue;
        const engine::Id id(item->data(0,Qt::UserRole).toString().toStdString());
        if(!document->layer(id).folder) ids.push_back(id);
    }
    std::set<std::string> wanted;for(const auto& id:ids) wanted.insert(id.text());
    {
        const QSignalBlocker treeSignals(layerTree_);
        for(const auto& id:thumbnailItems_) if(!wanted.contains(id)) {
            thumbnailImageKeys_.erase(id);
            if(const auto found=layerItems_.find(id);found!=layerItems_.end()) found->second->setIcon(0,{});
        }
    }
    thumbnailItems_=std::move(wanted);
    thumbnails_->request(thumbnailGeneration_,std::move(document),std::move(ids));
}
Canvas* MainWindow::currentCanvas() const {
    auto* page=tabs_->currentWidget();
    return page ? page->property("canvas").value<Canvas*>() : nullptr;
}
void MainWindow::addDocument() {
    auto* page=new QWidget;
    auto session=std::make_shared<Session>();
    session->history=std::make_unique<engine::DocumentHistory>(engine::blankDocument(1024,1024),100,512*1024*1024);
    session->title=QString("Untitled %1").arg(nextDocument_++); sessions_[page]=session;
    auto* layout=new QVBoxLayout(page); layout->setContentsMargins(0,0,0,0);
    attachCanvas(page,cpu_);
    const auto index=tabs_->addTab(page,session->title); tabs_->setCurrentIndex(index); refresh(page);
}
void MainWindow::attachCanvas(QWidget* page,bool cpu,const Viewport* previous,QString reason) {
    auto* canvas=new Canvas(instance_,cpu,deviceName_,nullptr,resources_);
    if(previous) canvas->restoreViewport(*previous,std::move(reason));
    auto* layout=page->layout();
    auto* container=QWidget::createWindowContainer(canvas,page);
    container->setFocusPolicy(Qt::StrongFocus); container->setAccessibleName("Image canvas"); container->setMinimumSize(160,120);
    connect(canvas,&Canvas::focusRequested,container,[container]{container->setFocus(Qt::OtherFocusReason);});
    container->setProperty("canvasTarget",QVariant::fromValue(canvas));container->installEventFilter(canvas);
    layout->addWidget(container); page->setProperty("canvas",QVariant::fromValue(canvas));
    connect(canvas,&Canvas::diagnostic,diagnostics_,&QPlainTextEdit::appendPlainText);
    connect(canvas,&Canvas::fallbackRequested,page,[this,page,canvas](QString message){
        if(page->property("canvas").value<Canvas*>()!=canvas) return;
        const auto viewport=canvas->viewport();
        auto* item=page->layout()->takeAt(0);
        delete item->widget(); // Owns and destroys the old native canvas.
        delete item;
        attachCanvas(page,true,&viewport,std::move(message));
        // Restore the same immutable document after native-surface replacement.
        refresh(page);
        if(page==tabs_->currentWidget()) currentCanvas()->activateCanvas();
    },Qt::QueuedConnection);
    connect(canvas,&Canvas::inputPosition,this,[this](QPointF p,double zoom){statusBar()->showMessage(QString("Document %1, %2 · Zoom %3%").arg(p.x(),0,'f',2).arg(p.y(),0,'f',2).arg(zoom*100,0,'f',1));});
    connect(canvas,&Canvas::frameCompleted,this,[this,canvas](quint64){
        if(zoomInfo_ && currentCanvas()==canvas) zoomInfo_->setText(QString("%1%").arg(canvas->viewport().zoom*100,0,'f',1));
    });
    connect(canvas,&Canvas::timing,this,[this](QJsonObject event){
        // Bounded capture, flushed explicitly at shutdown; no pointer-path disk IO.
        if(timings_.size()<10000) timings_.append(event);
    });
    diagnostics_->appendPlainText(canvas->backendReason());
}
void MainWindow::closeDocument(int index) {
    if(index<0) return;
    auto* page=tabs_->widget(index); if(!page || !mayClose(page)) return;
    tabs_->removeTab(index); sessions_.erase(page); delete page;
}
engine::DocumentPtr MainWindow::currentDocument() const {
    const auto found=sessions_.find(tabs_->currentWidget()); return found==sessions_.end() ? nullptr : found->second->history->current();
}
bool MainWindow::currentDirty() const { const auto found=sessions_.find(tabs_->currentWidget()); return found!=sessions_.end() && found->second->history->dirty(); }
bool MainWindow::currentBusy() const { const auto found=sessions_.find(tabs_->currentWidget()); return found!=sessions_.end() && found->second->busy; }
std::optional<engine::Id> MainWindow::activeLayer() const {
    const auto found=sessions_.find(tabs_->currentWidget());
    return found==sessions_.end() ? std::optional<engine::Id>{} : found->second->history->selectedLayer();
}
void MainWindow::selectLayer(const engine::Id& id) {
    if(!currentDocument() || currentBusy()) return;
    const auto& node=currentDocument()->layer(id); sessions_.at(tabs_->currentWidget())->history->selectLayer(id);
    const QSignalBlocker treeSignals(layerTree_),opacitySignals(opacity_),blendSignals(blend_),parentSignals(parent_),samplingSignals(sampling_);
    if(const auto item=layerItems_.find(id.text());item!=layerItems_.end()) layerTree_->setCurrentItem(item->second);
    opacity_->setEnabled(true); blend_->setEnabled(!node.folder);
    sampling_->setEnabled(!node.folder);sampling_->setCurrentIndex(static_cast<int>(node.sampling));
    opacity_->setValue(node.opacity*100); blend_->setCurrentIndex(static_cast<int>(node.blend));
    parent_->setEnabled(true); parent_->setCurrentIndex(parent_->findData(node.parent ? QString::fromStdString(node.parent->text()) : QString{}));
    // Target selection changes tool/menu availability without rebuilding the
    // layer tree or requesting another image render.
    if(tools_) {tools_->setRasterTargetAvailable(!node.folder);tools_->setRetouchTarget(id);tools_->setPaintTargetsAvailable(!node.folder && !node.adjustments && !node.retouch,!node.folder && node.mask && node.maskEnabled);}
    emit editingContextChanged();
}
void MainWindow::commitLayers(std::vector<engine::LayerNode> nodes,const std::string& name,std::optional<engine::Id> selected) {
    if(!currentDocument() || currentBusy()) return;
    auto* page=tabs_->currentWidget(); const auto session=sessions_.at(page);
    try {
        auto edit=session->history->begin(); edit.setLayers(std::move(nodes));
        if(session->history->commit(edit,name,std::move(selected))) session->imageActive=true;
    } catch(const std::exception& error) { diagnostics_->appendPlainText(QString::fromUtf8(error.what())); statusBar()->showMessage(QString::fromUtf8(error.what())); }
    refresh(page);
}
void MainWindow::addLayer(bool folder) {
    const auto input=currentDocument(); if(!input || currentBusy()) return;
    auto nodes=input->layers(); engine::LayerNode node{engine::Id::generate()}; node.folder=folder;
    const std::string prefix=folder ? "Folder " : "Layer "; int number=1;
    do { node.name=prefix+std::to_string(number++); } while(std::any_of(nodes.begin(),nodes.end(),[&](const auto& other){return other.name==node.name;}));
    if(const auto selected=activeLayer()) {
        const auto& active=input->layer(*selected); node.parent=active.folder ? selected : active.parent;
        node.siblingOrder=active.folder ? static_cast<int>(std::count_if(nodes.begin(),nodes.end(),[&](const auto& other){return other.parent==node.parent;})) : active.siblingOrder+1;
    } else node.siblingOrder=static_cast<int>(std::count_if(nodes.begin(),nodes.end(),[](const auto& other){return !other.parent;}));
    for(auto& other:nodes) if(other.parent==node.parent && other.siblingOrder>=node.siblingOrder) ++other.siblingOrder;
    if(!folder) node.raster=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),engine::Extent{0,0,input->width,input->height});
    const auto id=node.id; nodes.push_back(std::move(node)); commitLayers(std::move(nodes),folder ? "New folder" : "New blank layer",id);
}
void MainWindow::duplicateLayer() {
    const auto input=currentDocument(); const auto selected=activeLayer(); if(!input || !selected || currentBusy()) return;
    auto node=input->layer(*selected); if(node.folder) return;
    auto nodes=input->layers(); node.id=engine::Id::generate(); node.name+=" copy"; ++node.siblingOrder;
    for(auto& other:nodes) if(other.parent==node.parent && other.siblingOrder>=node.siblingOrder) ++other.siblingOrder;
    const auto id=node.id; nodes.push_back(std::move(node)); commitLayers(std::move(nodes),"Duplicate layer",id);
}
void MainWindow::deleteLayer() {
    const auto input=currentDocument(); const auto selected=activeLayer(); if(!input || !selected || currentBusy()) return;
    auto nodes=input->layers(); std::set<std::string> removed{selected->text()};
    // Validated depth is at most 64. A selected folder takes its descendants.
    for(int depth=0;depth<65;++depth) {
        const auto before=removed.size();
        for(const auto& node:nodes) if(node.parent && removed.contains(node.parent->text())) removed.insert(node.id.text());
        if(before==removed.size()) break;
    }
    std::erase_if(nodes,[&](const auto& node){return removed.contains(node.id.text());});
    std::map<std::string,std::vector<engine::LayerNode*>> siblings;
    for(auto& node:nodes) siblings[node.parent ? node.parent->text() : ""].push_back(&node);
    for(auto& [key,group]:siblings) {
        std::sort(group.begin(),group.end(),[](const auto* a,const auto* b){return a->siblingOrder<b->siblingOrder;});
        for(std::size_t i=0;i<group.size();++i) group[i]->siblingOrder=static_cast<int>(i);
    }
    commitLayers(std::move(nodes),"Delete layer",{});
}
void MainWindow::moveLayer(int offset) {
    const auto input=currentDocument(); const auto selected=activeLayer(); if(!input || !selected || currentBusy() || (offset!=1 && offset!=-1)) return;
    auto nodes=input->layers(); const auto& active=input->layer(*selected); const auto order=active.siblingOrder+offset;
    const auto neighbor=std::find_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.parent==active.parent && node.siblingOrder==order;});
    if(neighbor==nodes.end()) return;
    neighbor->siblingOrder=active.siblingOrder;
    for(auto& node:nodes) if(node.id==*selected) node.siblingOrder=order;
    commitLayers(std::move(nodes),"Reorder layers",selected);
}
void MainWindow::setLayerAppearance(bool visible,float opacity,engine::BlendMode blend) {
    const auto input=currentDocument(); const auto selected=activeLayer(); if(!input || !selected || currentBusy()) return;
    auto nodes=input->layers();
    for(auto& node:nodes) if(node.id==*selected) { node.visible=visible; node.opacity=opacity; node.blend=blend; }
    commitLayers(std::move(nodes),"Layer appearance",selected);
}
void MainWindow::setLayerSampling(engine::Sampling sampling) {
    const auto input=currentDocument();const auto selected=activeLayer();
    if(!input || !selected || currentBusy() || input->layer(*selected).folder) return;
    auto nodes=input->layers();
    for(auto& node:nodes) if(node.id==*selected) node.sampling=sampling;
    commitLayers(std::move(nodes),"Layer sampling",selected);
}
void MainWindow::reparentLayer(std::optional<engine::Id> parent) {
    const auto input=currentDocument(); const auto selected=activeLayer(); if(!input || !selected || currentBusy()) return;
    const auto& active=input->layer(*selected); if(active.parent==parent) return;
    auto nodes=input->layers();
    const int top=static_cast<int>(std::count_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.parent==parent;}));
    for(auto& node:nodes) {
        if(node.id==*selected) { node.parent=parent; node.siblingOrder=top; }
        else if(node.parent==active.parent && node.siblingOrder>active.siblingOrder) --node.siblingOrder;
    }
    // Graph validation rejects self/descendant parents and nonfolder targets
    // before replacing the live snapshot. Child transforms stay in document space.
    commitLayers(std::move(nodes),"Move layer to folder",selected);
}
bool MainWindow::placeLayer(const engine::Id& id,std::optional<engine::Id> parent,int order) {
    const auto input=currentDocument();if(!input || currentBusy()) return false;
    const auto source=std::find_if(input->layers().begin(),input->layers().end(),[&](const auto& node){return node.id==id;});
    if(source==input->layers().end()) return false;
    if(parent) {
        auto ancestor=parent;
        while(ancestor) {
            if(*ancestor==id) return false;
            const auto found=std::find_if(input->layers().begin(),input->layers().end(),[&](const auto& node){return node.id==*ancestor;});
            if(found==input->layers().end() || !found->folder) return false;
            ancestor=found->parent;
        }
    }
    const auto count=std::count_if(input->layers().begin(),input->layers().end(),[&](const auto& node){return node.id!=id && node.parent==parent;});
    if(order<0 || order>count) return false;
    if(source->parent==parent && source->siblingOrder==order) return true;
    auto nodes=input->layers();
    for(auto& node:nodes) if(node.id!=id && node.parent==source->parent && node.siblingOrder>source->siblingOrder) --node.siblingOrder;
    for(auto& node:nodes) if(node.id!=id && node.parent==parent && node.siblingOrder>=order) ++node.siblingOrder;
    for(auto& node:nodes) if(node.id==id) {node.parent=parent;node.siblingOrder=order;}
    commitLayers(std::move(nodes),"Move layer",id);
    if(parent && currentDocument()!=input) {
        const auto item=layerItems_.find(parent->text());
        if(item!=layerItems_.end()) item->second->setExpanded(true);
    }
    return currentDocument()!=input;
}
void MainWindow::refresh(QWidget* page,bool fitView) {
    const auto found=sessions_.find(page); if(found==sessions_.end()) return;
    auto& session=*found->second; const auto document=session.history->current();
    const auto selected=session.history->selectedLayer();
    tabs_->setTabText(tabs_->indexOf(page),session.title+(session.history->dirty() ? " *" : "")+(session.busy ? " …" : ""));
    if(session.imageActive) if(auto* canvas=page->property("canvas").value<Canvas*>()) canvas->setDocument(document,fitView);
    if(page==tabs_->currentWidget()) {
        if(tools_) {
            tools_->attach(currentCanvas());tools_->setDocument(document);tools_->setSelection(document->selection);
            tools_->setEnabled(!session.busy);tools_->setRasterTargetAvailable(selected && !document->layer(*selected).folder);tools_->setRetouchTarget(selected);
            const bool raster=selected && !document->layer(*selected).folder;
            tools_->setPaintTargetsAvailable(raster && !document->layer(*selected).adjustments && !document->layer(*selected).retouch,
                raster && document->layer(*selected).mask && document->layer(*selected).maskEnabled);
            if(auto* canvas=currentCanvas();canvas && !canvas->cpu() && (tools_->mode()==ToolController::Mode::Brush || tools_->mode()==ToolController::Mode::Erase || tools_->mode()==ToolController::Mode::Clone || tools_->mode()==ToolController::Mode::Heal))
                canvas->forceCpu("Interactive painting uses the CPU editor");
        }
        undoAction_->setEnabled(!session.busy && session.history->canUndo()); redoAction_->setEnabled(!session.busy && session.history->canRedo());
        const auto name=session.history->undoName(); undoAction_->setText(name.empty() ? "&Undo" : "&Undo "+QString::fromUtf8(name.data(),static_cast<qsizetype>(name.size())));
        layerInfo_->setText(QString("%1 × %2 px · %3 ppi").arg(document->width).arg(document->height).arg(document->resolution));
        const QSignalBlocker treeSignals(layerTree_),opacitySignals(opacity_),blendSignals(blend_),parentSignals(parent_),samplingSignals(sampling_);
        std::set<QString> collapsed;
        for(QTreeWidgetItemIterator it(layerTree_);*it;++it) if(!(*it)->isExpanded()) collapsed.insert((*it)->data(0,Qt::UserRole).toString());
        thumbnailImageKeys_.clear();thumbnailItems_.clear();layerItems_.clear();layerTree_->clear(); layerTree_->setEnabled(!session.busy);
        std::map<std::string,std::vector<const engine::LayerNode*>> children;
        for(const auto& node:document->layers()) children[node.parent ? node.parent->text() : ""].push_back(&node);
        for(auto& [key,siblings]:children) std::sort(siblings.begin(),siblings.end(),[](const auto* a,const auto* b){return a->siblingOrder>b->siblingOrder;});
        std::function<void(const std::string&,QTreeWidgetItem*)> populate=[&](const auto& parent,QTreeWidgetItem* owner) {
            for(const auto* node:children[parent]) {
                auto* item=owner ? new QTreeWidgetItem(owner) : new QTreeWidgetItem(layerTree_);
                layerItems_.emplace(node->id.text(),item);
                item->setData(0,Qt::UserRole,QString::fromStdString(node->id.text())); item->setText(0,QString::fromStdString(node->name));
                item->setData(0,Qt::UserRole+1,node->folder);
                if(!node->folder) item->setFlags(item->flags()&~Qt::ItemIsDropEnabled);
                item->setFlags(item->flags()|Qt::ItemIsEditable|Qt::ItemIsUserCheckable); item->setCheckState(0,node->visible ? Qt::Checked : Qt::Unchecked);
                item->setSizeHint(0,{0,40});
                if(node->folder) item->setIcon(0,style()->standardIcon(QStyle::SP_DirIcon));
                if(node->folder) { populate(node->id.text(),item); item->setExpanded(!collapsed.contains(QString::fromStdString(node->id.text()))); }
                if(selected==node->id) layerTree_->setCurrentItem(item);
            }
        };
        populate("",nullptr);
        const auto* active=selected ? &document->layer(*selected) : nullptr;
        opacity_->setEnabled(!session.busy && active); blend_->setEnabled(!session.busy && active && !active->folder);
        sampling_->setEnabled(blend_->isEnabled());sampling_->setCurrentIndex(active ? static_cast<int>(active->sampling) : 1);
        opacity_->setValue(active ? active->opacity*100 : 100); blend_->setCurrentIndex(active ? static_cast<int>(active->blend) : 0);
        parent_->clear(); parent_->addItem("Parent: canvas",QString{});
        for(const auto& node:document->layers()) if(node.folder) parent_->addItem("Parent: "+QString::fromStdString(node.name),QString::fromStdString(node.id.text()));
        parent_->setEnabled(!session.busy && active);
        parent_->setCurrentIndex(parent_->findData(active && active->parent ? QString::fromStdString(active->parent->text()) : QString{}));
        queueThumbnails();
        emit editingContextChanged();
    }
}
void MainWindow::runJob(QWidget* page,std::function<JobResult(std::stop_token)> task,std::function<void(const JobResult&)> install) {
    const auto session=sessions_.at(page); if(session->busy) return;
    session->busy=true; session->stop=std::stop_source{}; refresh(page);
    auto* watcher=new QFutureWatcher<JobResult>(this);
    connect(watcher,&QFutureWatcher<JobResult>::finished,this,[this,page,session,watcher,install=std::move(install)] {
        auto result=watcher->result(); watcher->deleteLater(); session->busy=false;
        if(!sessions_.contains(page)) return;
        bool success=result.error.isEmpty();
        try { if(success) install(result); } catch(const std::exception& error) { result.error=QString::fromUtf8(error.what()); success=false; }
        const auto message=success ? result.information : result.error;
        diagnostics_->appendPlainText(message); statusBar()->showMessage(message); refresh(page);
        emit operationFinished(success,message);
        if(success && session->closeAfterSave) { session->closeAfterSave=false; closeDocument(tabs_->indexOf(page)); }
        else if(!success) session->closeAfterSave=false;
    });
    const auto stop=session->stop.get_token();
    watcher->setFuture(QtConcurrent::run(&jobs_,[task=std::move(task),stop,resources=resources_] {
        try {
            auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true,stop);
            if(!permit) throw std::runtime_error("Operation canceled");
            // Initial allowance for graph/SQLite/zstd/fill scratch. Import has
            // separate encoded/decode reservations; final library peaks still
            // require measurement, not inference from these configured limits.
            auto memory=resources->memory->require(64*1024*1024);
            return task(stop);
        } catch(const std::exception& error) { JobResult result; result.error=QString::fromUtf8(error.what()); return result; }
    }));
}
void MainWindow::openPath(const QString& path) {
    addDocument(); auto* page=tabs_->currentWidget(); const auto session=sessions_.at(page); const auto tiles=tileStore_;
    const bool project=QFileInfo(path).suffix().compare("cproj",Qt::CaseInsensitive)==0;
    runJob(page,[path,project,tiles](std::stop_token stop) {
        JobResult result;
        if(project) { auto loaded=io::loadProject(path,*tiles,stop); result.document=loaded.document; result.identity=loaded.identity; result.information="Project reopened with canonical pixels preserved"; }
        else { auto loaded=io::importImage(path,*tiles,stop); result.document=loaded.document; result.information=loaded.information; }
        return result;
    },[this,page,session,path,project](const JobResult& result) {
        session->history=std::make_unique<engine::DocumentHistory>(result.document,100,512*1024*1024);
        if(!project) session->history->markUnsaved();
        session->path=project ? QFileInfo(path).absoluteFilePath() : QString{}; session->identity=result.identity;
        session->title=QFileInfo(path).fileName(); session->imageActive=true; refresh(page,true);
    });
}
void MainWindow::importLayer(const QString& path) {
    auto* page=tabs_->currentWidget(); if(!page || currentBusy()) return;
    const auto session=sessions_.at(page); const auto input=session->history->current(); const auto selected=session->history->selectedLayer(); const auto tiles=tileStore_;
    runJob(page,[path,input,selected,tiles](std::stop_token stop) {
        auto imported=io::importImage(path,*tiles,stop); auto node=imported.document->singleLayer(); auto nodes=input->layers();
        // EditorSession.insert: default imports are pixel-aligned and centered
        // on the existing canvas, at the top of the selected parent folder.
        node.localToDocument.tx=std::floor((input->width-imported.document->width)/2.0);
        node.localToDocument.ty=std::floor((input->height-imported.document->height)/2.0);
        if(selected) {
            const auto& active=input->layer(*selected); node.parent=active.folder ? selected : active.parent;
        }
        node.siblingOrder=static_cast<int>(std::count_if(nodes.begin(),nodes.end(),[&](const auto& other){return other.parent==node.parent;}));
        nodes.push_back(std::move(node)); auto edit=std::make_shared<engine::EditTransaction>(input); edit->setLayers(std::move(nodes));
        JobResult result; result.edit=std::move(edit); result.document=imported.document; result.information=imported.information; return result;
    },[session](const JobResult& result) {
        session->history->commit(*result.edit,"Import layer",result.document->singleLayer().id); session->imageActive=true;
    });
}
void MainWindow::fillCurrent(engine::Pixel straightSrgb) {
    auto* page=tabs_->currentWidget(); if(!page || currentBusy()) return;
    const auto session=sessions_.at(page); const auto input=session->history->current(); const auto tiles=tileStore_;
    const auto target=session->history->selectedLayer(); if(!target || !input->layer(*target).raster) return;
    runJob(page,[input,tiles,straightSrgb,target](std::stop_token stop) {
        JobResult result; auto edit=std::make_shared<engine::EditTransaction>(input,*target);
        const auto value=engine::fromStraightSrgb(straightSrgb); const auto extent=input->layer(*target).raster->extent;
        for(auto y=engine::floorTile(extent.y);y<=engine::floorTile(extent.y+extent.height-1);++y)
            for(auto x=engine::floorTile(extent.x);x<=engine::floorTile(extent.x+extent.width-1);++x) {
                if(stop.stop_requested()) throw std::runtime_error("Edit cancelled");
                const auto region=engine::tileExtent(extent,{x,y}); edit->replace({x,y},tiles->constant(static_cast<int>(region.width),static_cast<int>(region.height),value));
            }
        result.edit=std::move(edit); result.information="Layer pixels updated as one undoable transaction"; return result;
    },[session](const JobResult& result) { session->history->commit(*result.edit,"Fill layer"); session->imageActive=true; });
}
void MainWindow::runEdit(std::string name,std::function<engine::EditTransaction(engine::DocumentPtr,std::optional<engine::Id>,std::stop_token)> task) {
    if(currentBusy() || !currentDocument()) return;
    auto* page=tabs_->currentWidget(); const auto session=sessions_.at(page);
    const auto input=currentDocument(); const auto selected=activeLayer();
    runJob(page,[input,selected,task=std::move(task),name](std::stop_token stop){
        JobResult result;result.edit=std::make_shared<engine::EditTransaction>(task(input,selected,stop));result.information=QString::fromStdString(name);return result;
    },[session,name](const JobResult& result){session->history->commit(*result.edit,name);session->imageActive=true;});
}
void MainWindow::applyStroke(std::vector<engine::Coordinate> points,engine::BrushSettings settings) {
    runEdit(settings.paintMask ? "Paint mask" : settings.erasing ? "Erase" : "Brush stroke",[tiles=tileStore_,points=std::move(points),settings](auto input,auto target,auto stop){if(!target) throw std::invalid_argument("Select a raster layer");if(settings.paintMask) return engine::paintLayerMask(input,*target,*tiles,points,settings,!settings.erasing && settings.color.r>=.5f,stop);return engine::brushStroke(input,*target,*tiles,points,settings,stop);});
}
void MainWindow::applyRetouchStroke(std::vector<engine::Coordinate> points,engine::BrushSettings settings,engine::Coordinate anchor,bool heal,double radius) {
    runEdit(heal ? "Healing stroke" : "Clone stroke",[tiles=tileStore_,points=std::move(points),settings,anchor,heal,radius](auto input,auto target,auto stop){
        if(!target) throw std::invalid_argument("Select a raster layer");
        const auto& layer=input->layer(*target);
        engine::RetouchStroke stroke;stroke.kind=heal ? engine::RetouchKind::Heal : engine::RetouchKind::Clone;
        stroke.sourceAnchor=anchor;stroke.points=points;stroke.diameter=settings.diameter;stroke.hardness=settings.hardness;stroke.opacity=settings.opacity;stroke.healingRadius=radius;
        stroke.localToDocument=layer.localToDocument;stroke.selection=input->selection;stroke.canvasWidth=input->width;stroke.canvasHeight=input->height;
        auto strokes=layer.retouch ? layer.retouch->strokes : std::vector<engine::RetouchStroke>{};strokes.push_back(std::move(stroke));
        return engine::setRetouchStrokes(input,*target,*tiles,std::move(strokes),stop);
    });
}
void MainWindow::reviseRetouchStrength(std::size_t index,double opacity) {
    runEdit("Revise retouch strength",[tiles=tileStore_,index,opacity](auto input,auto target,auto stop){
        if(!target || !input->layer(*target).retouch) throw std::invalid_argument("Select a retouched layer");
        auto strokes=input->layer(*target).retouch->strokes;if(index>=strokes.size()) throw std::out_of_range("Retouch stroke index");strokes[index].opacity=opacity;
        return engine::setRetouchStrokes(input,*target,*tiles,std::move(strokes),stop);
    });
}
void MainWindow::transformCurrent(engine::TransformParameters parameters) {
    runEdit("Transform layer",[parameters](auto input,auto target,auto){if(!target) throw std::invalid_argument("Select a raster layer");return engine::transformLayer(input,*target,parameters);});
}
void MainWindow::cropCurrent(engine::Extent bounds) {runEdit("Crop canvas",[bounds](auto input,auto,auto){return engine::cropDocument(input,bounds);});}
void MainWindow::resampleCurrent(int width,int height,engine::Sampling sampling) {runEdit("Resample image",[tiles=tileStore_,width,height,sampling](auto input,auto,auto stop){return engine::resampleDocument(input,*tiles,width,height,sampling,stop);});}
void MainWindow::featherMaskCurrent(double radius) {runEdit("Feather layer mask",[tiles=tileStore_,radius](auto input,auto target,auto stop){if(!target) throw std::invalid_argument("Select a raster layer");return engine::featherLayerMask(input,*target,*tiles,radius,stop);});}
void MainWindow::resizeCurrent(int width,int height) {runEdit("Resize image",[width,height](auto input,auto,auto){return engine::resizeDocument(input,width,height);});}
void MainWindow::setSelectionCurrent(std::optional<engine::Selection> selection) {runEdit("Change selection",[selection](auto input,auto,auto){return engine::selectDocument(input,selection);});}
void MainWindow::createMaskCurrent(bool fromSelection) {runEdit("Create layer mask",[tiles=tileStore_,fromSelection](auto input,auto target,auto stop){if(!target) throw std::invalid_argument("Select a raster layer");return engine::createLayerMask(input,*target,*tiles,fromSelection,stop);});}
void MainWindow::removeMaskCurrent() {runEdit("Remove layer mask",[](auto input,auto target,auto){if(!target) throw std::invalid_argument("Select a raster layer");return engine::removeLayerMask(input,*target);});}
void MainWindow::enableMaskCurrent(bool enabled) {runEdit("Toggle layer mask",[enabled](auto input,auto target,auto){if(!target) throw std::invalid_argument("Select a raster layer");return engine::setLayerMaskEnabled(input,*target,enabled);});}
void MainWindow::rasterizeCurrentAdjustments() {
    runEdit("Rasterize retained edits",[](auto input,auto target,auto) {
        if(!target) throw std::invalid_argument("Select a raster layer");
        return engine::rasterizeLayerAdjustments(input,*target);
    });
}
void MainWindow::editRevisableExposure() {
    if(currentBusy() || !currentDocument() || !activeLayer()) return;
    auto* page=tabs_->currentWidget();const auto session=sessions_.at(page);
    const auto input=session->history->current();const auto target=*session->history->selectedLayer();
    const auto& layer=input->layer(target);if(layer.folder || !layer.raster) return;
    double initial=0;
    if(layer.adjustments) {
        const auto& operations=layer.adjustments->operations;
        if(operations.size()!=1 || operations.front().kind!=engine::AdjustmentKind::Exposure) {
            statusBar()->showMessage("This adjustment stack cannot be revised with the Exposure dialog");return;
        }
        initial=operations.front().value;
    }
    RevisableExposureDialog dialog(initial,this);
    AdjustmentPreview preview(resources_,&jobs_,&dialog);
    std::shared_ptr<engine::EditTransaction> latest;
    session->busy=true;session->stop=std::stop_source{};refresh(page);
    const auto valid=[&] {
        const auto found=sessions_.find(page);
        return found!=sessions_.end() && found->second==session && tabs_->currentWidget()==page &&
            session->history->current()==input && session->history->selectedLayer()==target && !session->stop.stop_requested();
    };
    const auto request=[&](double value) {
        latest.reset();dialog.setPreviewState(true);
        if(!valid()) {preview.cancel();dialog.reject();return;}
        engine::AdjustmentParameters operation;operation.kind=engine::AdjustmentKind::Exposure;operation.value=value;
        preview.request(input,target,{operation});
    };
    connect(&dialog,&RevisableExposureDialog::exposureChanged,&dialog,request);
    connect(&preview,&AdjustmentPreview::previewReady,&dialog,[&](engine::DocumentPtr document,std::shared_ptr<engine::EditTransaction> edit,engine::RgbHistogram histogram) {
        if(!valid()) {preview.cancel();dialog.reject();return;}
        latest=std::move(edit);
        if(auto* canvas=page->property("canvas").value<Canvas*>()) canvas->setDocument(std::move(document),false);
        dialog.setHistogram(histogram);dialog.setPreviewState(false);
    });
    connect(&preview,&AdjustmentPreview::failed,&dialog,[&](const QString& error) {
        latest.reset();dialog.setPreviewState(false,error);
        diagnostics_->appendPlainText(error);
    });
    connect(&dialog,&QDialog::finished,&preview,[&]{preview.cancel();});
    connect(this,&MainWindow::editingContextChanged,&dialog,[&]{if(!valid()){preview.cancel();dialog.reject();}});
    request(initial);
    const bool accepted=dialog.exec()==QDialog::Accepted;
    preview.cancel();
    bool committed=false;
    if(accepted && latest && valid()) {
        try {committed=session->history->commit(*latest,"Revisable Exposure",target);session->imageActive=true;}
        catch(const std::exception& error) {const auto message=QString::fromUtf8(error.what());diagnostics_->appendPlainText(message);statusBar()->showMessage(message);emit operationFinished(false,message);}
    }
    session->busy=false;
    if(sessions_.contains(page) && sessions_.at(page)==session) {
        if(auto* canvas=page->property("canvas").value<Canvas*>()) canvas->setDocument(session->history->current(),false);
        refresh(page);
    }
    if(committed) {statusBar()->showMessage("Revisable Exposure");emit operationFinished(true,"Revisable Exposure");}
}
void MainWindow::editRevisableAdjustments() {
    if(currentBusy() || !currentDocument() || !activeLayer()) return;
    auto* page=tabs_->currentWidget();const auto session=sessions_.at(page);
    const auto input=session->history->current();const auto target=*session->history->selectedLayer();
    const auto& layer=input->layer(target);if(layer.folder || !layer.raster) return;
    std::vector<engine::AdjustmentParameters> initial;
    if(layer.adjustments) initial=layer.adjustments->operations;
    else initial.push_back({engine::AdjustmentKind::Exposure,0});
    RevisableAdjustmentsDialog dialog(initial,this);
    AdjustmentPreview preview(resources_,&jobs_,&dialog);
    std::shared_ptr<engine::EditTransaction> latest;
    session->busy=true;session->stop=std::stop_source{};refresh(page);
    const auto valid=[&] {
        const auto found=sessions_.find(page);
        return found!=sessions_.end() && found->second==session && tabs_->currentWidget()==page &&
            session->history->current()==input && session->history->selectedLayer()==target && !session->stop.stop_requested();
    };
    const auto request=[&] {
        latest.reset();dialog.setPreviewState(true);
        if(!valid()) {preview.cancel();dialog.reject();return;}
        preview.request(input,target,dialog.operations());
    };
    connect(&dialog,&RevisableAdjustmentsDialog::operationsChanged,&dialog,request);
    connect(&preview,&AdjustmentPreview::previewReady,&dialog,[&](engine::DocumentPtr document,std::shared_ptr<engine::EditTransaction> edit,engine::RgbHistogram histogram) {
        if(!valid()) {preview.cancel();dialog.reject();return;}
        latest=std::move(edit);
        if(auto* canvas=page->property("canvas").value<Canvas*>()) canvas->setDocument(std::move(document),false);
        dialog.setHistogram(histogram);dialog.setPreviewState(false);
    });
    connect(&preview,&AdjustmentPreview::failed,&dialog,[&](const QString& error) {
        latest.reset();dialog.setPreviewState(false,error);
        diagnostics_->appendPlainText(error);
    });
    connect(&dialog,&QDialog::finished,&preview,[&]{preview.cancel();});
    connect(this,&MainWindow::editingContextChanged,&dialog,[&]{if(!valid()){preview.cancel();dialog.reject();}});
    request();
    const bool accepted=dialog.exec()==QDialog::Accepted;
    preview.cancel();
    bool committed=false;
    if(accepted && latest && valid()) {
        try {committed=session->history->commit(*latest,"Revisable adjustments",target);session->imageActive=true;}
        catch(const std::exception& error) {const auto message=QString::fromUtf8(error.what());diagnostics_->appendPlainText(message);statusBar()->showMessage(message);emit operationFinished(false,message);}
    }
    session->busy=false;
    if(sessions_.contains(page) && sessions_.at(page)==session) {
        if(auto* canvas=page->property("canvas").value<Canvas*>()) canvas->setDocument(session->history->current(),false);
        refresh(page);
    }
    if(committed) {statusBar()->showMessage("Revisable adjustments");emit operationFinished(true,"Revisable adjustments");}
}
void MainWindow::applyAdjustment(engine::AdjustmentParameters parameters) {
    if(!currentDocument() || currentBusy()) return;
    try {
        if(engine::adjustmentIsNeutral(parameters)) return;
    } catch(const std::exception& error) {
        const auto message=QString::fromUtf8(error.what());
        diagnostics_->appendPlainText(message);statusBar()->showMessage(message);
        emit operationFinished(false,message);return;
    }
    const char* name="Adjust pixels";
    switch(parameters.kind) {
        case engine::AdjustmentKind::Exposure:name="Exposure";break;
        case engine::AdjustmentKind::Brightness:name="Brightness";break;
        case engine::AdjustmentKind::Contrast:name="Contrast";break;
        case engine::AdjustmentKind::Saturation:name="Saturation";break;
        case engine::AdjustmentKind::Levels:name="Levels";break;
        case engine::AdjustmentKind::Curves:name="Curves";break;
        case engine::AdjustmentKind::ColorBalance:name="Color Balance";break;
    }
    runEdit(name,[tiles=tileStore_,parameters=std::move(parameters)](auto input,auto target,auto stop){
        if(!target) throw std::invalid_argument("Select a raster layer");
        return engine::adjustLayer(input,*target,*tiles,parameters,stop);
    });
}
void MainWindow::exportCurrentTo(const QString& suppliedPath,io::ExportOptions options) {
    if(currentBusy() || !currentDocument()) return;
    auto* page=tabs_->currentWidget();const auto input=currentDocument();const auto path=QFileInfo(suppliedPath).absoluteFilePath();
    options.memory=resources_->memory;
    runJob(page,[input,path,options](std::stop_token stop) mutable {options.stop=stop;io::exportImage(path,input,options);JobResult result;result.information="Image exported: "+path;return result;},[](const JobResult&){});
}
void MainWindow::undoCurrent() { if(!currentBusy() && currentDocument()) { sessions_.at(tabs_->currentWidget())->history->undo(); refresh(tabs_->currentWidget()); } }
void MainWindow::redoCurrent() { if(!currentBusy() && currentDocument()) { sessions_.at(tabs_->currentWidget())->history->redo(); refresh(tabs_->currentWidget()); } }
void MainWindow::saveCurrentTo(const QString& suppliedPath,bool overwriteConfirmed) {
    auto* page=tabs_->currentWidget(); if(!page || currentBusy()) return;
    const auto path=QFileInfo(suppliedPath).absoluteFilePath(); const auto session=sessions_.at(page); const auto captured=session->history->current();
    const auto previousPath=session->path; const auto previousIdentity=session->identity;
    runJob(page,[previousPath,previousIdentity,path,captured,overwriteConfirmed,memory=tileStore_->memoryAdmission()](std::stop_token stop) {
        const auto expected=path==previousPath && !overwriteConfirmed ? previousIdentity : io::fileIdentity(path);
        if(path!=previousPath && expected && !overwriteConfirmed) throw std::runtime_error("Destination exists; confirm replacement with Save As");
        JobResult result; result.identity=io::saveProject(path,captured,{expected,stop,{},memory}); result.information="Project saved durably"; return result;
    },[session,path,captured](const JobResult& result) { session->history->markSaved(captured); session->path=path; session->identity=result.identity; session->title=QFileInfo(path).fileName(); });
}
bool MainWindow::chooseSave(bool saveAs) {
    auto* page=tabs_->currentWidget(); if(!page || currentBusy()) return false;
    const auto session=sessions_.at(page); auto path=saveAs ? QString{} : session->path;
    bool confirmed=false;
    if(path.isEmpty()) {
        path=QFileDialog::getSaveFileName(this,"Save Lumaire Studio project",session->title+".cproj","Lumaire Studio project (*.cproj)");
        if(path.isEmpty()) return false;
        // Add extension before checking identity; the file dialog only confirms
        // the exact chosen name, so explicitly confirm a newly suffixed target.
        if(QFileInfo(path).suffix().compare("cproj",Qt::CaseInsensitive)!=0) {
            path+=".cproj";
            if(QFileInfo::exists(path) && QMessageBox::question(this,"Replace project?","The destination exists. Replace it?")!=QMessageBox::Yes) return false;
        }
        confirmed=true;
    }
    saveCurrentTo(path,confirmed); return true;
}
bool MainWindow::mayClose(QWidget* page) {
    const auto session=sessions_.at(page);
    if(session->busy) { session->stop.request_stop(); statusBar()->showMessage("Cancelling this operation. Close again when it finishes."); emit editingContextChanged(); return false; }
    if(!session->history->dirty()) return true;
    const auto choice=QMessageBox::warning(this,"Unsaved changes","Save changes to "+session->title+"?",QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
    if(choice==QMessageBox::Discard) return true;
    if(choice==QMessageBox::Save) { tabs_->setCurrentWidget(page); if(chooseSave(false)) session->closeAfterSave=true; }
    return false;
}
void MainWindow::closeEvent(QCloseEvent* event) {
    for(int index=0;index<tabs_->count();++index) if(!mayClose(tabs_->widget(index))) { event->ignore(); return; }
    event->accept();
}
void MainWindow::writeTiming(const QString& path) const {
    if(path.isEmpty()) return;
    QFile output(path); if(!output.open(QIODevice::WriteOnly|QIODevice::Truncate)) throw std::runtime_error("Cannot write timing JSON");
    const auto bytes=QJsonDocument(QJsonObject{{"schema_version",1},{"milestone","M3-in-progress"},{"platform",QGuiApplication::platformName()},
        {"clock","CLOCK_MONOTONIC_RAW"},{"presentation_feedback_available",false},{"capture_limit",10000},
        {"events",timings_}}).toJson();
    if(output.write(bytes)!=bytes.size()) throw std::runtime_error("Incomplete timing JSON write");
}
}
