#pragma once
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace compositor {
inline QIcon layerPanelIcon(int kind) {
    QPixmap pixmap(36,36);pixmap.setDevicePixelRatio(2);pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#cccccc"),1.2));
    if(kind==1) {
        QPainterPath folder;folder.moveTo(2,5);folder.lineTo(7,5);folder.lineTo(9,7);folder.lineTo(16,7);folder.lineTo(16,15);folder.lineTo(2,15);folder.closeSubpath();p.drawPath(folder);
    } else if(kind==3) {
        p.drawRect(QRectF(5,6,8,10));p.drawLine(4,4,14,4);p.drawLine(7,2,11,2);p.drawLine(8,8,8,14);p.drawLine(10,8,10,14);
    } else if(kind==4) {
        p.drawRect(QRectF(2,4,14,11));p.setBrush(QColor("#cccccc"));p.drawEllipse(QPointF(9,9.5),3,3);
    } else {
        if(kind==2) p.drawRect(QRectF(2,2,10,11));
        p.drawRect(QRectF(5,5,10,11));
        if(kind==0) {p.drawLine(7,10,13,10);p.drawLine(10,7,10,13);}
    }
    p.end();return QIcon(pixmap);
}
}
