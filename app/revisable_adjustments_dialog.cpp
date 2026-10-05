#include "app/revisable_adjustments_dialog.h"
#include "app/photo_adjustment_dialog.h"
#include "app/histogram_widget.h"
#include "core/editor_commands.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace compositor {
namespace {
const std::array<const char*,7> names{"Exposure","Brightness","Contrast","Saturation","Levels","Curves","Color Balance"};
QString title(const engine::AdjustmentParameters& parameter) {
    return QString::fromLatin1(names.at(static_cast<std::size_t>(parameter.kind)));
}
}
RevisableAdjustmentsDialog::RevisableAdjustmentsDialog(std::vector<engine::AdjustmentParameters> initial,QWidget* parent)
    :QDialog(parent),operations_(std::move(initial)) {
    if(operations_.size()>16) throw std::invalid_argument("Adjustment stack exceeds 16 operations");
    for(const auto& parameter:operations_) engine::adjustmentIsNeutral(parameter);
    setObjectName("revisableAdjustmentsDialog");setWindowTitle("Revisable Adjustments");resize(700,640);
    auto* layout=new QVBoxLayout(this);
    auto* description=new QLabel("Operations run in order over the entire retained source. The layer mask and opacity still control visibility. Remove all operations to restore the source.",this);
    description->setWordWrap(true);layout->addWidget(description);
    auto* body=new QHBoxLayout;layout->addLayout(body,1);
    auto* stackLayout=new QVBoxLayout;body->addLayout(stackLayout);
    list_=new QListWidget(this);list_->setObjectName("revisableAdjustmentsStack");list_->setAccessibleName("Ordered adjustment operations");list_->setMinimumWidth(170);stackLayout->addWidget(list_,1);
    kind_=new QComboBox(this);kind_->setObjectName("revisableAdjustmentsKind");kind_->setAccessibleName("Adjustment to add");
    for(const auto* name:names) kind_->addItem(QString::fromLatin1(name));
    stackLayout->addWidget(kind_);
    add_=new QPushButton("Add operation",this);add_->setObjectName("revisableAdjustmentsAdd");stackLayout->addWidget(add_);
    remove_=new QPushButton("Remove selected",this);remove_->setObjectName("revisableAdjustmentsRemove");stackLayout->addWidget(remove_);
    auto* order=new QHBoxLayout;up_=new QPushButton("Move up",this);down_=new QPushButton("Move down",this);
    up_->setObjectName("revisableAdjustmentsUp");down_->setObjectName("revisableAdjustmentsDown");
    for(auto* button:{add_,remove_,up_,down_}) button->setAutoDefault(false);
    order->addWidget(up_);order->addWidget(down_);stackLayout->addLayout(order);
    editorHost_=new QWidget(this);editorLayout_=new QVBoxLayout(editorHost_);editorLayout_->setContentsMargins(0,0,0,0);
    auto* editorScroll=new QScrollArea(this);editorScroll->setObjectName("revisableAdjustmentsEditorScroll");
    editorScroll->setWidgetResizable(true);editorScroll->setFrameShape(QFrame::NoFrame);
    editorScroll->setMinimumSize(340,220);editorScroll->setWidget(editorHost_);body->addWidget(editorScroll,1);
    histogram_=new HistogramWidget(this);layout->addWidget(histogram_);
    status_=new QLabel(this);status_->setObjectName("revisableAdjustmentsStatus");status_->setTextFormat(Qt::PlainText);status_->setWordWrap(true);layout->addWidget(status_);
    buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);buttons_->setObjectName("revisableAdjustmentsButtons");layout->addWidget(buttons_);
    connect(buttons_,&QDialogButtonBox::accepted,this,&RevisableAdjustmentsDialog::accept);connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(list_,&QListWidget::currentRowChanged,this,&RevisableAdjustmentsDialog::selectOperation);
    connect(add_,&QPushButton::clicked,this,[this] {
        if(operations_.size()>=16 || (editor_ && !editor_->parametersValid())) return;
        engine::AdjustmentParameters parameter;parameter.kind=static_cast<engine::AdjustmentKind>(kind_->currentIndex());
        parameter.value=parameter.kind==engine::AdjustmentKind::Saturation ? 1 : 0;
        operations_.push_back(std::move(parameter));refreshList(static_cast<int>(operations_.size())-1);changed();
    });
    connect(remove_,&QPushButton::clicked,this,[this] {
        const auto row=list_->currentRow();if(row<0) return;
        operations_.erase(operations_.begin()+row);refreshList(std::min(row,static_cast<int>(operations_.size())-1));changed();
    });
    connect(up_,&QPushButton::clicked,this,[this]{moveOperation(-1);});connect(down_,&QPushButton::clicked,this,[this]{moveOperation(1);});
    refreshList(operations_.empty() ? -1 : 0);updateControls();
    if(auto* targetScreen=screen()) {
        const auto available=targetScreen->availableGeometry().size();
        resize(std::min(700,available.width()-48),std::min(640,available.height()-64));
    }
}
std::vector<engine::AdjustmentParameters> RevisableAdjustmentsDialog::operations() const {return operations_;}
void RevisableAdjustmentsDialog::refreshList(int selection) {
    {const QSignalBlocker blocker(list_);list_->clear();for(const auto& parameter:operations_) list_->addItem(title(parameter));list_->setCurrentRow(selection);}
    selectOperation(selection);
}
void RevisableAdjustmentsDialog::selectOperation(int row) {
    delete editor_;editor_=nullptr;
    if(row>=0 && row<static_cast<int>(operations_.size())) {
        editor_=new PhotoAdjustmentDialog(operations_[static_cast<std::size_t>(row)],editorHost_,true);editorLayout_->addWidget(editor_,0,Qt::AlignTop);editor_->show();
        connect(editor_,&PhotoAdjustmentDialog::parametersChanged,this,[this,row](engine::AdjustmentParameters parameter) {
            if(row!=list_->currentRow()) return;
            if(parameter==operations_[static_cast<std::size_t>(row)]) {updateControls();return;}
            operations_[static_cast<std::size_t>(row)]=std::move(parameter);changed();
        });
        connect(editor_,&PhotoAdjustmentDialog::validityChanged,this,[this](bool) {updateControls();});
    }
    updateControls();
}
void RevisableAdjustmentsDialog::updateControls() {
    const auto row=list_->currentRow();const bool valid=!editor_ || editor_->parametersValid();
    list_->setEnabled(valid);kind_->setEnabled(valid && operations_.size()<16);add_->setEnabled(valid && operations_.size()<16);
    remove_->setEnabled(row>=0);up_->setEnabled(valid && row>0);down_->setEnabled(valid && row>=0 && row+1<static_cast<int>(operations_.size()));
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid && !pending_ && error_.isEmpty());
    status_->setText(!valid ? "Correct the selected operation's controls or remove it." : !error_.isEmpty() ? error_ : pending_ ? "Updating preview…" : "Preview ready");
}
void RevisableAdjustmentsDialog::changed() {error_.clear();pending_=true;updateControls();emit operationsChanged(operations_);}
void RevisableAdjustmentsDialog::moveOperation(int direction) {
    const auto row=list_->currentRow(),next=row+direction;
    if(row<0 || next<0 || next>=static_cast<int>(operations_.size()) || (editor_ && !editor_->parametersValid())) return;
    std::swap(operations_[static_cast<std::size_t>(row)],operations_[static_cast<std::size_t>(next)]);refreshList(next);changed();
}
void RevisableAdjustmentsDialog::setPreviewState(bool pending,const QString& error) {pending_=pending;error_=error;updateControls();}
void RevisableAdjustmentsDialog::setHistogram(const engine::RgbHistogram& histogram) {histogram_->setHistogram(histogram);}
void RevisableAdjustmentsDialog::accept() {
    if(editor_) editor_->commitTypedControls();
    updateControls();if(buttons_->button(QDialogButtonBox::Ok)->isEnabled()) QDialog::accept();
}
}
