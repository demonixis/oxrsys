// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/icons/oxrsys-simulator/icon-256.png"));

    SimulatorWidget widget;
    widget.setWindowTitle("OXRSys Simulator");
    widget.resize(1280, 720);
    widget.show();

    return app.exec();
}
