#pragma once
#include <QString>
namespace compositor {
class MainWindow;
// Run actual asynchronous editor commands, save/export artifacts in an absolute
// directory, report each completion, and leave the edited window open.
void runDevelopmentDemo(MainWindow& window,const QString& directory);
}
