// Dependency-only control for sanitizer triage. Deliberately links no
// Compositor code: failures here cannot originate in the canvas or renderer.
#include <QApplication>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWindow>
#include <QVulkanInstance>

class RasterProbe final:public QWidget {
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(),QColor(32,32,32));
        // Exercise Qt's own parallel raster path, as the CPU canvas does.
        for(int i=0;i<4;++i) painter.fillRect(rect(),QColor(128,128,128,128));
        painter.setPen(Qt::white);
        painter.drawText(20,40,"Qt runtime sanitizer control — no Compositor code");
    }
};

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    if(app.arguments().contains("--window-container-churn")) {
        QVulkanInstance instance;
        const bool vulkan=app.arguments().contains("--vulkan-containers");
        if(vulkan && !instance.create()) return 2;
        QTabWidget tabs;tabs.resize(1000,700);tabs.show();
        QTimer::singleShot(50,&tabs,[&] {
            for(int i=0;i<100;++i) {
                auto* page=new QWidget;auto* layout=new QVBoxLayout(page);
                auto* native=new QWindow;
                if(vulkan) {native->setSurfaceType(QSurface::VulkanSurface);native->setVulkanInstance(&instance);}
                layout->addWidget(QWidget::createWindowContainer(native,page));
                const int index=tabs.addTab(page,QString::number(i));tabs.setCurrentIndex(index);
                tabs.removeTab(index);delete page;
            }
        });
        QTimer::singleShot(500,&app,&QApplication::quit);
        return app.exec();
    }
    RasterProbe window;
    window.resize(1000,700);
    window.show();
    QTimer::singleShot(300,&app,&QApplication::quit);
    return app.exec();
}
