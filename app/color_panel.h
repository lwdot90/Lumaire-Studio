#pragma once
#include <QColor>
#include <QWidget>

class QLabel;
class QLineEdit;
class QSlider;
namespace compositor {
class ColorPanel final:public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(QWidget* parent=nullptr);
    QColor color() const {return color_;}
public slots:
    void setColor(QColor color);
signals:
    void colorChanged(QColor color);
private:
    void applyColor(QColor color,bool notify);
    void refreshControls();
    QColor color_{0,0,0};
    double hue_=0;
    QWidget* saturationValue_=nullptr;
    QSlider* hueSlider_=nullptr;
    QLineEdit* hex_=nullptr;
    QLabel* rgb_=nullptr;
};
}
