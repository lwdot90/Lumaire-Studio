#include "app/photo_adjustment_dialog.h"
#include "app/editor_theme.h"
#include "core/editor_commands.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace compositor {
namespace {
class ToneCurveEditor final:public QWidget {
public:
    explicit ToneCurveEditor(QWidget* parent=nullptr):QWidget(parent) {
        setObjectName("toneCurveEditor");setAccessibleName("Tone curve");
        setAccessibleDescription("Click to add a point; drag or use arrow keys to move it. Delete removes an interior point. Endpoints are fixed.");
        setFocusPolicy(Qt::StrongFocus);setMinimumSize(260,210);
        setToolTip("Click to add a point. Drag or use arrow keys. Delete removes an interior point; endpoints stay fixed.");
    }
    const std::vector<engine::CurvePoint>& points() const {return points_;}
    void setCurveColor(QColor color) {color_=std::move(color);update();}
    int selected() const {return selected_;}
    void setPoints(std::vector<engine::CurvePoint> points) {
        points_=std::move(points);selected_=0;notify();
    }
    void changeSelected(double input,double output) {
        if(!interior()) return;
        auto& point=points_[static_cast<std::size_t>(selected_)];
        point.input=std::clamp(input,points_[static_cast<std::size_t>(selected_-1)].input+gap,
            points_[static_cast<std::size_t>(selected_+1)].input-gap);
        point.output=std::clamp(output,0.,1.);notify();
    }
    void removeSelected() {
        if(!interior()) return;
        points_.erase(points_.begin()+selected_);selected_=std::min(selected_,static_cast<int>(points_.size())-1);notify();
    }
    std::function<void()> changed;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const auto area=plot();painter.fillRect(rect(),palette().color(QPalette::Base));
        painter.setPen(palette().color(QPalette::Mid));
        for(int i=0;i<5;++i) {
            const double fraction=static_cast<double>(i)/4;
            painter.drawLine(QPointF(area.left()+area.width()*fraction,area.top()),QPointF(area.left()+area.width()*fraction,area.bottom()));
            painter.drawLine(QPointF(area.left(),area.top()+area.height()*fraction),QPointF(area.right(),area.top()+area.height()*fraction));
        }
        painter.setPen(QPen(palette().color(QPalette::Mid),1,Qt::DashLine));painter.drawLine(position({0,0}),position({1,1}));
        painter.setPen(QPen(color_,2));
        for(std::size_t i=1;i<points_.size();++i) painter.drawLine(position(points_[i-1]),position(points_[i]));
        for(std::size_t i=0;i<points_.size();++i) {
            painter.setBrush(static_cast<int>(i)==selected_ ? QColor("#e1edf6") : palette().color(QPalette::Button));
            painter.drawEllipse(position(points_[i]),4,4);
        }
        if(hasFocus()) {painter.setBrush(Qt::NoBrush);painter.setPen(palette().color(QPalette::Highlight));painter.drawRect(rect().adjusted(1,1,-2,-2));}
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton) return;
        setFocus();const auto cursor=event->position();selected_=-1;
        for(std::size_t i=0;i<points_.size();++i)
            if(QLineF(cursor,position(points_[i])).length()<=10) {selected_=static_cast<int>(i);break;}
        if(selected_<0 && plot().contains(cursor) && points_.size()<16) {
            const auto point=value(cursor);
            auto next=std::upper_bound(points_.begin(),points_.end(),point.input,[](double input,const engine::CurvePoint& p){return input<p.input;});
            if(next!=points_.begin() && next!=points_.end() && point.input>std::prev(next)->input+2*gap && point.input<next->input-2*gap) {
                selected_=static_cast<int>(next-points_.begin());points_.insert(next,point);
            }
        }
        dragging_=interior();notify();event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(!dragging_) return;
        const auto point=value(event->position());changeSelected(point.input,point.output);event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {if(event->button()==Qt::LeftButton) dragging_=false;}
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Delete || event->key()==Qt::Key_Backspace) {removeSelected();event->accept();return;}
        if(interior()) {
            auto point=points_[static_cast<std::size_t>(selected_)];const double step=event->modifiers().testFlag(Qt::ShiftModifier) ? .05 : 1./255;
            switch(event->key()) {
                case Qt::Key_Left:point.input-=step;break;
                case Qt::Key_Right:point.input+=step;break;
                case Qt::Key_Up:point.output+=step;break;
                case Qt::Key_Down:point.output-=step;break;
                default:QWidget::keyPressEvent(event);return;
            }
            changeSelected(point.input,point.output);event->accept();return;
        }
        QWidget::keyPressEvent(event);
    }
