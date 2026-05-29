// SPDX-License-Identifier: MPL-2.0

#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
