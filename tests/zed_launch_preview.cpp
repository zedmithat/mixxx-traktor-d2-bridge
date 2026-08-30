#include <QApplication>
#include <QDir>
#include <QTimer>

#include "skin/legacy/launchimage.h"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (argc != 3) {
        return 2;
    }
    const QString skinRoot = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    const QString outputRoot = QDir(QString::fromLocal8Bit(argv[2])).absolutePath();
    const QString style = QStringLiteral(
            "LaunchImage { background-color:#020406; }"
            "QLabel { image:none; border:none; padding:0; margin:0; background:transparent; }"
            "QLabel#zedLaunchLetterZ { image:url(%1/images/zed_launch_z.svg); min-width:108px; max-width:108px; min-height:132px; max-height:132px; }"
            "QLabel#zedLaunchLetterE { image:url(%1/images/zed_launch_e.svg); min-width:108px; max-width:108px; min-height:132px; max-height:132px; }"
            "QLabel#zedLaunchLetterD { image:url(%1/images/zed_launch_d.svg); min-width:108px; max-width:108px; min-height:132px; max-height:132px; }"
            "QProgressBar { background-color:#10161b; border:none; padding:0; margin-top:8px; min-width:328px; max-width:328px; min-height:3px; max-height:3px; }"
            "QProgressBar::chunk { background-color:#35d8f2; }")
                                  .arg(skinRoot);

    LaunchImage launchImage(nullptr, style);
    launchImage.resize(800, 450);
    launchImage.progress(62, QString());
    launchImage.show();

    const int captureTimes[] = {100, 370, 700, 920};
    for (int frame = 0; frame < 4; ++frame) {
        QTimer::singleShot(captureTimes[frame], &launchImage,
                [&launchImage, outputRoot, frame]() {
                    launchImage.grab().save(
                            QStringLiteral("%1/zed-launch-%2.png")
                                    .arg(outputRoot)
                                    .arg(frame));
                });
    }
    QTimer::singleShot(1000, &application, &QApplication::quit);
    return application.exec();
}
