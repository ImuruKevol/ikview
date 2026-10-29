#include "apptheme.h"
#include "mainwindow.h"
#include "imagecanvas.h"
#include "document.h"
#include "finderhandoff.h"
#ifdef Q_OS_MACOS
#include "macosregistration.h"
#include "macosservices.h"
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileOpenEvent>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSGRendererInterface>
#include <QStyleFactory>
#include <QStandardPaths>
#include <QTimer>
#include <QTextStream>
#include <functional>
#include <utility>
#include <cmath>

class Application final : public QApplication {
public:
    using QApplication::QApplication;
    std::function<void(const QStringList&)> openFiles;
    QStringList pending;
    bool openScheduled = false;
    bool event(QEvent *event) override {
        if (event->type() == QEvent::FileOpen) {
            const auto *open = static_cast<QFileOpenEvent *>(event);
            if (open->url().scheme().compare(QStringLiteral("ikview"), Qt::CaseInsensitive) == 0) {
                QString error;
                const auto paths = ik::pathsFromFinderHandoff(open->url(), &error);
                if (!error.isEmpty()) { qWarning("Rejected invalid IKView Finder handoff"); return true; }
                pending.append(paths);
            } else if (!open->file().isEmpty()) pending.append(open->file());
            else return QApplication::event(event);
            if (openFiles && !openScheduled) {
                openScheduled = true;
                QTimer::singleShot(40, this, [this] {
                    openScheduled = false;
                    const QStringList files = std::exchange(pending, {});
                    if (!files.isEmpty() && openFiles) openFiles(files);
                });
            }
            return true;
        }
        return QApplication::event(event);
    }
};