private:
    bool interior() const {return selected_>0 && selected_<static_cast<int>(points_.size())-1;}
    QRectF plot() const {return QRectF(rect()).adjusted(18,14,-14,-18);}
    QPointF position(engine::CurvePoint point) const {const auto area=plot();return {area.left()+point.input*area.width(),area.bottom()-point.output*area.height()};}
    engine::CurvePoint value(QPointF point) const {const auto area=plot();return {std::clamp((point.x()-area.left())/area.width(),0.,1.),std::clamp((area.bottom()-point.y())/area.height(),0.,1.)};}
    void notify() {update();if(changed) changed();}
    static constexpr double gap=.0001;
    std::vector<engine::CurvePoint> points_{{0,0},{1,1}};
    int selected_=0;
    bool dragging_=false;
    QColor color_{"#9bbbd6"};
};
QDoubleSpinBox* number(QFormLayout* layout,const QString& label,const char* name,double minimum,double maximum,double initial,int decimals=1) {
    auto* control=new QDoubleSpinBox;control->setObjectName(name);control->setAccessibleName(label);
    control->setRange(minimum,maximum);control->setDecimals(decimals);control->setValue(initial);control->setKeyboardTracking(false);
    control->setButtonSymbols(QAbstractSpinBox::NoButtons);
    layout->addRow(label,control);return control;
}
}
PhotoAdjustmentDialog::PhotoAdjustmentDialog(engine::AdjustmentKind kind,QWidget* parent)
    :PhotoAdjustmentDialog([kind]{engine::AdjustmentParameters initial;initial.kind=kind;if(kind==engine::AdjustmentKind::Saturation) initial.value=1;return initial;}(),parent,false) {}
