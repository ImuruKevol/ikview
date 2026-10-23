#include "mainwindow.h"
#include "document.h"
#include "imagecanvas.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Controlled event-loop benchmark, not a physical mouse/WindowServer latency test.
static double cpuMs() {
#ifdef Q_OS_UNIX
    rusage r{}; getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0
        + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
#elif defined(Q_OS_WIN)
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto ticks=[](const FILETIME &t){return (quint64(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return (ticks(kernel)+ticks(user))/10000.0;
#else
    return 0;
#endif
}
static double percentile(QList<double> values, double p) {
    if (values.isEmpty()) return 0;
    std::sort(values.begin(), values.end());
    return values[qMin(values.size()-1, qsizetype(std::ceil(p*values.size())-1))];
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ikview-performance");
    QCoreApplication::setApplicationName("ikview-performance");
    QStandardPaths::setTestModeEnabled(true);
    QImageReader::setAllocationLimit(512);
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window,QColor("#15191f"));
    palette.setColor(QPalette::WindowText,QColor("#e5eaf0"));
    palette.setColor(QPalette::Base,QColor("#101419"));
    palette.setColor(QPalette::AlternateBase,QColor("#1b222a"));
    palette.setColor(QPalette::Text,QColor("#dce4ea"));
    palette.setColor(QPalette::Button,QColor("#222b34"));
    palette.setColor(QPalette::ButtonText,QColor("#e5eaf0"));
    palette.setColor(QPalette::Highlight,QColor("#214a43"));
    palette.setColor(QPalette::HighlightedText,QColor("#9ff5d7"));
    palette.setColor(QPalette::ToolTipBase,QColor("#27313b"));
    palette.setColor(QPalette::ToolTipText,QColor("#f4f6f8"));
    palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#66717b"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#66717b"));
    app.setPalette(palette);
    auto font=app.font(); font.setPointSize(12); app.setFont(font);
    QTemporaryDir temp;
    qputenv("IKVIEW_SETTINGS_PATH",temp.filePath("settings.ini").toUtf8());
    const auto filter = qEnvironmentVariable("IKVIEW_BENCHMARK_FILTER", "soften");
    if (filter != "none" && filter != "soften" && filter != "sharpen" && filter != "soften-sharpen") return 2;
    { QSettings settings(temp.filePath("settings.ini"),QSettings::IniFormat); settings.setValue("imageFilter",filter); }
    ik::MainWindow window; window.resize(1200,800); window.show();
    auto *canvas=window.findChild<ik::ImageCanvas *>();
    auto *document=window.findChild<ik::Document *>();
    if (!canvas || !document) return 2;
    QJsonArray phases;
    quint64 actionChanges=0;
    for (auto *action: window.findChildren<QAction *>())
        QObject::connect(action,&QAction::changed,&window,[&]{++actionChanges;});
    std::atomic<quint64> frames{0};
    QList<QMetaObject::Connection> connections;
    for (auto *w: QGuiApplication::allWindows())
        if (auto *quick=qobject_cast<QQuickWindow *>(w))
            connections.append(QObject::connect(quick,&QQuickWindow::frameSwapped,&window,[&]{frames.fetch_add(1,std::memory_order_relaxed);},Qt::DirectConnection));
    auto phase=[&](QString name,int duration,int interval,std::function<void(int)> action={}, int requestedOperations=0) {
        QTest::qWait(350);
        const auto before=canvas->activityStats();
        const auto changedBefore=actionChanges, framesBefore=frames.load();
        const double cpuBefore=cpuMs();
        QElapsedTimer clock; clock.start();
        double last=0; QList<double> gaps, costs;
        int operations=0;
        QEventLoop loop; QTimer heartbeat, drive;
        heartbeat.setTimerType(Qt::PreciseTimer); heartbeat.setInterval(5);
        QObject::connect(&heartbeat,&QTimer::timeout,&loop,[&]{const double now=clock.nsecsElapsed()/1e6;gaps.append(now-last);last=now;});
        drive.setTimerType(Qt::PreciseTimer); drive.setInterval(interval);
        QObject::connect(&drive,&QTimer::timeout,&loop,[&]{
            QElapsedTimer call;call.start();action(operations++);costs.append(call.nsecsElapsed()/1e6);
            if(requestedOperations && operations>=requestedOperations){drive.stop();QTimer::singleShot(60,Qt::PreciseTimer,&loop,&QEventLoop::quit);}
        });
        heartbeat.start(); if(action)drive.start();
        QTimer::singleShot(requestedOperations?duration+10000:duration,Qt::PreciseTimer,&loop,&QEventLoop::quit);loop.exec();
        const double elapsed=clock.nsecsElapsed()/1e6;
        const auto after=canvas->activityStats();
        QJsonObject item{{"phase",name},{"elapsed_ms",elapsed},{"operations",operations},
            {"requested_operations",requestedOperations},{"canvas_width",canvas->width()},{"canvas_height",canvas->height()},
            {"device_pixel_ratio",canvas->devicePixelRatioF()},
            {"heartbeat_samples",gaps.size()},{"heartbeat_p95_ms",percentile(gaps,.95)},
            {"heartbeat_p99_ms",percentile(gaps,.99)},{"heartbeat_max_ms",percentile(gaps,1)},
            {"gaps_over_16_7ms",std::count_if(gaps.begin(),gaps.end(),[](double n){return n>16.7;})},
            {"gaps_over_50ms",std::count_if(gaps.begin(),gaps.end(),[](double n){return n>50;})},
            {"call_p95_ms",percentile(costs,.95)},{"call_max_ms",percentile(costs,1)},
            {"process_cpu_percent",(cpuMs()-cpuBefore)/elapsed*100},
            {"action_changes",qint64(actionChanges-changedBefore)},{"frames",qint64(frames.load()-framesBefore)},
            {"scene_syncs",qint64(after.sceneSyncCalls-before.sceneSyncCalls)},
            {"scene_updates",qint64(after.sceneUpdates-before.sceneUpdates)},
            {"worker_jobs",qint64(after.preparationJobs-before.preparationJobs)},
            {"texture_uploads",qint64(after.textureUploads-before.textureUploads)},
            {"preview_uploads",qint64(after.previewUploads-before.previewUploads)},
            {"detail_uploads",qint64(after.detailUploads-before.detailUploads)},
            {"overlay_paints",qint64(after.overlayPaints-before.overlayPaints)}};
        phases.append(item);
        fprintf(stderr,"%s: p99 %.2fms max %.2fms calls %.2fms CPU %.1f%% actions %lld frames %lld\n",qPrintable(name),percentile(gaps,.99),percentile(gaps,1),percentile(costs,1),item["process_cpu_percent"].toDouble(),item["action_changes"].toInteger(),item["frames"].toInteger());
    };
    QTest::qWait(500);
    phase("empty_idle",1500,16);
    const QString imageDir=temp.filePath("images"); QDir().mkpath(imageDir);
    for(int i=0;i<3;++i) {
        QImage image(2560,1600,QImage::Format_RGB32);image.fill(QColor::fromHsv(i*90,150,180));
        image.save(imageDir+QString("/%1.png").arg(i));
    }
    window.openPaths({imageDir});
    QElapsedTimer ready; ready.start();
    while((document->isBusy()||document->currentImage().isNull()||canvas->isPreparing())&&ready.elapsed()<15000)QTest::qWait(20);
    if(document->currentImage().isNull())return 3;
    phase("image_idle",2000,16);
    const QPoint origin=window.pos();
    phase("window_move",2500,16,[&](int n){window.move(origin+QPoint(int(50*std::sin(n*.12)),int(30*std::cos(n*.12))));},150);
    phase("fit_resize",3000,16,[&](int n){window.resize(1120+int(110*std::sin(n*.1)),740+int(90*std::cos(n*.1)));},180);
    window.resize(1200,800);
    auto click=[&](const QString &name){
        auto *action=window.findChild<QAction *>(name);
        for(auto *b:window.findChildren<QToolButton *>())if(b->defaultAction()==action){QTest::mouseClick(b,Qt::LeftButton);return;}
        if(action)action->trigger();
    };
    phase("sidebar_button",2200,80,[&](int){click("toggleFileListAction");},24);
    phase("navigation_action",2200,80,[&](int n){click(n%2?"previousImageAction":"nextImageAction");},28);
    // Generate large source outside all measured intervals.
    QImage large(10000,8000,QImage::Format_RGB32);large.fill(QColor("#406080"));
    canvas->setImage(large);canvas->setFitMode(ik::FitMode::Fit);
    QTest::qWait(1000);
    phase("80mp_fit_resize",3000,16,[&](int n){window.resize(1120+int(110*std::sin(n*.1)),740+int(90*std::cos(n*.1)));},180);
    window.resize(1200,800);
    canvas->setZoom(1.0);QTest::qWait(1000);
    phase("80mp_pan",2500,16,[&](int n){canvas->panViewport(QPointF(n%80<40?24:-24,n%100<50?8:-8));},150);
    phase("80mp_idle",1500,16);
    canvas->setFitMode(ik::FitMode::Fit);
    phase("80mp_effect_and_window_move",3000,16,[&](int n){
        window.move(origin+QPoint(int(50*std::sin(n*.12)),int(30*std::cos(n*.12))));
        if(n%20==0)canvas->setGamma(n%40?1.7:.8);
    },180);
    for(const auto &c:connections)QObject::disconnect(c);
    QJsonObject result{{"qt_version",QT_VERSION_STR},{"renderer",canvas->rendererDescription()},
        {"image_filter",filter},{"heartbeat_interval_ms",5},{"method","QTimer driven QWidget move/resize, QTest sidebar clicks and QAction navigation; 80MP source injected into canvas; no physical-input latency measurement"},{"phases",phases}};
    const QByteArray json=QJsonDocument(result).toJson();
    if(argc>1){QFile output(QString::fromLocal8Bit(argv[1]));if(!output.open(QIODevice::WriteOnly))return 4;output.write(json);}
    else fwrite(json.constData(),1,json.size(),stdout);
    return 0;
}
