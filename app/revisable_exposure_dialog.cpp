#include "app/revisable_exposure_dialog.h"
#include "app/histogram_widget.h"
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>
#include <stdexcept>

namespace compositor {
RevisableExposureDialog::RevisableExposureDialog(double initialExposure,QWidget* parent):QDialog(parent),exposure_(initialExposure) {
    if(!std::isfinite(initialExposure) || initialExposure<-8 || initialExposure>8)
        throw std::invalid_argument("Exposure must be finite and between -8 and 8 stops");
    setObjectName("revisableExposureDialog");setWindowTitle("Revisable Exposure");resize(420,330);
    auto* layout=new QVBoxLayout(this);
    auto* description=new QLabel("Adjust the entire selected layer. Its mask and layer opacity remain visible. Source pixels are retained, so you can revise exposure after saving and reopening.",this);
    description->setWordWrap(true);layout->addWidget(description);
    auto* form=new QFormLayout;
    value_=new QDoubleSpinBox(this);value_->setObjectName("revisableExposureValue");value_->setAccessibleName("Exposure in stops");
    value_->setRange(-8,8);value_->setDecimals(2);value_->setSingleStep(.1);value_->setSuffix(" EV");value_->setButtonSymbols(QAbstractSpinBox::NoButtons);value_->setValue(initialExposure);
    value_->setKeyboardTracking(false);form->addRow("Exposure",value_);layout->addLayout(form);
    slider_=new QSlider(Qt::Horizontal,this);slider_->setObjectName("revisableExposureSlider");slider_->setAccessibleName("Exposure in stops");slider_->setRange(-800,800);slider_->setValue(qRound(initialExposure*100));layout->addWidget(slider_);
    auto* reset=new QPushButton("Reset to 0 EV",this);reset->setObjectName("revisableExposureReset");reset->setAccessibleName("Reset exposure to zero stops");layout->addWidget(reset);
    histogram_=new HistogramWidget(this);histogram_->setObjectName("revisableExposureHistogram");layout->addWidget(histogram_);
    status_=new QLabel("Preview ready",this);status_->setObjectName("revisableExposureStatus");status_->setTextFormat(Qt::PlainText);status_->setWordWrap(true);layout->addWidget(status_);
    buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);buttons_->setObjectName("revisableExposureButtons");layout->addWidget(buttons_);
    connect(buttons_,&QDialogButtonBox::accepted,this,&RevisableExposureDialog::accept);
    connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(value_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        const QSignalBlocker blocker(slider_);slider_->setValue(qRound(value*100));
        if(value==exposure_) return;
        exposure_=value;setPreviewState(true);emit exposureChanged(value);
    });
    connect(slider_,&QSlider::valueChanged,this,[this](int value){value_->setValue(value/100.);});
    connect(reset,&QPushButton::clicked,this,[this]{
        // A rounded initial display can already show zero while the persisted
        // parameter is nonzero. Reset must still change that exact parameter.
        if(value_->value()==0 && exposure_!=0) {exposure_=0;setPreviewState(true);emit exposureChanged(0);}
        else value_->setValue(0);
    });
}
double RevisableExposureDialog::exposure() const {return exposure_;}
void RevisableExposureDialog::setPreviewState(bool pending,const QString& error) {
    ready_=!pending && error.isEmpty();buttons_->button(QDialogButtonBox::Ok)->setEnabled(ready_);
    status_->setText(!error.isEmpty() ? error : pending ? QString("Updating preview…") : QString("Preview ready"));
}
void RevisableExposureDialog::setHistogram(const engine::RgbHistogram& histogram) {histogram_->setHistogram(histogram);}
void RevisableExposureDialog::accept() {
    value_->interpretText();
    if(ready_) QDialog::accept();
}
}
