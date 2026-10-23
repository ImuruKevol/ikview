#include "document.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// A QCoreApplication benchmark: no native window, scene graph, or GUI workload.
// Measure the synchronous navigation call separately from background decoding.
static bool settle(ik::Document &document)
{
    QElapsedTimer timer;
    timer.start();
    while (document.isBusy() && timer.elapsed() < 20000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return !document.isBusy() && !document.currentImage().isNull();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QElapsedTimer timer;
    timer.start();
    const auto extensions = ik::Document::supportedExtensions();
    const double cold = timer.nsecsElapsed() / 1e6;
    timer.restart();
    qsizetype copies = 0;
    for (int i = 0; i < 100000; ++i) copies += ik::Document::supportedExtensions().size();
    out << "extensions=" << extensions.size() << " cold_ms=" << cold
        << " warm_100000_ms=" << timer.nsecsElapsed() / 1e6 << " checksum=" << copies << Qt::endl;

    // Tiny identical images keep codec and storage size out of the call timings.
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString base = temp.filePath("image.png");
    if (!image.save(base)) return 2;
    QStringList all;
    for (int i = 0; i < 4000; ++i) {
        const QString path = temp.filePath(QString("file%1.png").arg(i, 4, 10, QChar('0')));
#ifdef Q_OS_UNIX
        if (::link(QFile::encodeName(base).constData(), QFile::encodeName(path).constData())) return 3;
#else
        if (!QFile::copy(base, path)) return 3;
#endif
        all.append(path);
    }
    for (const int count : {20, 200, 1000, 4000}) {
        ik::Document document;
        timer.restart();
        document.openFiles(all.mid(0, count), false);
        const double openCall = timer.nsecsElapsed() / 1e6;
        if (!settle(document)) return 4;
        QList<double> samples;
        for (int i = 1; i <= 8; ++i) {
            timer.restart();
            document.goTo(i);
            samples.append(timer.nsecsElapsed() / 1e6);
            if (!settle(document)) return 5;
        }
        std::sort(samples.begin(), samples.end());
        out << "explicit_count=" << count << " open_call_ms=" << openCall
            << " goto_median_ms=" << samples[samples.size() / 2]
            << " goto_max_ms=" << samples.last() << Qt::endl;
    }

    // A non-image write should leave the image, decoded cache, and model intact.
    ik::Document document;
    document.open(temp.path());
    if (!settle(document)) return 6;
    int resets = 0, images = 0;
    QObject::connect(&document, &ik::Document::entriesChanged, &app, [&] { ++resets; });
    QObject::connect(&document, &ik::Document::imageReady, &app, [&](const QImage &, const ik::ImageInfo &) { ++images; });
    QFile settings(temp.filePath("settings.ini"));
    if (!settings.open(QIODevice::WriteOnly)) return 7;
    settings.write("windowMoved=1\n");
    settings.close();
    timer.restart();
    while (timer.elapsed() < 900) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    out << "nonimage_write_model_resets=" << resets
        << " nonimage_write_image_deliveries=" << images << Qt::endl;
    return 0;
}
