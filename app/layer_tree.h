#pragma once
#include <QTreeWidget>
#include <QDrag>
#include <QDropEvent>
#include <QPointer>
#include <functional>

namespace compositor {
// Qt supplies drag feedback; the document owns hierarchy mutations. Never let
// a successful drop independently move/delete Qt rows behind the document.
class LayerTree final:public QTreeWidget {
public:
    using Move=std::function<bool(QString,int)>;
    std::function<Move(QString)> captureMove;
    explicit LayerTree(QWidget* parent=nullptr):QTreeWidget(parent) {
        setDragEnabled(true);setAcceptDrops(true);setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::InternalMove);setDefaultDropAction(Qt::MoveAction);
        setAccessibleName("Layers");
    }
protected:
    void startDrag(Qt::DropActions) override {
        auto* item=currentItem();if(!item || !captureMove) return;
        move_=captureMove(item->data(0,Qt::UserRole).toString());if(!move_) return;
        source_=item->data(0,Qt::UserRole).toString();
        const QPointer<LayerTree> alive(this);
        const QPointer<QDrag> drag=new QDrag(this);
        drag->setMimeData(mimeData({item}));drag->exec(Qt::MoveAction);
        if(drag) drag->deleteLater();
        // Native drag execution can dispatch close/deletion events.
        if(!alive) return;
        move_={};source_.clear();
    }
    void dropEvent(QDropEvent* event) override {
        if(event->source()!=this || !move_) {event->ignore();return;}
        auto* target=itemAt(event->position().toPoint());
        auto* parent=target ? target->parent() : nullptr;
        int row=parent ? parent->indexOfChild(target) : indexOfTopLevelItem(target);
        if(!target || dropIndicatorPosition()==OnViewport) {parent=nullptr;row=topLevelItemCount();}
        else if(dropIndicatorPosition()==OnItem) {
            if(!target->data(0,Qt::UserRole+1).toBool()) {event->ignore();return;}
            parent=target;row=0; // Into a folder lands at its visual top.
        } else if(dropIndicatorPosition()==BelowItem) ++row;
        const int count=parent ? parent->childCount() : topLevelItemCount();
        int remaining=count;
        for(int i=0;i<count;++i) {
            auto* sibling=parent ? parent->child(i) : topLevelItem(i);
            if(sibling->data(0,Qt::UserRole).toString()==source_) {
                --remaining;if(i<row) --row;break;
            }
        }
        const auto parentId=parent ? parent->data(0,Qt::UserRole).toString() : QString{};
        // Convert top-to-bottom UI insertion to canonical bottom-to-top order.
        if(move_(parentId,remaining-row)) {event->setDropAction(Qt::MoveAction);event->accept();}
        else event->ignore();
    }
private:
    Move move_;
    QString source_;
};
}
