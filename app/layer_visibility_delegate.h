#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QStyle>

namespace compositor {
class LayerVisibilityDelegate final:public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter,option,index);
        QStyleOptionViewItem item(option);initStyleOption(&item,index);
        if(!item.widget || !(item.features & QStyleOptionViewItem::HasCheckIndicator)) return;
        const auto rect=item.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,&item,item.widget);
        painter->save();painter->setRenderHint(QPainter::Antialiasing);
        painter->fillRect(rect.adjusted(-1,-1,1,1),item.state & QStyle::State_Selected ? QColor("#626262") : QColor("#414141"));
        if(item.checkState==Qt::Checked) {
            const auto center=QRectF(rect).center();const double half=rect.width()/2.0;
            QPainterPath eye;eye.moveTo(center.x()-half,center.y());
            eye.quadTo(center.x(),center.y()-half,center.x()+half,center.y());
            eye.quadTo(center.x(),center.y()+half,center.x()-half,center.y());
            painter->setPen(QPen(QColor("#cccccc"),1));painter->setBrush(Qt::NoBrush);painter->drawPath(eye);
            painter->setBrush(QColor("#cccccc"));painter->drawEllipse(center,2,2);
        }
        painter->restore();
    }
};
}
