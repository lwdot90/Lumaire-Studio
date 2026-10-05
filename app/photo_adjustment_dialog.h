#pragma once
#include "core/editing_types.h"
#include <QDialog>

class QDoubleSpinBox;
class QDialogButtonBox;
class QLabel;
namespace compositor {
class PhotoAdjustmentDialog final:public QDialog {
    Q_OBJECT
public:
    explicit PhotoAdjustmentDialog(engine::AdjustmentKind kind,QWidget* parent=nullptr);
    explicit PhotoAdjustmentDialog(engine::AdjustmentParameters initial,QWidget* parent=nullptr,bool embedded=false);
    engine::AdjustmentParameters parameters() const;
    bool parametersValid() const;
    void commitTypedControls();
public slots:
    void accept() override;
signals:
    void parametersChanged(engine::AdjustmentParameters parameters);
    void validityChanged(bool valid);
private:
    void validateControls();
    void controlsChanged();
    void resetControls();
    engine::AdjustmentParameters current_,notified_;
    bool initializing_=true,valid_=true;
    QDoubleSpinBox* scalar_=nullptr;
    engine::AdjustmentKind kind_;
    QDoubleSpinBox* inputBlack_=nullptr;
    QDoubleSpinBox* inputWhite_=nullptr;
    QDoubleSpinBox* gamma_=nullptr;
    QDoubleSpinBox* outputBlack_=nullptr;
    QDoubleSpinBox* outputWhite_=nullptr;
    QDoubleSpinBox* warmth_=nullptr;
    QDoubleSpinBox* tint_=nullptr;
    QWidget* curve_=nullptr;
    int curveChannel_=0;
    QDialogButtonBox* buttons_=nullptr;
    QLabel* validation_=nullptr;
};
}
