#include "app/photo_adjustment_dialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QtTest>

using namespace compositor;
namespace {
QPoint graphPoint(QWidget* widget,double input,double output) {
    const auto area=QRectF(widget->rect()).adjusted(18,14,-14,-18);
    return QPointF(area.left()+input*area.width(),area.bottom()-output*area.height()).toPoint();
}
void enter(QDoubleSpinBox* box,const QString& text) {
    auto* editor=box->findChild<QLineEdit*>();QVERIFY(editor);
    QTest::mouseClick(editor,Qt::LeftButton);
    QTRY_VERIFY(editor->hasFocus());
    QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(editor,text);
    QCOMPARE(editor->text(),text);
    QTest::keyClick(editor,Qt::Key_Tab);
    QTRY_VERIFY(!editor->hasFocus());
}
}
class PhotoAdjustmentDialogTest:public QObject {
    Q_OBJECT
private slots:
    void levelsMappingAndInvalidSubmission() {
        PhotoAdjustmentDialog dialog(engine::AdjustmentKind::Levels);dialog.show();QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.objectName(),QString("photoAdjustmentDialog"));
        auto find=[&](const char* name){return dialog.findChild<QDoubleSpinBox*>(name);};
        auto* black=find("levelsInputBlack");auto* white=find("levelsInputWhite");auto* gamma=find("levelsGamma");
        auto* outputBlack=find("levelsOutputBlack");auto* outputWhite=find("levelsOutputWhite");
        QVERIFY(black && white && gamma && outputBlack && outputWhite);
        for(auto* control:{black,white,gamma,outputBlack,outputWhite}) QVERIFY(!control->accessibleName().isEmpty());
        const auto neutral=dialog.parameters();QCOMPARE(neutral.levels.inputBlack,0.);QCOMPARE(neutral.levels.inputWhite,1.);
        QCOMPARE(neutral.levels.gamma,1.);QCOMPARE(neutral.levels.outputBlack,0.);QCOMPARE(neutral.levels.outputWhite,1.);
        enter(black,"51");enter(white,"204");enter(gamma,"2");enter(outputBlack,"25.5");enter(outputWhite,"229.5");
        const auto mapped=dialog.parameters();QCOMPARE(mapped.levels.inputBlack,.2);QCOMPARE(mapped.levels.inputWhite,.8);
        QCOMPARE(mapped.levels.gamma,2.);QCOMPARE(mapped.levels.outputBlack,.1);QCOMPARE(mapped.levels.outputWhite,.9);
        auto* buttons=dialog.findChild<QDialogButtonBox*>("photoAdjustmentButtons");QVERIFY(buttons);
        black->setValue(204);QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY_EXCEPTION_THROWN(dialog.parameters(),std::invalid_argument);
        dialog.accept();QVERIFY(dialog.isVisible());QCOMPARE(dialog.result(),int(QDialog::Rejected));
        auto* error=dialog.findChild<QLabel*>("photoAdjustmentValidation");QVERIFY(error && !error->text().isEmpty());
        black->setValue(0);outputBlack->setValue(240);QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        outputBlack->setValue(0);QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok),Qt::LeftButton);QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void colorBalanceUsesRelativeUnitsAndCancel() {
        PhotoAdjustmentDialog dialog(engine::AdjustmentKind::ColorBalance);dialog.show();QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.windowTitle(),QString("Color Balance"));
        auto* warmth=dialog.findChild<QDoubleSpinBox*>("balanceWarmth");auto* tint=dialog.findChild<QDoubleSpinBox*>("balanceTint");
        QVERIFY(warmth && tint);QCOMPARE(warmth->accessibleName(),QString("Warmth"));QCOMPARE(tint->accessibleName(),QString("Tint"));
        QCOMPARE(dialog.parameters().colorBalance.warmth,0.);QCOMPARE(dialog.parameters().colorBalance.tint,0.);
        enter(warmth,"30");enter(tint,"-40");
        QCOMPARE(dialog.parameters().colorBalance.warmth,.3);QCOMPARE(dialog.parameters().colorBalance.tint,-.4);
        auto* buttons=dialog.findChild<QDialogButtonBox*>("photoAdjustmentButtons");QVERIFY(buttons);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel),Qt::LeftButton);QCOMPARE(dialog.result(),int(QDialog::Rejected));
    }
    void curvesGraphPresetsKeyboardAndNumericEdit() {
        PhotoAdjustmentDialog dialog(engine::AdjustmentKind::Curves);dialog.show();QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* graph=dialog.findChild<QWidget*>("toneCurveEditor");auto* presets=dialog.findChild<QComboBox*>("curvePreset");
        auto* input=dialog.findChild<QDoubleSpinBox*>("curvePointInput");auto* output=dialog.findChild<QDoubleSpinBox*>("curvePointOutput");
        QVERIFY(graph && presets && input && output);QVERIFY(!graph->accessibleName().isEmpty());
        QCOMPARE(dialog.parameters().curve.size(),std::size_t{2});
        QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,.5,.5));
        QCOMPARE(dialog.parameters().curve.size(),std::size_t{3});QVERIFY(input->isEnabled() && output->isEnabled());
        const auto before=dialog.parameters().curve[1].output;QTest::keyClick(graph,Qt::Key_Up);
        QVERIFY(dialog.parameters().curve[1].output>before);
        enter(output,"191.25");QCOMPARE(dialog.parameters().curve[1].output,.75);
        graph->setFocus();QTest::keyClick(graph,Qt::Key_Delete);QCOMPARE(dialog.parameters().curve.size(),std::size_t{2});
        presets->setFocus();QTest::keyClick(presets,Qt::Key_Home);QTest::keyClick(presets,Qt::Key_Down);
        QCOMPARE(presets->currentText(),QString("Lift shadows"));QCOMPARE(dialog.parameters().curve[1].output,.38);
        QTest::keyClick(presets,Qt::Key_End);QCOMPARE(presets->currentText(),QString("Gentle contrast"));
        QCOMPARE(dialog.parameters().curve[1].output,.18);
        QTest::mousePress(graph,Qt::LeftButton,{},graphPoint(graph,.25,.18));
        QTest::mouseMove(graph,graphPoint(graph,.35,.3));QTest::mouseRelease(graph,Qt::LeftButton,{},graphPoint(graph,.35,.3));
        QVERIFY(dialog.parameters().curve[1].input>.3);QVERIFY(dialog.parameters().curve[1].output>.25);
        QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,0,0));QTest::keyClick(graph,Qt::Key_Delete);
        QCOMPARE(dialog.parameters().curve.size(),std::size_t{5});QCOMPARE(dialog.parameters().curve.front().input,0.);QCOMPARE(dialog.parameters().curve.front().output,0.);
        QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,1,1));QTest::keyClick(graph,Qt::Key_Left);
        QCOMPARE(dialog.parameters().curve.back().input,1.);QCOMPARE(dialog.parameters().curve.back().output,1.);
    }
    void curvePointCountAndOrderStayBounded() {
        PhotoAdjustmentDialog dialog(engine::AdjustmentKind::Curves);dialog.resize(650,430);dialog.show();QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* graph=dialog.findChild<QWidget*>("toneCurveEditor");QVERIFY(graph);
        for(int i=1;i<16;++i) QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,static_cast<double>(i)/16,.6));
        const auto points=dialog.parameters().curve;QCOMPARE(points.size(),std::size_t{16});
        for(std::size_t i=1;i<points.size();++i) QVERIFY(points[i-1].input<points[i].input);
        QCOMPARE(points.front().input,0.);QCOMPARE(points.back().input,1.);
    }
};
QTEST_MAIN(PhotoAdjustmentDialogTest)
#include "photo_adjustment_dialog_test.moc"
