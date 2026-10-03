#include "app/job_progress_widget.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

namespace compositor {
JobProgressWidget::JobProgressWidget(QWidget* parent):QWidget(parent) {
    setObjectName("jobProgressWidget");
    setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);
    auto* layout=new QHBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
    status_=new QLabel(this);status_->setObjectName("operationStatusLabel");
    status_->setAccessibleName("Operation status");layout->addWidget(status_);
    progress_=new QProgressBar(this);progress_->setObjectName("operationProgress");
    progress_->setRange(0,0);progress_->setTextVisible(false);
    progress_->setFixedSize(88,14);progress_->setAccessibleName("Operation progress");
    progress_->setAccessibleDescription("An operation is running; progress is indeterminate.");
    layout->addWidget(progress_);
    cancel_=new QPushButton(this);cancel_->setObjectName("cancelOperationButton");cancel_->setText("Cancel");
    cancel_->setToolTip("Cancel current operation");cancel_->setAccessibleName("Cancel current operation");
    layout->addWidget(cancel_);
    connect(cancel_,&QPushButton::clicked,this,[this] {
        if(!busy_ || cancelling_) return;
        cancelling_=true;refresh();emit cancelRequested();
    });
    refresh();
}
void JobProgressWidget::setState(bool busy,bool cancelling) {
    if(!busy || !busy_) cancelling_=false;
    busy_=busy;
    if(busy_ && cancelling) cancelling_=true;
    refresh();
}
void JobProgressWidget::refresh() {
    status_->setText(cancelling_ ? "Cancelling…" : "Working…");
    cancel_->setEnabled(busy_ && !cancelling_);
    setVisible(busy_);
}
}
