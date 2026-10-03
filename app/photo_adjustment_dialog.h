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
    engine::AdjustmentParameters parameters() const;
public slots:
    void accept() override;
private:
    void validateControls();
    engine::AdjustmentKind kind_;
    QDoubleSpinBox* inputBlack_=nullptr;
    QDoubleSpinBox* inputWhite_=nullptr;
    QDoubleSpinBox* gamma_=nullptr;
    QDoubleSpinBox* outputBlack_=nullptr;
    QDoubleSpinBox* outputWhite_=nullptr;
    QDoubleSpinBox* warmth_=nullptr;
    QDoubleSpinBox* tint_=nullptr;
    QWidget* curve_=nullptr;
    QDialogButtonBox* buttons_=nullptr;
    QLabel* validation_=nullptr;
};
}
