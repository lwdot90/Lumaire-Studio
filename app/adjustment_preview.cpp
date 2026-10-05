#include "app/adjustment_preview.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace compositor {
namespace {
struct Request {
    engine::DocumentPtr input;
    engine::Id target;
    std::vector<engine::AdjustmentParameters> operations;
    std::uint64_t generation;
};
struct Result {
    engine::DocumentPtr document;
    std::shared_ptr<engine::EditTransaction> edit;
    engine::RgbHistogram histogram;
    QString error;
};
std::int64_t previewRevision(const engine::DocumentSnapshot& document) {
    auto revision=document.revision;
    for(const auto& layer:document.layers()) {
        if(layer.raster) revision=std::max(revision,layer.raster->revision);
        if(layer.mask) revision=std::max(revision,layer.mask->revision);
        if(layer.adjustments && layer.adjustments->source)
            revision=std::max(revision,layer.adjustments->source->revision);
    }
    if(revision==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("Document revision exhausted");
    return revision+1;
}
}
struct AdjustmentPreview::State {
    std::shared_ptr<RuntimeResources> resources;
    QThreadPool* pool;
    QTimer* timer;
    std::optional<Request> pending;
    std::stop_source stop;
    std::uint64_t generation=0;
    bool active=false,ready=false;
};

AdjustmentPreview::AdjustmentPreview(std::shared_ptr<RuntimeResources> resources,QThreadPool* pool,QObject* parent)
    :QObject(parent),state_(std::make_unique<State>()) {
    if(!resources || !pool) throw std::invalid_argument("Preview requires shared resources and a worker pool");
    state_->resources=std::move(resources); state_->pool=pool;
    state_->timer=new QTimer(this); state_->timer->setSingleShot(true); state_->timer->setInterval(150);
    connect(state_->timer,&QTimer::timeout,this,[this] {state_->ready=true;startPending();});
}
AdjustmentPreview::~AdjustmentPreview() {cancel();}

void AdjustmentPreview::request(engine::DocumentPtr input,const engine::Id& target,std::vector<engine::AdjustmentParameters> operations) {
    Q_ASSERT(QThread::currentThread()==thread());
    state_->stop.request_stop();
    const auto generation=++state_->generation;
    state_->pending=Request{std::move(input),target,std::move(operations),generation};
    state_->ready=false; state_->timer->start();
}
void AdjustmentPreview::cancel() {
    Q_ASSERT(QThread::currentThread()==thread());
    ++state_->generation; state_->pending.reset(); state_->ready=false;
    state_->timer->stop(); state_->stop.request_stop();
}
void AdjustmentPreview::startPending() {
    if(state_->active || !state_->ready || !state_->pending) return;
    auto request=std::move(*state_->pending); state_->pending.reset(); state_->ready=false;
    state_->stop=std::stop_source{}; state_->active=true;
    const auto stop=state_->stop.get_token();
    auto* watcher=new QFutureWatcher<Result>(this);
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,generation=request.generation] {
        auto result=watcher->result(); watcher->deleteLater(); state_->active=false;
        const QPointer<AdjustmentPreview> alive(this);
        if(generation==state_->generation) {
            if(result.error.isEmpty()) emit previewReady(std::move(result.document),std::move(result.edit),result.histogram);
            else emit failed(result.error);
        }
        if(alive) startPending();
    });
    watcher->setFuture(QtConcurrent::run(state_->pool,[request=std::move(request),stop,resources=state_->resources] {
        Result result;
        try {
            if(!request.input) throw std::invalid_argument("Preview requires a document");
            auto permit=resources->compute.acquire(WorkScheduler::Priority::Processing,true,stop);
            if(!permit || stop.stop_requested()) throw std::runtime_error("Operation canceled");
            auto memory=resources->memory->require(16*1024*1024);
            result.edit=std::make_shared<engine::EditTransaction>(engine::setRevisableAdjustments(request.input,request.target,*resources->tiles,std::move(request.operations),stop));
            result.document=result.edit->finish(previewRevision(*request.input));
            const auto& layer=result.document->layer(request.target);
            if(!layer.raster) throw std::invalid_argument("Select a raster layer");
            result.histogram=engine::histogramLayer(*layer.raster,65536,stop);
            if(stop.stop_requested()) throw std::runtime_error("Operation canceled");
        } catch(const std::exception& error) {
            result.document.reset(); result.edit.reset(); result.error=QString::fromUtf8(error.what());
        } catch(...) {
            result.document.reset(); result.edit.reset(); result.error=QStringLiteral("Adjustment preview failed");
        }
        return result;
    }));
}
}