static QImage appIcon() {
    QImage image(1024,1024,QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient bg(0,0,1024,1024); bg.setColorAt(0,QColor("#202d35")); bg.setColorAt(1,QColor("#0b1219"));
    p.setBrush(bg); p.setPen(Qt::NoPen); p.drawRoundedRect(QRectF(24,24,976,976),220,220);
    p.setPen(QPen(QColor("#70e4c0"),48,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    p.drawLine(QPointF(290,290),QPointF(290,732));
    p.drawLine(QPointF(460,290),QPointF(460,732));
    p.drawLine(QPointF(730,300),QPointF(492,510));
    p.drawLine(QPointF(502,510),QPointF(754,732));
    p.setPen(Qt::NoPen); p.setBrush(QColor("#c9fff0")); p.drawEllipse(QPointF(290,192),25,25);
    return image;
}

static void makeDemo(const QString &directory) {
    QDir().mkpath(directory);
    const QStringList names={"01-alpine-dusk.png","02-desert-light.jpg","03-ocean-lines.png"};
    for(int n=0;n<3;++n) {
        QImage image(2560,1600,QImage::Format_RGB32);
        QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0,0,0,1600);
        sky.setColorAt(0,QColor(n==1?"#4b536e":"#102c47"));
        sky.setColorAt(0.7,QColor(n==1?"#efb18f":"#7bb6b0"));
        sky.setColorAt(1,QColor("#e4dac1")); p.fillRect(image.rect(),sky);
        p.setPen(Qt::NoPen);p.setBrush(QColor(n==1?"#ffe6bb":"#e6eee0"));p.drawEllipse(QPointF(1810,420),142,142);
        for(int layer=0;layer<5;++layer) {
            const QColor colors[]={QColor("#759999"),QColor("#496f78"),QColor("#2b505e"),QColor("#193847"),QColor("#102530")};
            QPainterPath path; path.moveTo(0,1600);path.lineTo(0,750+layer*130);
            for(int x=0;x<=2700;x+=180) {
                double y=750+layer*140+std::sin(x*0.003+layer*1.7+n)*150+std::cos(x*0.006+layer)*75;
                if(n==1) y=800+layer*130+std::sin(x*0.0018+layer)*170;
                if(n==2) y=850+layer*135+std::sin(x*0.0028+layer*0.7)*65;
                path.lineTo(x,y);
            }
            path.lineTo(2560,1600);path.closeSubpath();
            p.fillPath(path,n==1?QColor::fromHslF(0.065,0.28,0.60-layer*0.085):colors[layer]);
        }
        p.setPen(QColor(240,251,247,210));
        QFont font=p.font(); font.setPixelSize(28);font.setLetterSpacing(QFont::AbsoluteSpacing,7);p.setFont(font);
        p.drawText(100,1460,QString("IKVIEW   /   STUDY %1").arg(n+1,2,10,QChar('0')));
        p.end(); image.save(QDir(directory).filePath(names[n].toLower().endsWith("jpg")?names[n]:names[n]),nullptr,95);
    }
}

int main(int argc,char **argv) {
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    Application app(argc,argv);
    app.setQuitOnLastWindowClosed(true);
    QCoreApplication::setOrganizationName("ikview");
    QCoreApplication::setOrganizationDomain("ikview.io");
    QCoreApplication::setApplicationName("ikview");
    QCoreApplication::setApplicationVersion("0.7.2");
    QCommandLineParser parser; parser.setApplicationDescription("ikview — a native, Metal-accelerated image and PDF viewer");
    parser.addHelpOption();parser.addVersionOption();parser.addPositionalArgument("files","Images, PDFs, folders or archives; extensions for --default-status/--set-default.","[files...]");
    parser.addOption({"generate-demo","Generate demo images and exit.","directory"});
    parser.addOption({"screenshot","Save a window screenshot after rendering.","path"});
    parser.addOption({"inspect","Write runtime, decoder and document state as JSON for diagnostics.","path"});
    parser.addOption({"capture-after","Delay in milliseconds before screenshot/diagnostic capture.","ms","2500"});
    parser.addOption({"image-number","Open the given image number in a collection (1-based).","n"});
    parser.addOption({"page-number","Open the given document page (1-based).","n"});
    parser.addOption({"quit-after","Exit after the given number of milliseconds (for smoke testing).","ms"});
    parser.addOption({"register","Register this app and its document types with macOS."});
    parser.addOption({"default-status","Show default apps for positional extensions (for example: jpg,png)."});
    parser.addOption({"set-default","Use this app for the explicitly listed extensions; save previous apps."});
    parser.addOption({"restore-defaults","Restore saved previous default apps where IKView is still the default."});
    parser.addOption({"defaults-backup","Path to the default-app backup JSON.","path"});
    parser.process(app);
    QString registrationCommand;
    for (const auto &command : {QStringLiteral("register"),QStringLiteral("default-status"),QStringLiteral("set-default"),QStringLiteral("restore-defaults")}) {
        if (!parser.isSet(command)) continue;
        if (!registrationCommand.isEmpty()) { QTextStream(stderr)<<"Choose one registration command.\n"; return 2; }
        registrationCommand=command;
    }
    if (!registrationCommand.isEmpty()) {
        for (const auto &option : {"generate-demo","screenshot","inspect","capture-after","image-number","page-number","quit-after"})
            if (parser.isSet(option)) { QTextStream(stderr)<<"Registration commands cannot be combined with viewer options.\n"; return 2; }
        QStringList extensions;
        for (const auto &argument : parser.positionalArguments()) extensions.append(argument.split(',',Qt::SkipEmptyParts));
        if ((registrationCommand=="register" || registrationCommand=="restore-defaults") && !extensions.isEmpty()) {
            QTextStream(stderr)<<"This command does not accept files or extensions.\n"; return 2;
        }
#ifdef Q_OS_MACOS
        const QString backup=parser.isSet("defaults-backup") ? parser.value("defaults-backup")
            : QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("default-apps-backup.json");
        return ik::runMacRegistrationCommand(registrationCommand,extensions,backup);
#else
        QTextStream(stderr)<<"Desktop registration commands require macOS.\n"; return 2;
#endif
    }
    if (parser.isSet("defaults-backup")) { QTextStream(stderr)<<"--defaults-backup requires a default-app command.\n"; return 2; }
    if(parser.isSet("generate-demo")) { makeDemo(parser.value("generate-demo"));return 0; }
    QImageReader::setAllocationLimit(512);
    app.setWindowIcon(QIcon(QPixmap::fromImage(appIcon())));
    app.setStyle(ik::createViewerStyle());
    QFont font=app.font(); font.setPixelSize(13);app.setFont(font);
    bool imageNumberValid = true, pageNumberValid = true;
    const int imageNumber = parser.isSet("image-number") ? parser.value("image-number").toInt(&imageNumberValid) : 1;
    const int pageNumber = parser.isSet("page-number") ? parser.value("page-number").toInt(&pageNumberValid) : 1;
    if (!imageNumberValid || !pageNumberValid || imageNumber < 1 || pageNumber < 1) {
        QTextStream(stderr) << "Image and page numbers must be positive integers.\n"; return 2;
    }
    QStringList openErrors;
    QMap<QString, QSet<QString>> observedColors;
    QMap<QString, int> observedFrames;
    ik::MainWindow window;
    app.openFiles=[&window](const QStringList &paths){
        window.openPaths(paths);
        if (window.isMinimized()) window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.show(); window.raise(); window.activateWindow();
    };
#ifdef Q_OS_MACOS
    ik::installMacFinderServices([&app](const QStringList &paths){if(app.openFiles)app.openFiles(paths);},
        [&window](const QString &message){QMessageBox::warning(&window,"IKView",message);});
    QObject::connect(&app,&QApplication::aboutToQuit,&app,[]{ik::removeMacFinderServices();});
#endif
    window.show();
    QStringList paths=parser.positionalArguments();paths.append(std::exchange(app.pending, {}));
    auto *document = window.findChild<ik::Document*>();
    if (document) QObject::connect(document, &ik::Document::errorOccurred, &window, [&openErrors](const QString &message) { openErrors.append(message); });
    if (document && parser.isSet("image-number")) {
        QObject::connect(document, &ik::Document::entriesChanged, &window, [document,imageNumber,&app] {
            if (document->count() > 0 && imageNumber > document->count()) { QTextStream(stderr) << "Image number is outside the collection.\n"; app.exit(2); }
        });
    }
    if (document && parser.isSet("page-number")) {
        QObject::connect(document, &ik::Document::imageReady, &window, [document,imageNumber,pageNumber,&app](const QImage &,const ik::ImageInfo &info) {
            if (document->currentIndex() == imageNumber - 1 && pageNumber > info.pageCount) { QTextStream(stderr) << "Page number is outside the document.\n"; app.exit(2); }
        });
    }
    if (document && parser.isSet("inspect")) QObject::connect(document, &ik::Document::imageReady, &window, [document,&observedColors,&observedFrames](const QImage &image,const ik::ImageInfo &info) {
        if (!info.animated || image.isNull()) return;
        const auto key = document->currentEntry().key();
        if (observedColors[key].size() < 64) observedColors[key].insert(image.pixelColor(image.width()/2,image.height()/2).name(QColor::HexArgb));
        ++observedFrames[key];
    });
    if (!paths.isEmpty()) {
        if (parser.isSet("image-number") || parser.isSet("page-number")) window.openPathsAt(paths,imageNumber-1,pageNumber-1);
        else window.openPaths(paths);
    }
    if(parser.isSet("screenshot") || parser.isSet("inspect")) QTimer::singleShot(qMax(0,parser.value("capture-after").toInt()),&window,[&window,&parser,&app,&openErrors,&observedColors,&observedFrames,document]{
        auto *canvas = window.findChild<ik::ImageCanvas*>();
        const QImage frame = canvas ? canvas->viewportImage() : QImage();
        if (parser.isSet("screenshot")) {
            QPixmap screenshot=window.grab();
            if (canvas) {
                QPainter painter(&screenshot);
                painter.drawImage(QRect(canvas->mapTo(&window,QPoint()),canvas->size()),frame);
            }
            if (!screenshot.save(parser.value("screenshot"))) { QTextStream(stderr) << "Screenshot could not be saved.\n"; app.exit(2); }
        }
        if (parser.isSet("inspect")) {
            auto formats = [](const QList<QByteArray> &values) { QJsonArray result; for (const auto &value : values) result.append(QString::fromLatin1(value)); return result; };
            QJsonObject result {{"version", QCoreApplication::applicationVersion()}, {"qt_version", QT_VERSION_STR},
                {"renderer", canvas ? canvas->rendererDescription() : QString()},
                {"read_formats", formats(QImageReader::supportedImageFormats())}, {"write_formats", formats(QImageWriter::supportedImageFormats())},
                {"errors", QJsonArray::fromStringList(openErrors)}};
            result["frame_ready"] = !frame.isNull();
            if (!frame.isNull()) result["frame_center_pixel"] = frame.pixelColor(frame.width()/2,frame.height()/2).name(QColor::HexArgb);
            if (document) {
                const auto info = document->currentInfo(); const auto entry = document->currentEntry(); const auto pixels = document->currentImage();
                QJsonObject state {{"count", document->count()}, {"index", document->currentIndex()}, {"name", entry.name},
                    {"key", entry.key()}, {"source_path", entry.isArchive() ? entry.archivePath : entry.path}, {"busy", document->isBusy()}, {"image_ready", !pixels.isNull()},
                    {"width", pixels.width()}, {"height", pixels.height()}, {"page", info.page}, {"page_count", info.pageCount}, {"format", info.format},
                    {"pdf", info.pdf}, {"animated", info.animated}, {"observed_frame_count", observedFrames.value(entry.key())}};
                const auto colorSet = observedColors.value(entry.key());
                QStringList colors(colorSet.begin(),colorSet.end());
                colors.sort(); state["observed_center_colors"] = QJsonArray::fromStringList(colors);
                if (!pixels.isNull()) {
                    state["center_pixel"] = pixels.pixelColor(pixels.width()/2,pixels.height()/2).name(QColor::HexArgb);
                    QJsonArray corners;
                    for (const QPoint point : {QPoint(pixels.width()/8,pixels.height()/8),QPoint(pixels.width()*7/8,pixels.height()/8),QPoint(pixels.width()/8,pixels.height()*7/8),QPoint(pixels.width()*7/8,pixels.height()*7/8)}) corners.append(pixels.pixelColor(point).name(QColor::HexArgb));
                    state["corner_pixels"] = corners;
                }
                result["document"] = state;
            }
            if (auto *list = window.findChild<QListView*>(QStringLiteral("fileList"))) {
                QJsonArray selected; for (const auto &index : list->selectionModel()->selectedRows()) selected.append(index.data(Qt::UserRole).toInt());
                result["selection"] = selected; result["visible_rows"] = list->model()->rowCount();
            }
            if (canvas) { result["zoom"] = canvas->zoom(); result["manual_zoom"] = canvas->isManualZoom(); }
            QSaveFile output(parser.value("inspect"));
            const auto bytes = QJsonDocument(result).toJson();
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                QTextStream(stderr) << "Diagnostic JSON could not be saved.\n"; app.exit(2);
            }
        }
    });
    if(parser.isSet("quit-after")) QTimer::singleShot(qMax(100,parser.value("quit-after").toInt()),&app,&QApplication::quit);
    return app.exec();
}
