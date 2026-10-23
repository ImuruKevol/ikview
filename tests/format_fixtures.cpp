#include <QCoreApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QTextStream>
#include <archive.h>
#include <archive_entry.h>

static bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
static QByteArray png(const QColor &color) {
    QImage image(96,64,QImage::Format_RGBA8888); image.fill(color);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); return bytes;
}
static bool archiveFile(const QString &path, int (*format)(archive *), bool duplicates, const QByteArray &secondPageDocument = {}, const char *documentName = "pages.tiff") {
    archive *writer=archive_write_new(); format(writer);
    if(archive_write_open_filename(writer,QFile::encodeName(path).constData())!=ARCHIVE_OK) {archive_write_free(writer); return false;}
    bool success=true;
    for(int i=0;i<2;++i) {
        const QByteArray bytes=i==1 && !secondPageDocument.isEmpty() ? secondPageDocument : png(i==0?QColor(Qt::red):QColor(Qt::blue));
        auto *entry=archive_entry_new();
        archive_entry_set_pathname(entry,duplicates?"same.png":(i==0?"1.png":(secondPageDocument.isEmpty()?"2.png":documentName)));
        archive_entry_set_size(entry,bytes.size()); archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0644); archive_entry_set_mtime(entry,2000-i*1000,0);
        success=archive_write_header(writer,entry)==ARCHIVE_OK && archive_write_data(writer,bytes.constData(),bytes.size())==bytes.size() && success;
        archive_entry_free(entry);
    }
    success=archive_write_close(writer)==ARCHIVE_OK && success; archive_write_free(writer); return success;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(argc!=2) return 2;
    const QDir directory(QString::fromLocal8Bit(argv[1])); if(!QDir().mkpath(directory.path())) return 2;
    QJsonArray cases;
    auto add=[&](const QString &file,int count,int width,int height,const QString &color,int image=1,int page=1,int pages=1,int tolerance=0) {
        cases.append(QJsonObject{{"file",file},{"count",count},{"width",width},{"height",height},{"center",color},{"image_number",image},{"page_number",page},{"page_count",pages},{"tolerance",tolerance},{"animated",file=="animation.gif"}});
    };
    QImage image(96,64,QImage::Format_RGB32); image.fill(QColor("#e04060"));
    for(const auto &format:{QByteArray("png"),QByteArray("jpeg"),QByteArray("bmp"),QByteArray("webp"),QByteArray("tiff")}) {
        const QString name="color."+QString::fromLatin1(format);
        if(!image.save(directory.filePath(name),format.constData(),95)) {QTextStream(stderr)<<"Missing fixture encoder: "<<format<<'\n';return 2;}
        add(name,1,96,64,"#ffe04060",1,1,1,format=="jpeg"||format=="webp"?12:0);
    }
#ifdef Q_OS_MACOS
    QProcess converter;
    converter.start(QStringLiteral("/usr/bin/sips"), {QStringLiteral("--setProperty"),QStringLiteral("format"),QStringLiteral("heic"),directory.filePath("color.png"),QStringLiteral("--out"),directory.filePath("color.heic")});
    if(!converter.waitForFinished(30000) || converter.exitStatus()!=QProcess::NormalExit || converter.exitCode()!=0) {
        QTextStream(stderr)<<"Could not generate native HEIC fixture: "<<converter.readAllStandardError()<<'\n';return 2;
    }
    add("color.heic",1,96,64,"#ffe04060",1,1,1,12);
