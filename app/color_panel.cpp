#include "app/color_panel.h"
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace compositor {
namespace {
class SaturationValue final:public QWidget {
public:
    explicit SaturationValue(QWidget* parent):QWidget(parent) {
        setObjectName("colorSvArea");setFocusPolicy(Qt::StrongFocus);
        setAccessibleName("Color saturation and brightness");
        setAccessibleDescription("Left and right change saturation; up and down change brightness.");
        setMinimumSize(80,64);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    }
    QSize sizeHint() const override {return {220,124};}
    void setValues(double hue,double saturation,double value) {
        hue_=hue;saturation_=saturation;value_=value;update();
    }
    std::function<void(double,double)> changed;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);const QRectF area(1,1,std::max(1,width()-3),std::max(1,height()-3));
        QLinearGradient saturation(area.topLeft(),area.topRight());
        saturation.setColorAt(0,Qt::white);saturation.setColorAt(1,QColor::fromHsvF(static_cast<float>(hue_/360),1,1));
        painter.fillRect(area,saturation);
        QLinearGradient value(area.topLeft(),area.bottomLeft());
        value.setColorAt(0,QColor(0,0,0,0));value.setColorAt(1,Qt::black);painter.fillRect(area,value);
        painter.setPen(QColor(90,90,90));painter.drawRect(area);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF cursor(area.left()+saturation_*area.width(),area.top()+(1-value_)*area.height());
        painter.setPen(QPen(Qt::black,2));painter.drawEllipse(cursor,4,4);
        painter.setPen(QPen(Qt::white,1));painter.drawEllipse(cursor,3,3);
        if(hasFocus()) {painter.setPen(QPen(palette().highlight().color(),1,Qt::DashLine));painter.drawRect(area.adjusted(2,2,-2,-2));}
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton) {event->ignore();return;}
        setFocus(Qt::MouseFocusReason);choose(event->position());event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(event->buttons()&Qt::LeftButton) {choose(event->position());event->accept();}
        else event->ignore();
    }
    void keyPressEvent(QKeyEvent* event) override {
        const double step=(event->modifiers()&Qt::ShiftModifier ? 10. : 1.)/255;
        double saturation=saturation_,value=value_;
        switch(event->key()) {
            case Qt::Key_Left:saturation-=step;break;
            case Qt::Key_Right:saturation+=step;break;
            case Qt::Key_Down:value-=step;break;
            case Qt::Key_Up:value+=step;break;
            default:QWidget::keyPressEvent(event);return;
        }
        if(changed) changed(std::clamp(saturation,0.,1.),std::clamp(value,0.,1.));
        event->accept();
    }
    void focusInEvent(QFocusEvent* event) override {QWidget::focusInEvent(event);update();}
    void focusOutEvent(QFocusEvent* event) override {QWidget::focusOutEvent(event);update();}
private:
    void choose(QPointF point) {
        const double width=std::max(1,this->width()-3),height=std::max(1,this->height()-3);
        if(changed) changed(std::clamp((point.x()-1)/width,0.,1.),std::clamp(1-(point.y()-1)/height,0.,1.));
    }
    double hue_=0,saturation_=0,value_=0;
};
class HueSlider final:public QSlider {
public:
    explicit HueSlider(QWidget* parent):QSlider(Qt::Horizontal,parent) {
        setObjectName("colorHueSlider");setAccessibleName("Color hue");setRange(0,359);setPageStep(15);setSingleStep(1);
        setMinimumHeight(20);setFocusPolicy(Qt::StrongFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area(7,height()/2.-4,std::max(1,width()-14),8);
        QLinearGradient gradient(area.topLeft(),area.topRight());
        for(int i=0;i<=6;++i) gradient.setColorAt(i/6.,QColor::fromHsv(i==6 ? 0 : i*60,255,255));
        painter.setPen(QColor(90,90,90));painter.setBrush(gradient);painter.drawRoundedRect(area,2,2);
        const double x=area.left()+value()/359.*area.width();
        painter.setPen(QPen(hasFocus() ? palette().highlight().color() : QColor(225,225,225),1.5));
        painter.setBrush(QColor(45,45,45));painter.drawRoundedRect(QRectF(x-3,area.top()-3,6,area.height()+6),1,1);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton) {event->ignore();return;}
        setFocus(Qt::MouseFocusReason);setSliderDown(true);choose(event->position().x());event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(isSliderDown()) {choose(event->position().x());event->accept();} else event->ignore();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton && isSliderDown()) {choose(event->position().x());setSliderDown(false);event->accept();}
        else event->ignore();
    }
private:
    void choose(double x) {setValue(qRound(std::clamp((x-7)/std::max(1,width()-14),0.,1.)*359));}
};
}
ColorPanel::ColorPanel(QWidget* parent):QWidget(parent) {
    setObjectName("colorPanel");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,6,8,6);layout->setSpacing(6);
    auto* saturationValue=new SaturationValue(this);saturationValue_=saturationValue;layout->addWidget(saturationValue,1);
    hueSlider_=new HueSlider(this);layout->addWidget(hueSlider_);
    auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);
    auto* label=new QLabel("Hex",this);row->addWidget(label);
    hex_=new QLineEdit(this);hex_->setObjectName("colorHexEdit");hex_->setAccessibleName("Hex foreground color");
    hex_->setMaxLength(7);hex_->setPlaceholderText("#RRGGBB");row->addWidget(hex_,1);layout->addLayout(row);
    rgb_=new QLabel(this);rgb_->setObjectName("colorRgbReadout");rgb_->setProperty("role","mutedLabel");layout->addWidget(rgb_);
    saturationValue->changed=[this](double saturation,double value){
        applyColor(QColor::fromHsvF(static_cast<float>(hue_/360),static_cast<float>(saturation),static_cast<float>(value)),true);
    };
    connect(hueSlider_,&QSlider::valueChanged,this,[this](int value) {
        hue_=value;applyColor(QColor::fromHsvF(static_cast<float>(hue_/360),color_.hsvSaturationF(),color_.valueF()),true);
    });
    connect(hex_,&QLineEdit::editingFinished,this,[this] {
        const auto text=hex_->text().trimmed();
        static const QRegularExpression expression("^#?[0-9a-fA-F]{6}$");
        if(!expression.match(text).hasMatch()) {refreshControls();return;}
        applyColor(QColor(text.startsWith('#') ? text : '#'+text),true);
    });
    refreshControls();
}
void ColorPanel::setColor(QColor color) {applyColor(color,false);}
void ColorPanel::applyColor(QColor color,bool notify) {
    if(!color.isValid()) return;
    color=color.toRgb();color.setAlpha(255);
    const bool changed=color!=color_;color_=color;
    if(color_.hsvHueF()>=0) hue_=color_.hsvHueF()*360;
    refreshControls();
    if(notify && changed) emit colorChanged(color_);
}
void ColorPanel::refreshControls() {
    const QSignalBlocker hueSignals(hueSlider_),hexSignals(hex_);
    hueSlider_->setValue(std::clamp(qRound(hue_),0,359));
    hex_->setText(color_.name(QColor::HexRgb));
    rgb_->setText(QString("R %1   G %2   B %3").arg(color_.red()).arg(color_.green()).arg(color_.blue()));
    static_cast<SaturationValue*>(saturationValue_)->setValues(hue_,color_.hsvSaturationF(),color_.valueF());
}
}
