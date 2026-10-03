#pragma once
#include "platform/canvas.h"
#include <QMainWindow>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QJsonArray>
#include <QThreadPool>
#include <QLabel>
#include <QTreeWidget>
#include <QDoubleSpinBox>
#include <QComboBox>
#include "io/project_store.h"
#include "io/image_export.h"
#include "core/editor_commands.h"
#include "app/tool_controller.h"
#include "rendering/thumbnail_worker.h"
#include <QTimer>
#include <map>
#include <set>

namespace compositor {
class MainWindow final:public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QVulkanInstance* instance,bool cpu,QString deviceName={},
               std::shared_ptr<RuntimeResources> resources=defaultRuntimeResources());
    ~MainWindow() override;
    Canvas* currentCanvas() const;
    void addDocument();
    void closeDocument(int index);
    QTabWidget* tabs() const { return tabs_; }
    void writeTiming(const QString& path) const;
    void openPath(const QString& path);
    void importLayer(const QString& path);
    void saveCurrentTo(const QString& path,bool overwriteConfirmed=false);
    void fillCurrent(engine::Pixel straightSrgb);
    void applyStroke(std::vector<engine::Coordinate>,engine::BrushSettings);
    void transformCurrent(engine::TransformParameters);
    void cropCurrent(engine::Extent);
    void resizeCurrent(int,int);
    void setSelectionCurrent(std::optional<engine::Selection>);
    void createMaskCurrent(bool);
    void removeMaskCurrent();
    void enableMaskCurrent(bool);
    void applyAdjustment(engine::AdjustmentParameters);
    void exportCurrentTo(const QString&,io::ExportOptions={});
    void addLayer(bool folder=false);
    void duplicateLayer();
    void deleteLayer();
    void moveLayer(int offset);
    void reparentLayer(std::optional<engine::Id> parent);
    bool placeLayer(const engine::Id& id,std::optional<engine::Id> parent,int order);
    void selectLayer(const engine::Id& id);
    void setLayerAppearance(bool visible,float opacity,engine::BlendMode blend);
    void setLayerSampling(engine::Sampling sampling);
    std::optional<engine::Id> activeLayer() const;
    void undoCurrent();
    void redoCurrent();
    engine::DocumentPtr currentDocument() const;
    bool currentDirty() const;
    bool currentBusy() const;
signals:
    void operationFinished(bool success,QString message);
    void editingContextChanged();
protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched,QEvent* event) override;
private:
    struct Session {
        std::unique_ptr<engine::DocumentHistory> history;
        QString path,title;
        std::optional<io::FileIdentity> identity;
        std::stop_source stop;
        bool busy=false,imageActive=false,closeAfterSave=false;
    };
    struct JobResult {
        engine::DocumentPtr document;
        std::shared_ptr<engine::EditTransaction> edit;
        std::optional<io::FileIdentity> identity;
        QString information,error;
    };
    void runJob(QWidget* page,std::function<JobResult(std::stop_token)> task,std::function<void(const JobResult&)> install);
    void runEdit(std::string name,std::function<engine::EditTransaction(engine::DocumentPtr,std::optional<engine::Id>,std::stop_token)> task);
    void refresh(QWidget* page,bool fit=false);
    void queueThumbnails();
    void requestThumbnails();
    void commitLayers(std::vector<engine::LayerNode> nodes,const std::string& name,std::optional<engine::Id> selected);
    bool chooseSave(bool saveAs);
    bool mayClose(QWidget* page);
    void attachCanvas(QWidget* page,bool cpu,const Viewport* previous=nullptr,QString reason={});
    QVulkanInstance* instance_;
    bool cpu_;
    QString deviceName_;
    QTabWidget* tabs_;
    QPlainTextEdit* diagnostics_;
    QJsonArray timings_;
    std::shared_ptr<RuntimeResources> resources_;
    std::shared_ptr<engine::TileStore> tileStore_;
    std::map<QWidget*,std::shared_ptr<Session>> sessions_;
    QThreadPool jobs_;
    QAction *undoAction_=nullptr,*redoAction_=nullptr;
    QLabel* layerInfo_=nullptr;
    QLabel* zoomInfo_=nullptr;
    QTreeWidget* layerTree_=nullptr;
    std::map<std::string,QTreeWidgetItem*> layerItems_;
    std::set<std::string> thumbnailItems_;
    std::map<std::string,qint64> thumbnailImageKeys_;
    QTimer* thumbnailTimer_=nullptr;
    std::unique_ptr<ThumbnailWorker> thumbnails_;
    std::uint64_t thumbnailGeneration_=0;
    QDoubleSpinBox* opacity_=nullptr;
    QComboBox* blend_=nullptr;
    QComboBox* sampling_=nullptr;
    QComboBox* parent_=nullptr;
    std::unique_ptr<ToolController> tools_;
    int nextDocument_=1;
};
}
