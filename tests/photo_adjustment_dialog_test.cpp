#include "app/photo_adjustment_dialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
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
    void initializedScalarEditorsAndReset() {
        for(const auto kind:{engine::AdjustmentKind::Exposure,engine::AdjustmentKind::Brightness,engine::AdjustmentKind::Contrast,engine::AdjustmentKind::Saturation}) {
            engine::AdjustmentParameters initial;initial.kind=kind;initial.value=.123456789;
            PhotoAdjustmentDialog dialog(initial);dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
            QCOMPARE(dialog.parameters(),initial);QVERIFY(dialog.parametersValid());
            auto* scalar=dialog.findChild<QDoubleSpinBox*>("photoAdjustmentValue");auto* reset=dialog.findChild<QPushButton*>("photoAdjustmentReset");QVERIFY(scalar && reset);
            QSignalSpy changes(&dialog,&PhotoAdjustmentDialog::parametersChanged);
            enter(scalar,"0.25");QCOMPARE(dialog.parameters().value,.25);QCOMPARE(changes.count(),1);
            QTest::mouseClick(reset,Qt::LeftButton);QCOMPARE(dialog.parameters().value,kind==engine::AdjustmentKind::Saturation ? 1. : 0.);QCOMPARE(changes.count(),2);
            QTest::mouseClick(reset,Qt::LeftButton);QCOMPARE(changes.count(),2);
        }
        PhotoAdjustmentDialog saturation(engine::AdjustmentKind::Saturation);QCOMPARE(saturation.parameters().value,1.);
    }
    void exactInitializedValuesSurviveUnrelatedChanges() {
        engine::AdjustmentParameters initial;initial.kind=engine::AdjustmentKind::Levels;
        initial.levels={.123456789,.876543219,1.23456789,.024681357,.975318642};
        PhotoAdjustmentDialog dialog(initial);QCOMPARE(dialog.parameters(),initial);
        auto* gamma=dialog.findChild<QDoubleSpinBox*>("levelsGamma");QVERIFY(gamma);gamma->setValue(2);
        auto expected=initial;expected.levels.gamma=2;QCOMPARE(dialog.parameters(),expected);
        engine::AdjustmentParameters balance;balance.kind=engine::AdjustmentKind::ColorBalance;balance.colorBalance={.123456789,-.234567891};
        PhotoAdjustmentDialog balanceDialog(balance);QCOMPARE(balanceDialog.parameters(),balance);
        balanceDialog.findChild<QDoubleSpinBox*>("balanceWarmth")->setValue(30);
        balance.colorBalance.warmth=.3;QCOMPARE(balanceDialog.parameters(),balance);
        engine::AdjustmentParameters curve;curve.kind=engine::AdjustmentKind::Curves;curve.curve={{0,0},{.314159265,.271828182},{1,1}};
        PhotoAdjustmentDialog curveDialog(curve);QCOMPARE(curveDialog.parameters(),curve);
        auto* reset=curveDialog.findChild<QPushButton*>("photoAdjustmentReset");QVERIFY(reset);reset->click();
        QCOMPARE(curveDialog.parameters().curve,(std::vector<engine::CurvePoint>{{0,0},{1,1}}));
    }
    void embeddedValidityAndCommittedTypedFields() {
        QWidget host;auto* layout=new QVBoxLayout(&host);
        engine::AdjustmentParameters initial;initial.kind=engine::AdjustmentKind::Levels;
        auto* editor=new PhotoAdjustmentDialog(initial,&host,true);layout->addWidget(editor);host.show();QVERIFY(QTest::qWaitForWindowActive(&host));
        QVERIFY(!editor->isWindow());auto* buttons=editor->findChild<QDialogButtonBox*>("photoAdjustmentButtons");QVERIFY(buttons);QVERIFY(!buttons->isVisible());
        QSignalSpy validity(editor,&PhotoAdjustmentDialog::validityChanged),changes(editor,&PhotoAdjustmentDialog::parametersChanged);
        auto* black=editor->findChild<QDoubleSpinBox*>("levelsInputBlack");QVERIFY(black);black->setValue(255);
        QVERIFY(!editor->parametersValid());QCOMPARE(validity.count(),1);QCOMPARE(validity.last()[0].toBool(),false);QCOMPARE(changes.count(),0);
        black->setValue(0);QVERIFY(editor->parametersValid());QCOMPARE(validity.count(),2);QCOMPARE(changes.count(),1);
        auto* gamma=editor->findChild<QDoubleSpinBox*>("levelsGamma");QVERIFY(gamma);
        auto* input=gamma->findChild<QLineEdit*>();QVERIFY(input);QTest::mouseClick(input,Qt::LeftButton);QTRY_VERIFY(input->hasFocus());
        QTest::keyClick(input,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(input,"2.5");
        QCOMPARE(editor->parameters().levels.gamma,1.);editor->commitTypedControls();QCOMPARE(editor->parameters().levels.gamma,2.5);QCOMPARE(changes.count(),2);
    }
    void channelSwitchPreservesExactCurvesAndSelectedEdit() {
        engine::AdjustmentParameters initial;initial.kind=engine::AdjustmentKind::Curves;
        initial.curve={{0,0},{.314159265,.271828182},{1,1}};
        initial.channelCurves[0]={{0,0},{.321987654,.234567891},{1,1}};
        initial.channelCurves[1]={{0,0},{.456789123,.567891234},{1,1}};
        initial.channelCurves[2]={{0,0},{.678912345,.789123456},{1,1}};
        PhotoAdjustmentDialog dialog(initial);dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        auto* channel=dialog.findChild<QComboBox*>("curveChannel");auto* graph=dialog.findChild<QWidget*>("toneCurveEditor");
        auto* output=dialog.findChild<QDoubleSpinBox*>("curvePointOutput");auto* preset=dialog.findChild<QComboBox*>("curvePreset");QVERIFY(channel && graph && output && preset);
        QCOMPARE(channel->count(),4);QCOMPARE(channel->itemText(0),QString("RGB"));QCOMPARE(channel->itemText(1),QString("Red"));QCOMPARE(channel->itemText(2),QString("Green"));QCOMPARE(channel->itemText(3),QString("Blue"));
        QSignalSpy changes(&dialog,&PhotoAdjustmentDialog::parametersChanged),validity(&dialog,&PhotoAdjustmentDialog::validityChanged);
        for(int index:{1,2,3,0,3,2,1}) {channel->setCurrentIndex(index);QCOMPARE(dialog.parameters(),initial);QCOMPARE(changes.count(),0);}
        QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,initial.channelCurves[0][1].input,initial.channelCurves[0][1].output));
        QCOMPARE(changes.count(),0);enter(output,"127.5");
        auto expected=initial;expected.channelCurves[0][1].output=.5;QCOMPARE(dialog.parameters(),expected);QCOMPARE(changes.count(),1);QVERIFY(dialog.parametersValid());QCOMPARE(validity.count(),0);
        channel->setCurrentIndex(2);QCOMPARE(changes.count(),1);preset->setCurrentIndex(1);
        expected.channelCurves[1]={{0,0},{.25,.38},{.5,.62},{.75,.82},{1,1}};QCOMPARE(dialog.parameters(),expected);QCOMPARE(changes.count(),2);
        channel->setCurrentIndex(3);QCOMPARE(changes.count(),2);
        auto* reset=dialog.findChild<QPushButton*>("photoAdjustmentReset");QVERIFY(reset);QCOMPARE(reset->text(),QString("Reset all curves"));
        QTest::mouseClick(reset,Qt::LeftButton);engine::AdjustmentParameters identity;identity.kind=engine::AdjustmentKind::Curves;
        QCOMPARE(dialog.parameters(),identity);QCOMPARE(changes.count(),3);QTest::mouseClick(reset,Qt::LeftButton);QCOMPARE(changes.count(),3);
    }
    void typedChannelValueCommitsBeforeAcceptOrSwitch() {
        engine::AdjustmentParameters initial;initial.kind=engine::AdjustmentKind::Curves;initial.channelCurves[2]={{0,0},{.5,.4},{1,1}};
        PhotoAdjustmentDialog dialog(initial);dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        auto* channel=dialog.findChild<QComboBox*>("curveChannel");auto* graph=dialog.findChild<QWidget*>("toneCurveEditor");auto* output=dialog.findChild<QDoubleSpinBox*>("curvePointOutput");QVERIFY(channel && graph && output);
        channel->setCurrentIndex(3);QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,.5,.4));
        auto* editor=output->findChild<QLineEdit*>();QVERIFY(editor);QTest::mouseClick(editor,Qt::LeftButton);QTRY_VERIFY(editor->hasFocus());
        QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(editor,"153");
        channel->setCurrentIndex(1);QCOMPARE(dialog.parameters().channelCurves[2][1].output,.6);
        QCOMPARE(dialog.parameters().channelCurves[0],initial.channelCurves[0]);QCOMPARE(dialog.parameters().curve,initial.curve);
        channel->setCurrentIndex(3);QTest::mouseClick(graph,Qt::LeftButton,{},graphPoint(graph,.5,.6));
        QTest::mouseClick(editor,Qt::LeftButton);QTRY_VERIFY(editor->hasFocus());QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(editor,"178.5");
        dialog.accept();QCOMPARE(dialog.result(),int(QDialog::Accepted));QCOMPARE(dialog.parameters().channelCurves[2][1].output,.7);
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
