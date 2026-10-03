#pragma once
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
namespace compositor {
class JobProgressWidget final:public QWidget {
    Q_OBJECT
public:
    explicit JobProgressWidget(QWidget* parent=nullptr);
    // Idle resets the cancellation latch for the next job. Repeated busy
    // notifications cannot re-enable Cancel after a request was emitted.
    void setState(bool busy,bool cancelling=false);
signals:
    void cancelRequested();
private:
    void refresh();
    QLabel* status_=nullptr;
    QProgressBar* progress_=nullptr;
    QPushButton* cancel_=nullptr;
    bool busy_=false,cancelling_=false;
};
}