PhotoAdjustmentDialog::PhotoAdjustmentDialog(engine::AdjustmentParameters initial,QWidget* parent,bool embedded)
    :QDialog(parent),current_(initial),notified_(initial),kind_(initial.kind) {
    (void)engine::adjustmentIsNeutral(initial);
    const auto kind=kind_;
    if(embedded) setWindowFlags(Qt::Widget);
    applyEditorTheme();setObjectName("photoAdjustmentDialog");setModal(!embedded);
    auto* layout=new QVBoxLayout(this);layout->setSpacing(8);auto* form=new QFormLayout;
    if(kind==engine::AdjustmentKind::Exposure || kind==engine::AdjustmentKind::Brightness || kind==engine::AdjustmentKind::Contrast || kind==engine::AdjustmentKind::Saturation) {
        QString title,label;double minimum=0,maximum=0;
        switch(kind) {
            case engine::AdjustmentKind::Exposure:title="Exposure";label="Exposure (stops)";minimum=-8;maximum=8;break;
            case engine::AdjustmentKind::Brightness:title="Brightness";label="Brightness";minimum=-1;maximum=1;break;
            case engine::AdjustmentKind::Contrast:title="Contrast";label="Contrast";minimum=-.95;maximum=4;break;
            default:title="Saturation";label="Saturation";minimum=0;maximum=2;break;
        }
        setWindowTitle(title);scalar_=number(form,label,"photoAdjustmentValue",minimum,maximum,initial.value,3);scalar_->setSingleStep(.1);
    } else if(kind==engine::AdjustmentKind::Levels) {
        setWindowTitle("Levels");
        inputBlack_=number(form,"Input black","levelsInputBlack",0,255,initial.levels.inputBlack*255);
        inputWhite_=number(form,"Input white","levelsInputWhite",0,255,initial.levels.inputWhite*255);
        gamma_=number(form,"Gamma","levelsGamma",.1,10,initial.levels.gamma,2);gamma_->setSingleStep(.1);
        outputBlack_=number(form,"Output black","levelsOutputBlack",0,255,initial.levels.outputBlack*255);
        outputWhite_=number(form,"Output white","levelsOutputWhite",0,255,initial.levels.outputWhite*255);
    } else if(kind==engine::AdjustmentKind::ColorBalance) {
        setWindowTitle("Color Balance");
        auto* explanation=new QLabel("Relative RGB correction. Neutral is 0.");explanation->setWordWrap(true);layout->addWidget(explanation);
        warmth_=number(form,"Warmth","balanceWarmth",-100,100,initial.colorBalance.warmth*100,0);
        tint_=number(form,"Tint","balanceTint",-100,100,initial.colorBalance.tint*100,0);
    } else if(kind==engine::AdjustmentKind::Curves) {
        setWindowTitle("Curves");
        auto* channel=new QComboBox;channel->setObjectName("curveChannel");channel->setAccessibleName("Curve channel");
        channel->addItems({"RGB","Red","Green","Blue"});form->addRow("Channel",channel);
        auto* presets=new QComboBox;presets->setObjectName("curvePreset");presets->setAccessibleName("Curve preset");
        presets->addItems({"Linear","Lift shadows","Gentle contrast"});presets->setPlaceholderText("Custom curve");form->addRow("Preset",presets);
        layout->addLayout(form);form=nullptr;
        auto* editor=new ToneCurveEditor(this);curve_=editor;layout->addWidget(editor);
        auto* points=new QFormLayout;
        auto* input=number(points,"Selected input","curvePointInput",0,255,0,3);
        auto* output=number(points,"Selected output","curvePointOutput",0,255,0,3);
        auto* remove=new QPushButton("Remove point");remove->setObjectName("curvePointRemove");remove->setAccessibleName("Remove selected curve point");
        points->addRow(remove);layout->addLayout(points);
        editor->changed=[this,editor,presets,input,output,remove] {
            const QSignalBlocker presetBlock(presets),inputBlock(input),outputBlock(output);
            presets->setCurrentIndex(-1);
            const int selected=editor->selected();const bool interior=selected>0 && selected<static_cast<int>(editor->points().size())-1;
            input->setEnabled(interior);output->setEnabled(interior);remove->setEnabled(interior);
            if(selected>=0) {
                const auto point=editor->points()[static_cast<std::size_t>(selected)];
                if(interior) input->setRange((editor->points()[static_cast<std::size_t>(selected-1)].input+.0001)*255,(editor->points()[static_cast<std::size_t>(selected+1)].input-.0001)*255);
                else input->setRange(0,255);
                input->setValue(point.input*255);output->setValue(point.output*255);
            }
            if(!initializing_) {
                if(curveChannel_==0) current_.curve=editor->points();
                else current_.channelCurves[static_cast<std::size_t>(curveChannel_-1)]=editor->points();
                controlsChanged();
            }
        };
        connect(input,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[editor](double value){if(editor->selected()>=0) editor->changeSelected(value/255,editor->points()[static_cast<std::size_t>(editor->selected())].output);});
        connect(output,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[editor](double value){if(editor->selected()>=0) editor->changeSelected(editor->points()[static_cast<std::size_t>(editor->selected())].input,value/255);});
        connect(remove,&QPushButton::clicked,this,[editor]{editor->removeSelected();});
        connect(presets,qOverload<int>(&QComboBox::currentIndexChanged),this,[editor,presets](int index) {
            if(index<0) return;
            if(index==0) editor->setPoints({{0,0},{1,1}});
            else if(index==1) editor->setPoints({{0,0},{.25,.38},{.5,.62},{.75,.82},{1,1}});
            else editor->setPoints({{0,0},{.25,.18},{.5,.5},{.75,.82},{1,1}});
            const QSignalBlocker blocker(presets);presets->setCurrentIndex(index);
        });
        connect(channel,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,editor,presets](int index) {
            if(index<0 || index>3) return;
            commitTypedControls();curveChannel_=index;
            const auto& selectedCurvePoints=index==0 ? current_.curve : current_.channelCurves[static_cast<std::size_t>(index-1)];
            const bool wasInitializing=initializing_;initializing_=true;editor->setPoints(selectedCurvePoints);initializing_=wasInitializing;
            const QSignalBlocker blocker(presets);presets->setCurrentIndex(selectedCurvePoints==std::vector<engine::CurvePoint>{{0,0},{1,1}} ? 0 : -1);
            const QColor colors[]{QColor("#9bbbd6"),QColor("#e58d8d"),QColor("#86c994"),QColor("#85abe8")};editor->setCurveColor(colors[index]);
        });
        editor->setPoints(initial.curve);
        if(initial.curve==std::vector<engine::CurvePoint>{{0,0},{1,1}}) {const QSignalBlocker blocker(presets);presets->setCurrentIndex(0);}
        auto* help=new QLabel("Input → output. Click to add a point; drag or use arrow keys. Endpoints stay fixed.");help->setWordWrap(true);layout->addWidget(help);
    } else throw std::invalid_argument("Unsupported photo adjustment dialog kind");
    if(form) layout->addLayout(form);
    validation_=new QLabel;validation_->setObjectName("photoAdjustmentValidation");validation_->setWordWrap(true);layout->addWidget(validation_);
    buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons_->setObjectName("photoAdjustmentButtons");layout->addWidget(buttons_);
    connect(buttons_,&QDialogButtonBox::accepted,this,&PhotoAdjustmentDialog::accept);
    connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);
    if(embedded) buttons_->hide();
    auto* reset=new QPushButton(kind==engine::AdjustmentKind::Curves ? "Reset all curves" : "Reset",this);reset->setObjectName("photoAdjustmentReset");reset->setAccessibleName(kind==engine::AdjustmentKind::Curves ? "Reset RGB, red, green and blue curves to identity" : "Reset adjustment to defaults");reset->setAutoDefault(false);layout->insertWidget(layout->count()-1,reset);
    connect(reset,&QPushButton::clicked,this,&PhotoAdjustmentDialog::resetControls);
    const auto bind=[this](QDoubleSpinBox* control,std::function<void(double)> update) {
        if(control) connect(control,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,update=std::move(update)](double value){if(initializing_) return;update(value);controlsChanged();});
    };
    bind(scalar_,[this](double value){current_.value=value;});
    bind(inputBlack_,[this](double value){current_.levels.inputBlack=value/255;});
    bind(inputWhite_,[this](double value){current_.levels.inputWhite=value/255;});
    bind(gamma_,[this](double value){current_.levels.gamma=value;});
    bind(outputBlack_,[this](double value){current_.levels.outputBlack=value/255;});
    bind(outputWhite_,[this](double value){current_.levels.outputWhite=value/255;});
    bind(warmth_,[this](double value){current_.colorBalance.warmth=value/100;});
    bind(tint_,[this](double value){current_.colorBalance.tint=value/100;});
    initializing_=false;validateControls();setMinimumWidth(320);
}
engine::AdjustmentParameters PhotoAdjustmentDialog::parameters() const {
    if(!parametersValid()) throw std::invalid_argument("Invalid adjustment parameters");
    return current_;
}
bool PhotoAdjustmentDialog::parametersValid() const {
    try {(void)engine::adjustmentIsNeutral(current_);return true;} catch(const std::exception&) {return false;}
}
void PhotoAdjustmentDialog::validateControls() {
    if(!buttons_ || !validation_) return;
    const bool valid=parametersValid();
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid);
    validation_->setText(valid ? QString() : QString("Input black must be below input white. Output black cannot exceed output white."));
    validation_->setVisible(!valid);
    if(valid!=valid_) {valid_=valid;emit validityChanged(valid);}
}
void PhotoAdjustmentDialog::controlsChanged() {
    const bool wasValid=valid_;validateControls();
    if(valid_ && (current_!=notified_ || !wasValid)) {notified_=current_;emit parametersChanged(current_);}
}
void PhotoAdjustmentDialog::resetControls() {
    engine::AdjustmentParameters neutral;neutral.kind=kind_;if(kind_==engine::AdjustmentKind::Saturation) neutral.value=1;
    current_=neutral;initializing_=true;
    const auto set=[](QDoubleSpinBox* control,double value){if(control){const QSignalBlocker blocker(control);control->setValue(value);}};
    set(scalar_,neutral.value);set(inputBlack_,0);set(inputWhite_,255);set(gamma_,1);set(outputBlack_,0);set(outputWhite_,255);set(warmth_,0);set(tint_,0);
    if(curve_) {
        static_cast<ToneCurveEditor*>(curve_)->setPoints(neutral.curve);
        if(auto* preset=findChild<QComboBox*>("curvePreset")) {const QSignalBlocker blocker(preset);preset->setCurrentIndex(0);}
    }
    initializing_=false;controlsChanged();
}
void PhotoAdjustmentDialog::commitTypedControls() {
    for(auto* control:{scalar_,inputBlack_,inputWhite_,gamma_,outputBlack_,outputWhite_,warmth_,tint_}) if(control) control->interpretText();
    for(const auto* name:{"curvePointInput","curvePointOutput"}) if(auto* control=findChild<QDoubleSpinBox*>(name)) control->interpretText();
    validateControls();
}
void PhotoAdjustmentDialog::accept() {
    commitTypedControls();if(parametersValid()) QDialog::accept();
}
}
