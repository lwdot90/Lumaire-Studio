#include "app/revisable_exposure_dialog.h"
#include "core/histogram.h"
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QtTest>
#include <limits>
#include <stdexcept>

using namespace compositor;
namespace {
void enter(QDoubleSpinBox* value,const QString& text) {
    auto* editor=value->findChild<QLineEdit*>();QVERIFY(editor);
    QTest::mouseClick(editor,Qt::LeftButton);QTRY_VERIFY(editor->hasFocus());
    QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(editor,text);
    QTest::keyClick(editor,Qt::Key_Tab);QTRY_VERIFY(!editor->hasFocus());
}
}
class RevisableExposureDialogTest final:public QObject {
    Q_OBJECT
private slots:
    void initialValueAndInvalidParameters() {
        RevisableExposureDialog dialog(1.234567);
        QCOMPARE(dialog.objectName(),QString("revisableExposureDialog"));QCOMPARE(dialog.exposure(),1.234567);
        auto* value=dialog.findChild<QDoubleSpinBox*>("revisableExposureValue");QVERIFY(value);
        QCOMPARE(value->value(),1.23);QCOMPARE(value->minimum(),-8.);QCOMPARE(value->maximum(),8.);
        QCOMPARE(value->decimals(),2);QVERIFY(!value->accessibleName().isEmpty());
        dialog.accept();QCOMPARE(dialog.result(),int(QDialog::Accepted));QCOMPARE(dialog.exposure(),1.234567);
        QVERIFY_EXCEPTION_THROWN(RevisableExposureDialog(std::numeric_limits<double>::quiet_NaN()),std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(RevisableExposureDialog(8.01),std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(RevisableExposureDialog(-8.01),std::invalid_argument);
    }
    void typedChangeRequiresReadyPreviewAndResetEmitsOnce() {
        RevisableExposureDialog dialog(1.5);dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QSignalSpy changes(&dialog,&RevisableExposureDialog::exposureChanged);
        auto* value=dialog.findChild<QDoubleSpinBox*>("revisableExposureValue");
        auto* slider=dialog.findChild<QSlider*>("revisableExposureSlider");
        auto* reset=dialog.findChild<QPushButton*>("revisableExposureReset");
        auto* box=dialog.findChild<QDialogButtonBox*>();auto* status=dialog.findChild<QLabel*>("revisableExposureStatus");
        QVERIFY(value && slider && reset && box && status);
        enter(value,"-2.25");QCOMPARE(dialog.exposure(),-2.25);QCOMPARE(slider->value(),-225);QCOMPARE(changes.count(),1);
        QVERIFY(!box->button(QDialogButtonBox::Ok)->isEnabled());dialog.accept();QVERIFY(dialog.isVisible());
        dialog.setPreviewState(false,"Preview refused: not enough available memory");QVERIFY(!box->button(QDialogButtonBox::Ok)->isEnabled());
        QCOMPARE(status->text(),QString("Preview refused: not enough available memory"));QVERIFY(value->isEnabled());QVERIFY(slider->isEnabled());QVERIFY(reset->isEnabled());
        QTest::mouseClick(reset,Qt::LeftButton);QCOMPARE(dialog.exposure(),0.);QCOMPARE(changes.count(),2);QCOMPARE(changes.last()[0].toDouble(),0.);
        QTest::mouseClick(reset,Qt::LeftButton);QCOMPARE(changes.count(),2);
        dialog.setPreviewState(false);QVERIFY(box->button(QDialogButtonBox::Ok)->isEnabled());
        QTest::mouseClick(box->button(QDialogButtonBox::Ok),Qt::LeftButton);QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void pendingCancelAndSliderKeepControlsUsable() {
        RevisableExposureDialog dialog(0);dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QSignalSpy accepted(&dialog,&QDialog::accepted),rejected(&dialog,&QDialog::rejected),changes(&dialog,&RevisableExposureDialog::exposureChanged);
        auto* slider=dialog.findChild<QSlider*>("revisableExposureSlider");auto* box=dialog.findChild<QDialogButtonBox*>();QVERIFY(slider && box);
        dialog.setPreviewState(true);QVERIFY(!box->button(QDialogButtonBox::Ok)->isEnabled());QVERIFY(box->button(QDialogButtonBox::Cancel)->isEnabled());
        QTest::mouseClick(slider,Qt::LeftButton);slider->setFocus();QTest::keyClick(slider,Qt::Key_Home);QCOMPARE(dialog.exposure(),-8.);
        QTest::keyClick(slider,Qt::Key_End);QCOMPARE(dialog.exposure(),8.);QVERIFY(changes.count()>=2);
        const auto count=changes.count();QTest::mouseClick(box->button(QDialogButtonBox::Cancel),Qt::LeftButton);
        QCOMPARE(accepted.count(),0);QCOMPARE(rejected.count(),1);QCOMPARE(changes.count(),count);QCOMPARE(dialog.result(),int(QDialog::Rejected));
    }
    void exactSmallStoredValueResetAndHistogram() {
        RevisableExposureDialog dialog(.004);QSignalSpy changes(&dialog,&RevisableExposureDialog::exposureChanged);
        auto* reset=dialog.findChild<QPushButton*>("revisableExposureReset");QVERIFY(reset);
        reset->click();QCOMPARE(dialog.exposure(),0.);QCOMPARE(changes.count(),1);
        engine::RgbHistogram histogram;histogram.red[128]=1;histogram.green[64]=1;histogram.blue[32]=1;histogram.luma[64]=1;histogram.samples=1;
        dialog.setHistogram(histogram);QVERIFY(dialog.findChild<QWidget*>("revisableExposureHistogram"));
        histogram={};dialog.setHistogram(histogram);QCOMPARE(dialog.exposure(),0.);
    }
};
QTEST_MAIN(RevisableExposureDialogTest)
#include "revisable_exposure_dialog_test.moc"
