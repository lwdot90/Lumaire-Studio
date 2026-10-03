#include "app/color_panel.h"
#include <QLineEdit>
#include <QSignalSpy>
#include <QSlider>
#include <QtTest>

using namespace compositor;
class ColorPanelTest:public QObject {
    Q_OBJECT
private slots:
    void colorInputAndProgrammaticSync() {
        ColorPanel panel;panel.resize(240,220);panel.show();
        QSignalSpy changes(&panel,&ColorPanel::colorChanged);
        auto* hex=panel.findChild<QLineEdit*>("colorHexEdit");QVERIFY(hex);
        panel.setColor(QColor(10,20,30,50));
        QCOMPARE(panel.color(),QColor(10,20,30));QCOMPARE(changes.count(),0);
        QCOMPARE(hex->text(),QString("#0a141e"));
        hex->setFocus();hex->selectAll();QTest::keyClicks(hex,"#d75568");QTest::keyClick(hex,Qt::Key_Return);
        QCOMPARE(panel.color(),QColor("#d75568"));QCOMPARE(changes.count(),1);
        QCOMPARE(changes.last()[0].value<QColor>(),QColor("#d75568"));
        QTest::keyClick(hex,Qt::Key_Return);QCOMPARE(changes.count(),1);
        hex->selectAll();QTest::keyClicks(hex,"#zzzzzz");QTest::keyClick(hex,Qt::Key_Return);
        QCOMPARE(panel.color(),QColor("#d75568"));QCOMPARE(changes.count(),1);
        QCOMPARE(hex->text(),QString("#d75568"));
        panel.setColor(QColor());QCOMPARE(panel.color(),QColor("#d75568"));QCOMPARE(changes.count(),1);
    }
    void grayscaleRetainsHueAndAccessibleControls() {
        ColorPanel panel;panel.resize(240,220);panel.show();
        QSignalSpy changes(&panel,&ColorPanel::colorChanged);
        auto* hue=panel.findChild<QSlider*>("colorHueSlider");QVERIFY(hue);
        auto* area=panel.findChild<QWidget*>("colorSvArea");QVERIFY(area);
        panel.setColor(Qt::blue);panel.setColor(QColor(128,128,128));
        QCOMPARE(changes.count(),0);
        QTest::mouseClick(area,Qt::LeftButton,{},QPoint(area->width()-2,1));
        QVERIFY(panel.color().blue()>250);QVERIFY(panel.color().red()<5);QVERIFY(panel.color().green()<5);
        QCOMPARE(changes.count(),1);
        hue->setFocus();QTest::keyClick(hue,Qt::Key_Right);
        QCOMPARE(changes.count(),2);QVERIFY(panel.color()!=QColor(Qt::blue));
        const auto before=panel.color();area->setFocus();QTest::keyClick(area,Qt::Key_Down);
        QVERIFY(panel.color().value()<before.value());QCOMPARE(changes.count(),3);
    }
};
QTEST_MAIN(ColorPanelTest)
#include "color_panel_test.moc"
