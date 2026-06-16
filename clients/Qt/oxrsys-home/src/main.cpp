// SPDX-License-Identifier: MPL-2.0

#include "MainWindow.h"
#include "RuntimeManager.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/icons/oxrsys-home/appicon-256.png"));
    QString elevatedError;
    const int elevatedResult =
        RuntimeManager::handleElevatedWindowsCommand(app.arguments(), &elevatedError);
    if (elevatedResult >= 0)
    {
        if (elevatedResult != 0 && !elevatedError.isEmpty())
        {
            qCritical("%s", qPrintable(elevatedError));
        }
        return elevatedResult;
    }

    MainWindow window;
    window.show();

    return app.exec();
}
