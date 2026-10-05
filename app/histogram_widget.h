#pragma once
#include "core/histogram.h"
#include <QWidget>

namespace compositor {
class HistogramWidget final : public QWidget {
public:
    explicit HistogramWidget(QWidget* parent=nullptr);
    void setHistogram(const engine::RgbHistogram& histogram);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    engine::RgbHistogram histogram_;
};
}
