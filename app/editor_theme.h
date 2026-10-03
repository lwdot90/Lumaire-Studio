#pragma once
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QVariant>
#include <QStyleFactory>

namespace compositor {
inline void applyEditorTheme() {
    if(qApp->property("editorThemeApplied").toBool()) return;
    qApp->setProperty("editorThemeApplied",true);
    qApp->setStyle(QStyleFactory::create("Fusion"));
    auto font=qApp->font();font.setPointSizeF(9);qApp->setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window,QColor("#414141"));
    palette.setColor(QPalette::WindowText,QColor("#dddddd"));
    palette.setColor(QPalette::Base,QColor("#353535"));
    palette.setColor(QPalette::AlternateBase,QColor("#3d3d3d"));
    palette.setColor(QPalette::Text,QColor("#dddddd"));
    palette.setColor(QPalette::Button,QColor("#4a4a4a"));
    palette.setColor(QPalette::ButtonText,QColor("#dddddd"));
    palette.setColor(QPalette::Light,QColor("#777777"));
    palette.setColor(QPalette::Mid,QColor("#555555"));
    palette.setColor(QPalette::Dark,QColor("#282828"));
    palette.setColor(QPalette::Highlight,QColor("#626262"));
    palette.setColor(QPalette::HighlightedText,Qt::white);
    palette.setColor(QPalette::ToolTipBase,QColor("#eeeeee"));
    palette.setColor(QPalette::ToolTipText,QColor("#202020"));
    palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#8c8c8c"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#8c8c8c"));
    qApp->setPalette(palette);
    qApp->setStyleSheet(R"(
        QMainWindow::separator { background: #292929; width: 3px; height: 3px; }
        QMenuBar { background: #303030; padding: 0; }
        QMenuBar::item { padding: 4px 8px; background: transparent; }
        QMenuBar::item:selected { background: #555555; }
        QMenu { padding: 3px; border: 1px solid #222222; }
        QMenu::item { padding: 5px 24px 5px 12px; }
        QToolBar { background: #414141; border: 0; spacing: 3px; padding: 3px; }
        QToolBar#brushSettingsToolbar { border-bottom: 1px solid #292929; min-height: 30px; }
        QToolBar#editingToolbar { background: #454545; border-right: 1px solid #292929; padding: 1px; }
        QToolBar QLabel { padding: 0 3px; }
        QToolBar::separator { background: #303030; width: 1px; height: 1px; margin: 4px; }
        QToolButton { padding: 4px; border: 1px solid transparent; border-radius: 1px; }
        QToolButton:hover { background: #555555; border-color: #666666; }
        QToolButton:checked { background: #303030; border-color: #777777; }
        QToolButton:disabled { color: #858585; }
        QToolButton#foregroundSwatch, QToolButton#backgroundSwatch { padding: 0; border: 0; }
        QToolButton[role="adjustmentButton"] { padding: 4px 8px; }
        QDockWidget { font-weight: 600; }
        QDockWidget::title { background: #333333; padding: 5px 7px; border-bottom: 1px solid #242424; }
        QDockWidget QWidget { font-weight: normal; }
        QLabel[mutedLabel="true"], QLabel[role="mutedLabel"], QLabel#mutedLabel { color: #b5b5b5; }
        QLabel#optionsToolLabel { font-weight: 600; padding-right: 8px; }
        QTreeWidget { border: 0; background: #414141; outline: none; }
        QTreeWidget::item { padding: 1px; border-bottom: 1px solid #353535; }
        QTreeWidget::item:selected { background: #626262; }
        QHeaderView::section { background: #414141; border: 0; padding: 3px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #353535; border: 1px solid #575757; border-radius: 2px; padding: 1px 3px; min-height: 19px; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid #9baec2; }
        QCheckBox { spacing: 4px; }
        QTabWidget::pane { border: 0; }
        QTabBar::tab { background: #353535; color: #bbbbbb; padding: 6px 10px; border-right: 1px solid #292929; }
        QTabBar::tab:selected { background: #4a4a4a; color: #eeeeee; border-bottom: 1px solid #a0a0a0; }
        QStatusBar { background: #353535; color: #b8b8b8; font-size: 11px; }
        QStatusBar::item { border: 0; }
        QDialog QPushButton { padding: 4px 12px; }
    )");
}
}
