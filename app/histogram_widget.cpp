#include "app/histogram_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace compositor {
HistogramWidget::HistogramWidget(QWidget* parent):QWidget(parent) {
    setObjectName("histogramWidget");setMinimumSize(220,110);
    setAccessibleName("RGB and luminance histogram");
    setToolTip("Straight display sRGB values for the selected source; transparent pixels excluded");
}
void HistogramWidget::setHistogram(const engine::RgbHistogram& histogram) {histogram_=histogram;update();}
void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(),palette().base());
    const QRectF plot(8,8,width()-16,height()-35);
    painter.setPen(palette().mid().color());painter.drawRect(plot);
    std::uint64_t maximum=0;
    for(const auto* bins:{&histogram_.red,&histogram_.green,&histogram_.blue,&histogram_.luma})
        maximum=std::max(maximum,*std::max_element(bins->begin(),bins->end()));
    if(maximum) {
        const auto draw=[&](const auto& bins,QColor color) {
            QPainterPath path;
            for(int i=0;i<256;++i) {
                const QPointF point(plot.left()+plot.width()*i/255.,plot.bottom()-plot.height()*static_cast<double>(bins[i])/static_cast<double>(maximum));
                if(i==0) path.moveTo(point);else path.lineTo(point);
            }
            painter.setPen(QPen(color,1.2));painter.drawPath(path);
        };
        draw(histogram_.luma,palette().text().color());draw(histogram_.red,QColor(239,99,107));
        draw(histogram_.green,QColor(101,203,139));draw(histogram_.blue,QColor(102,164,245));
    }
    painter.setPen(palette().text().color());
    painter.drawText(QRect(8,height()-24,width()-16,20),Qt::AlignLeft|Qt::AlignVCenter,
        QString("%1 · %2 visible samples").arg(histogram_.sampled ? "Sampled histogram" : "Histogram").arg(static_cast<qulonglong>(histogram_.samples)));
}
}
