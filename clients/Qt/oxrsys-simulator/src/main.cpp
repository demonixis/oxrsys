// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    SimulatorWidget widget;
    widget.setWindowTitle("OXRSys Simulator");
    widget.resize(1280, 720);
    widget.show();

    return app.exec();
}
