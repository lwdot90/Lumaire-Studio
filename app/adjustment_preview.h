#pragma once
#include "core/editor_commands.h"
#include "core/histogram.h"
#include "core/resources.h"
#include <QObject>
#include <QThreadPool>
#include <memory>

namespace compositor {
// GUI-thread owner. Evaluation runs on the supplied pool with shared admission.
class AdjustmentPreview final:public QObject {
    Q_OBJECT
public:
    AdjustmentPreview(std::shared_ptr<RuntimeResources> resources,QThreadPool* pool,QObject* parent=nullptr);
    ~AdjustmentPreview() override;
    void request(engine::DocumentPtr input,const engine::Id& target,std::vector<engine::AdjustmentParameters> operations);
    void cancel();
signals:
    void previewReady(engine::DocumentPtr document,std::shared_ptr<engine::EditTransaction> edit,engine::RgbHistogram histogram);
    void failed(QString error);
private:
    struct State;
    std::unique_ptr<State> state_;
    void startPending();
};
}
