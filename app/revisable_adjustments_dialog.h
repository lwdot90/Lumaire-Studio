#pragma once
#include "core/editing_types.h"
#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;
namespace compositor {
namespace engine {struct RgbHistogram;}
class HistogramWidget;
class PhotoAdjustmentDialog;
class RevisableAdjustmentsDialog final:public QDialog {
    Q_OBJECT
public:
    explicit RevisableAdjustmentsDialog(std::vector<engine::AdjustmentParameters> initial,QWidget* parent=nullptr);
    std::vector<engine::AdjustmentParameters> operations() const;
    void setPreviewState(bool pending,const QString& error={});
    void setHistogram(const engine::RgbHistogram& histogram);
public slots:
    void accept() override;
signals:
    void operationsChanged(std::vector<engine::AdjustmentParameters> operations);
private:
    void selectOperation(int row);
    void refreshList(int selection);
    void updateControls();
    void changed();
    void moveOperation(int direction);
    std::vector<engine::AdjustmentParameters> operations_;
    QListWidget* list_;
    QComboBox* kind_;
    QPushButton* add_;
    QPushButton* remove_;
    QPushButton* up_;
    QPushButton* down_;
    QWidget* editorHost_;
    QVBoxLayout* editorLayout_;
    PhotoAdjustmentDialog* editor_=nullptr;
    HistogramWidget* histogram_;
    QLabel* status_;
    QDialogButtonBox* buttons_;
    bool pending_=false;
    QString error_;
};
}