#endif
    if(!image.save(directory.filePath(QString::fromUtf8("한글 사진.png")),"PNG")) return 2;
    add(QString::fromUtf8("한글 사진.png"),1,96,64,"#ffe04060");
    if(!writeFile(directory.filePath("unknown.data"),png(Qt::red))) return 2;
    add("unknown.data",1,96,64,"#ffff0000");
    QImage alpha(96,64,QImage::Format_RGBA8888);alpha.fill(QColor(80,120,200,128));
    if(!alpha.save(directory.filePath("alpha.png"))) return 2;
    add("alpha.png",1,96,64,"#805078c8",1,1,1,1);
    const QByteArray svg="<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" height=\"64\"><rect width=\"96\" height=\"64\" fill=\"#60d9b1\"/></svg>";
    if(!writeFile(directory.filePath("vector.svg"),svg)) return 2;
    add("vector.svg",1,96,64,"#ff60d9b1");
    const QByteArray gif=QByteArray::fromHex("47494638396101000100800000ff00000000ff21ff0b4e45545343415045322e30030100000021f90400050000002c000000000100010000020244010021f90400050000002c00000000010001000002024c01003b");
    if(!writeFile(directory.filePath("animation.gif"),gif)) return 2;
    add("animation.gif",1,1,1,QString());
    QByteArray tiff; QDataStream data(&tiff,QIODevice::WriteOnly);data.setByteOrder(QDataStream::LittleEndian);
    data.writeRawData("II",2);data<<quint16(42)<<quint32(8);
    for(int page=0;page<2;++page) {
        data<<quint16(10);auto tag=[&](quint16 id,quint16 type,quint32 count,quint32 value){data<<id<<type<<count<<value;};
        tag(256,4,1,2);tag(257,4,1,2);tag(258,3,3,260+page*6);tag(259,3,1,1);tag(262,3,1,2);tag(273,4,1,272+page*12);tag(277,3,1,3);tag(278,4,1,2);tag(279,4,1,12);tag(284,3,1,1);data<<quint32(page==0?134:0);
    }
    for(int i=0;i<6;++i)data<<quint16(8);
    for(int page=0;page<2;++page)for(int i=0;i<4;++i)data<<quint8(page==0?255:0)<<quint8(0)<<quint8(page==1?255:0);
    if(!writeFile(directory.filePath("pages.tiff"),tiff))return 2;
    add("pages.tiff",1,2,2,"#ff0000ff",1,2,2);
    QImage orientationInput=image;
    { QPainter painter(&orientationInput); painter.fillRect(0,0,24,16,Qt::red); painter.fillRect(72,0,24,16,Qt::green); painter.fillRect(0,48,24,16,Qt::blue); painter.fillRect(72,48,24,16,Qt::yellow); }
    QByteArray encoded;QBuffer orientationBuffer(&encoded);orientationBuffer.open(QIODevice::WriteOnly);
    if(!orientationInput.save(&orientationBuffer,"JPEG",100))return 2;
    const auto exif=QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
    QByteArray oriented=encoded.left(2)+QByteArray::fromHex("ffe1");const int length=exif.size()+2;oriented.append(char(length>>8));oriented.append(char(length&255));oriented+=exif;oriented+=encoded.mid(2);
    if(!writeFile(directory.filePath("orientation.jpeg"),oriented))return 2;
    add("orientation.jpeg",1,64,96,"#ffe04060",1,1,1,12);
    { auto record=cases.last().toObject();record["corner_colors"]=QJsonArray{"#ff0000ff","#ffff0000","#ffffff00","#ff00ff00"};cases.replace(cases.size()-1,record); }
    if(!archiveFile(directory.filePath("images.cbz"),archive_write_set_format_zip,false)
       ||!archiveFile(directory.filePath("images.7z"),archive_write_set_format_7zip,false)
       ||!archiveFile(directory.filePath("images.tar"),archive_write_set_format_pax_restricted,false)
       ||!archiveFile(directory.filePath("duplicates.cbz"),archive_write_set_format_zip,true)
       ||!archiveFile(directory.filePath("pages.cbz"),archive_write_set_format_zip,false,tiff))return 2;
    add("images.cbz",2,96,64,"#ffff0000");add("images.7z",2,96,64,"#ffff0000");add("images.tar",2,96,64,"#ffff0000");add("duplicates.cbz",2,96,64,"#ff0000ff",2);
    add("pages.cbz",2,2,2,"#ff0000ff",2,2,2);
#ifdef Q_OS_MACOS
    QByteArray pdfBytes;
    { QBuffer buffer(&pdfBytes); buffer.open(QIODevice::WriteOnly); QPdfWriter writer(&buffer); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(120,80),QPageSize::Point)); writer.setPageMargins(QMarginsF(0,0,0,0)); QPainter painter(&writer); painter.fillRect(QRect(0,0,120,80),Qt::red); if(!writer.newPage())return 2; painter.fillRect(QRect(0,0,120,80),Qt::blue); painter.end(); }
    if(!writeFile(directory.filePath("pages.pdf"),pdfBytes) || !writeFile(directory.filePath("renamed-pdf.data"),pdfBytes) || !archiveFile(directory.filePath("pdf-pages.cbz"),archive_write_set_format_zip,false,pdfBytes,"pages.pdf"))return 2;
    auto addPdf=[&](const QString &name,int count,int image,int page,const QString &color){add(name,count,240,160,color,image,page,2); auto record=cases.last().toObject();record["pdf"]=true;cases.replace(cases.size()-1,record);};
    addPdf("pages.pdf",1,1,2,"#ff0000ff"); addPdf("renamed-pdf.data",1,1,1,"#ffff0000"); addPdf("pdf-pages.cbz",2,2,2,"#ff0000ff");
#endif
    if(!writeFile(directory.filePath("manifest.json"),QJsonDocument(cases).toJson()))return 2;
    QTextStream(stdout)<<cases.size()<<" codec/collection fixtures generated\n"; return 0;
}
