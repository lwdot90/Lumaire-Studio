#include "window.h"
#include "app/development_demo.h"
#include <QFileInfo>
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <iostream>

int main(int argc,char** argv) {
    QApplication app(argc,argv); app.setApplicationName("lumaire-studio"); app.setApplicationDisplayName("Lumaire Studio"); app.setApplicationVersion("0.1.0-dev");
    app.setDesktopFileName("lumaire-studio");
    QCommandLineParser parser; parser.setApplicationDescription("Lumaire Studio — image and project editor"); parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument("file","PNG, JPEG or .cproj to open","[file]");
    parser.addOption({"backend","Renderer: auto or cpu","backend","auto"});
    parser.addOption({"device","Vulkan device name substring (unmatched selects CPU fallback)","name"});
    parser.addOption({"require-validation","Fail if Vulkan validation is unavailable"});
    parser.addOption({"timing-json","Write bounded input/render timing capture on exit","path"});
    parser.addOption({"demo-dir","Run the edit/save/export demonstration and keep the window open","directory"});
    parser.addOption({"smoke-ms","Exit after a bounded desktop smoke run","milliseconds"});
    parser.process(app);
    if(parser.value("backend")!="auto" && parser.value("backend")!="cpu") parser.showHelp(2);
    const bool cpu=parser.value("backend")=="cpu";
    if(cpu && parser.isSet("require-validation")) { std::cerr<<"CPU backend cannot require Vulkan validation\n"; return 2; }
    QVulkanInstance instance;
    bool validation=false;
    if(!cpu) {
        instance.setApiVersion(QVersionNumber(1,2));
        if(COMPOSITOR_VALIDATION || parser.isSet("require-validation")) {
            validation=instance.supportedLayers().contains("VK_LAYER_KHRONOS_validation");
            if(validation) instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        }
        if(parser.isSet("require-validation") && !validation) { std::cerr<<"Required Vulkan validation layer unavailable\n"; return 2; }
        if(!instance.create()) {
            if(parser.isSet("require-validation")) { std::cerr<<"Required Vulkan instance creation failed\n"; return 2; }
            std::cerr<<"Vulkan instance unavailable; selecting CPU canvas\n";
        }
    }
    try {
        compositor::MainWindow window(&instance,cpu,parser.value("device")); window.show();
        for(const auto& path:parser.positionalArguments()) window.openPath(path);
        if(parser.isSet("demo-dir")) QTimer::singleShot(0,&window,[&window,&parser]{compositor::runDevelopmentDemo(window,QFileInfo(parser.value("demo-dir")).absoluteFilePath());});
        if(parser.isSet("smoke-ms")) {
            bool okay=false; const int delay=parser.value("smoke-ms").toInt(&okay);
            if(!okay || delay<100 || delay>60000) { std::cerr<<"smoke-ms must be 100–60000\n"; return 2; }
            QTimer::singleShot(delay,&window,&QWidget::close);
        }
        const int result=app.exec(); window.writeTiming(parser.value("timing-json")); return result;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
