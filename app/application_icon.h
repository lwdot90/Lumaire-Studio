#pragma once
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

namespace compositor {
inline QIcon applicationIcon() {
    QIcon icon;
    for(int size:{32,64,128}) {
        QPixmap pixmap(size,size);pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);painter.setRenderHint(QPainter::Antialiasing);painter.scale(size/64.,size/64.);
        painter.setPen(Qt::NoPen);painter.setBrush(QColor("#282b30"));painter.drawRoundedRect(QRectF(0,0,64,64),14,14);
        const auto layer=[&](double top,const char* color){painter.setBrush(QColor(color));painter.drawPolygon(QPolygonF{{12,top+11},{32,top+22},{52,top+11},{32,top}});};
        layer(28,"#66788b");layer(21,"#a8bdd1");layer(14,"#e3edf6");
        painter.setBrush(QColor("#6ca5d6"));painter.drawPolygon(QPolygonF{{32,14},{52,25},{32,36}});
        painter.end();icon.addPixmap(pixmap);
    }
    return icon;
}
}
