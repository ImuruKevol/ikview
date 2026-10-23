#include "document.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

int main(int argc,char **argv) {
    QApplication app(argc,argv);
    if(argc!=2){QTextStream(stderr)<<"Usage: ikview_benchmark <image-folder-or-archive>\n";return 2;}
    ik::Document document;
    QElapsedTimer initial;initial.start();
    QElapsedTimer transition;
    QJsonArray samples;
    QList<double> times;
    bool first=true;
    int target=0;
    int pass=0;
    double firstMs=0;
    QObject::connect(&document,&ik::Document::errorOccurred,&app,[&](const QString &message){QTextStream(stderr)<<message<<'\n';app.exit(1);});
    QObject::connect(&document,&ik::Document::imageReady,&app,[&](const QImage &image,const ik::ImageInfo &info){
        if(first){first=false;firstMs=initial.nsecsElapsed()/1e6;}
        else {
            double ms=transition.nsecsElapsed()/1e6;
            times.append(ms);
            samples.append(QJsonObject{{"pass",pass},{"index",target},{"ready_ms",ms},{"decode_ms",info.decodeMs},{"width",image.width()},{"height",image.height()}});
        }
        QTimer::singleShot(5,&document,[&]{
            ++target;
            if(target>=document.count()){target=0;++pass;}
            if(pass==3 || document.count()<2){
                std::sort(times.begin(),times.end());
                QJsonObject result{{"qt_version",QT_VERSION_STR},{"images",document.count()},{"first_ready_ms",firstMs},{"cache_limit_mib",256},{"samples",samples}};
                if(!times.isEmpty()){
                    result["median_switch_ms"]=times[times.size()/2];
                    result["p95_switch_ms"]=times[qMin(times.size()-1,int(times.size()*0.95))];
                }
                QTextStream(stdout)<<QJsonDocument(result).toJson(QJsonDocument::Indented);
                app.quit();return;
            }
            transition.restart();document.goTo(target);
        });
    });
    QTimer::singleShot(60000,&app,[&]{QTextStream(stderr)<<"Benchmark timeout\n";app.exit(1);});
    document.open(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
