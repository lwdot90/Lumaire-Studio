#include "app/job_progress_widget.h"
#include <QLabel>
#include <QProgressBar>
#include <QSignalSpy>
#include <QStatusBar>
#include <QPushButton>
#include <QtTest>

using namespace compositor;
class JobProgressWidgetTest:public QObject {
    Q_OBJECT
private slots:
    void idleBusyAndStatusBarLayout() {
        QStatusBar bar;bar.resize(640,32);
        auto* widget=new JobProgressWidget(&bar);bar.addPermanentWidget(widget);bar.show();
        auto* progress=widget->findChild<QProgressBar*>("operationProgress");
        auto* label=widget->findChild<QLabel*>("operationStatusLabel");
        auto* cancel=widget->findChild<QPushButton*>("cancelOperationButton");
        QVERIFY(progress);QVERIFY(label);QVERIFY(cancel);
        QCOMPARE(widget->objectName(),QString("jobProgressWidget"));
        QVERIFY(widget->isHidden());QVERIFY(!cancel->isEnabled());
        widget->setState(true);
        QTRY_VERIFY(widget->isVisible());
        QCOMPARE(label->text(),QString("Working…"));
        QCOMPARE(progress->minimum(),0);QCOMPARE(progress->maximum(),0);
        QVERIFY(!progress->isTextVisible());QVERIFY(cancel->isEnabled());
        QCOMPARE(cancel->text(),QString("Cancel"));
        QCOMPARE(cancel->toolTip(),QString("Cancel current operation"));
        QCOMPARE(cancel->accessibleName(),QString("Cancel current operation"));
        QTRY_VERIFY(bar.rect().contains(widget->geometry()));
        QVERIFY(widget->rect().contains(cancel->geometry()));
        QVERIFY(widget->rect().contains(progress->geometry()));
        QVERIFY(widget->width()<400);
        widget->setState(false);
        QVERIFY(widget->isHidden());QVERIFY(!cancel->isEnabled());
    }
    void realClickRequestsOnceAndIdleResetsNextJob() {
        QStatusBar bar;bar.resize(640,32);
        auto* widget=new JobProgressWidget(&bar);bar.addPermanentWidget(widget);bar.show();
        auto* cancel=widget->findChild<QPushButton*>("cancelOperationButton");
        auto* label=widget->findChild<QLabel*>("operationStatusLabel");
        QVERIFY(cancel);QVERIFY(label);
        QSignalSpy requests(widget,&JobProgressWidget::cancelRequested);QVERIFY(requests.isValid());
        widget->setState(true);QTRY_VERIFY(widget->isVisible());
        QTest::mouseClick(cancel,Qt::LeftButton);
        QCOMPARE(requests.count(),1);QVERIFY(!cancel->isEnabled());
        QCOMPARE(label->text(),QString("Cancelling…"));QVERIFY(widget->isVisible());
        // A queued pre-cancellation busy notification cannot re-enable Cancel.
        widget->setState(true,false);QVERIFY(!cancel->isEnabled());
        QTest::mouseClick(cancel,Qt::LeftButton);QCOMPARE(requests.count(),1);
        widget->setState(true,true);QVERIFY(widget->isVisible());QVERIFY(!cancel->isEnabled());
        widget->setState(false);QVERIFY(widget->isHidden());
        widget->setState(true,false);QTRY_VERIFY(cancel->isEnabled());
        QCOMPARE(label->text(),QString("Working…"));
        QTest::mouseClick(cancel,Qt::LeftButton);
        QCOMPARE(requests.count(),2);QVERIFY(!cancel->isEnabled());
    }
    void ownerCancellationDoesNotEmitAndReentrantStateCannotRearm() {
        QStatusBar bar;bar.resize(640,32);
        auto* widget=new JobProgressWidget(&bar);bar.addPermanentWidget(widget);bar.show();
        auto* cancel=widget->findChild<QPushButton*>("cancelOperationButton");
        auto* label=widget->findChild<QLabel*>("operationStatusLabel");
        QVERIFY(cancel);QVERIFY(label);
        QSignalSpy requests(widget,&JobProgressWidget::cancelRequested);
        widget->setState(true,true);QTRY_VERIFY(widget->isVisible());
        QCOMPARE(label->text(),QString("Cancelling…"));QVERIFY(!cancel->isEnabled());
        QCOMPARE(requests.count(),0);
        widget->setState(false,true);QVERIFY(widget->isHidden());
        widget->setState(true);QVERIFY(cancel->isEnabled());
        connect(widget,&JobProgressWidget::cancelRequested,widget,[widget] {widget->setState(true,false);});
        QTest::mouseClick(cancel,Qt::LeftButton);
        QCOMPARE(requests.count(),1);QVERIFY(!cancel->isEnabled());
        QCOMPARE(label->text(),QString("Cancelling…"));
        QTest::mouseClick(cancel,Qt::LeftButton);QCOMPARE(requests.count(),1);
    }
};
QTEST_MAIN(JobProgressWidgetTest)
#include "job_progress_widget_test.moc"
