#pragma once
#include <QDialog>

class QDoubleSpinBox;
class QDialogButtonBox;
class QLabel;
class QSlider;
namespace compositor {
namespace engine { struct RgbHistogram; }
class HistogramWidget;
class RevisableExposureDialog final:public QDialog {
    Q_OBJECT
public:
    explicit RevisableExposureDialog(double initialExposure,QWidget* parent=nullptr);
    double exposure() const;
    void setPreviewState(bool pending,const QString& error={});
    void setHistogram(const engine::RgbHistogram& histogram);
public slots:
    void accept() override;
signals:
    void exposureChanged(double exposure);
private:
    QDoubleSpinBox* value_;
    QSlider* slider_;
    QDialogButtonBox* buttons_;
    QLabel* status_;
    HistogramWidget* histogram_;
    bool ready_=true;
    double exposure_=0;
};
}
