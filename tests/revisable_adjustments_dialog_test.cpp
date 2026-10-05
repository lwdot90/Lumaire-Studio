#include "app/revisable_adjustments_dialog.h"
#include "app/photo_adjustment_dialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace compositor;
class RevisableAdjustmentsDialogTest final:public QObject {
    Q_OBJECT
private slots:
    void exactInitialAndSelectionDoNotMutate() {
        engine::AdjustmentParameters exposure;exposure.value=1.234567;
        engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;levels.levels.gamma=1.234567;
        const std::vector initial{exposure,levels};RevisableAdjustmentsDialog dialog(initial);
        QSignalSpy changes(&dialog,&RevisableAdjustmentsDialog::operationsChanged);
        auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");QVERIFY(list);QCOMPARE(list->count(),2);
        list->setCurrentRow(1);list->setCurrentRow(0);QCOMPARE(changes.count(),0);QVERIFY(dialog.operations()==initial);
        dialog.accept();QCOMPARE(dialog.result(),int(QDialog::Accepted));QVERIFY(dialog.operations()==initial);
    }
    void addReorderRemoveAndCapacity() {
        RevisableAdjustmentsDialog dialog({});QSignalSpy changes(&dialog,&RevisableAdjustmentsDialog::operationsChanged);
        auto* kind=dialog.findChild<QComboBox*>("revisableAdjustmentsKind");auto* list=dialog.findChild<QListWidget*>("revisableAdjustmentsStack");
        auto* add=dialog.findChild<QPushButton*>("revisableAdjustmentsAdd");auto* remove=dialog.findChild<QPushButton*>("revisableAdjustmentsRemove");
        auto* up=dialog.findChild<QPushButton*>("revisableAdjustmentsUp");auto* down=dialog.findChild<QPushButton*>("revisableAdjustmentsDown");
        auto* buttons=dialog.findChild<QDialogButtonBox*>("revisableAdjustmentsButtons");QVERIFY(kind && list && add && remove && up && down && buttons);
        kind->setCurrentIndex(3);add->click();QCOMPARE(dialog.operations().size(),std::size_t(1));QCOMPARE(dialog.operations()[0].value,1.);
        QCOMPARE(changes.count(),1);QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());dialog.setPreviewState(false);
        kind->setCurrentIndex(4);add->click();QCOMPARE(dialog.operations().size(),std::size_t(2));
        dialog.setPreviewState(false);up->click();QVERIFY(dialog.operations()[0].kind==engine::AdjustmentKind::Levels);
        dialog.setPreviewState(false);down->click();QVERIFY(dialog.operations()[1].kind==engine::AdjustmentKind::Levels);
        dialog.setPreviewState(false);remove->click();QCOMPARE(dialog.operations().size(),std::size_t(1));
        remove->click();QVERIFY(dialog.operations().empty());QVERIFY(!remove->isEnabled());QVERIFY(!up->isEnabled());QVERIFY(!down->isEnabled());
        for(int i=0;i<16;++i) {add->click();}
        QCOMPARE(dialog.operations().size(),std::size_t(16));QVERIFY(!add->isEnabled());
        dialog.setPreviewState(false,"Preview refused");QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        dialog.setPreviewState(false);QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY_EXCEPTION_THROWN(RevisableAdjustmentsDialog(std::vector<engine::AdjustmentParameters>(17)),std::invalid_argument);
    }
    void invalidControlsAndTypedAcceptGuard() {
        engine::AdjustmentParameters levels;levels.kind=engine::AdjustmentKind::Levels;
        RevisableAdjustmentsDialog dialog({levels});dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QSignalSpy changes(&dialog,&RevisableAdjustmentsDialog::operationsChanged);
        auto* child=dialog.findChild<PhotoAdjustmentDialog*>();auto* black=dialog.findChild<QDoubleSpinBox*>("levelsInputBlack");
        auto* white=dialog.findChild<QDoubleSpinBox*>("levelsInputWhite");auto* gamma=dialog.findChild<QDoubleSpinBox*>("levelsGamma");
        auto* buttons=dialog.findChild<QDialogButtonBox*>("revisableAdjustmentsButtons");QVERIFY(child && black && white && gamma && buttons);
        black->setValue(255);QVERIFY(!child->parametersValid());QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        dialog.accept();QVERIFY(dialog.isVisible());QCOMPARE(changes.count(),0);
        black->setValue(0);QVERIFY(child->parametersValid());QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        gamma->setFocus();gamma->selectAll();QTest::keyClicks(gamma,"2");
        dialog.accept();QVERIFY(dialog.isVisible());QCOMPARE(dialog.operations()[0].levels.gamma,2.);
        QCOMPARE(changes.count(),1);QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        dialog.setPreviewState(false);dialog.accept();QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void channelSelectionPreservesExactStackAndEditingNeedsPreview() {
        engine::AdjustmentParameters curves;curves.kind=engine::AdjustmentKind::Curves;
        curves.curve={{0,0},{.412345678,.567891234},{1,1}};
        curves.channelCurves[0]={{0,0},{.345678912,.456789123},{1,1}};
        curves.channelCurves[1]={{0,0},{.456789123,.623456789},{1,1}};
        curves.channelCurves[2]={{0,0},{.567891234,.712345678},{1,1}};
        const std::vector initial{curves};RevisableAdjustmentsDialog dialog(initial);
        dialog.show();QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QSignalSpy changes(&dialog,&RevisableAdjustmentsDialog::operationsChanged);
        auto* channels=dialog.findChild<QComboBox*>("curveChannel");
        auto* plot=dialog.findChild<QWidget*>("toneCurveEditor");
        auto* output=dialog.findChild<QDoubleSpinBox*>("curvePointOutput");
        auto* buttons=dialog.findChild<QDialogButtonBox*>("revisableAdjustmentsButtons");
        QVERIFY(channels && plot && output && buttons);QCOMPARE(channels->count(),4);
        for(int channel=0;channel<4;++channel) {
            channels->setCurrentIndex(channel);QVERIFY(dialog.operations()==initial);
            QCOMPARE(changes.count(),0);QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        }
        channels->setCurrentIndex(2);
        const auto point=curves.channelCurves[1][1];
        const auto area=QRectF(plot->rect()).adjusted(18,14,-14,-18);
        QTest::mouseClick(plot,Qt::LeftButton,Qt::NoModifier,
            QPoint(qRound(area.left()+point.input*area.width()),qRound(area.bottom()-point.output*area.height())));
        QVERIFY(output->isEnabled());QCOMPARE(changes.count(),0);QVERIFY(dialog.operations()==initial);
        auto* line=output->findChild<QLineEdit*>();QVERIFY(line);
        QTest::mouseClick(line,Qt::LeftButton);QTest::keyClick(line,Qt::Key_A,Qt::ControlModifier);
        QTest::keyClicks(line,"157");QTest::keyClick(line,Qt::Key_Tab);
        QCOMPARE(changes.count(),1);const auto edited=dialog.operations()[0];
        QVERIFY(edited.curve==curves.curve);QVERIFY(edited.channelCurves[0]==curves.channelCurves[0]);
        QVERIFY(edited.channelCurves[2]==curves.channelCurves[2]);
        QCOMPARE(edited.channelCurves[1][1].input,point.input);
        QCOMPARE(edited.channelCurves[1][1].output,157./255.);
        QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());dialog.accept();QVERIFY(dialog.isVisible());
        dialog.setPreviewState(false);QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QSignalSpy accepted(&dialog,&QDialog::accepted);buttons->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(accepted.count(),0);QCOMPARE(dialog.result(),int(QDialog::Rejected));QVERIFY(initial[0]==curves);
    }
};
QTEST_MAIN(RevisableAdjustmentsDialogTest)
#include "revisable_adjustments_dialog_test.moc"
