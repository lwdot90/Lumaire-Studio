#include "io/image_import.h"
#include "io/project_store.h"
#include "rendering/cpu_image.h"
#include <QCoreApplication>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <iostream>

using namespace compositor;
namespace {
void expect(bool okay,const char* message) { if(!okay) throw std::runtime_error(message); }
template<class F> void rejects(F action) { bool rejected=false; try { action(); } catch(const std::exception&) { rejected=true; } expect(rejected,"Expected malformed image rejection"); }
QByteArray read(const QString& path) { QFile f(path); expect(f.open(QIODevice::ReadOnly),"Read fixture"); return f.readAll(); }
void write(const QString& path,const QByteArray& bytes) { QFile f(path); expect(f.open(QIODevice::WriteOnly),"Write fixture"); expect(f.write(bytes)==bytes.size(),"Fixture write complete"); }
void append32(QByteArray& bytes,std::uint32_t v) { for(int shift:{24,16,8,0}) bytes+=static_cast<char>((v>>shift)&255); }
QByteArray chunk(const QByteArray& type,const QByteArray& data) {
    QByteArray result; append32(result,static_cast<std::uint32_t>(data.size())); result+=type; result+=data;
    append32(result,static_cast<std::uint32_t>(crc32(0,reinterpret_cast<const Bytef*>(result.constData()+4),static_cast<uInt>(result.size()-4)))); return result;
}
QByteArray orientedBytes(const QByteArray& encoded,int orientation,bool png) {
    auto exif=QByteArray::fromHex("49492a0008000000010012010300010000000100000000000000"); exif[18]=static_cast<char>(orientation);
    auto result=encoded;
    if(png) result.insert(33,chunk("eXIf",exif));
    else {
        const auto data=QByteArray("Exif\0\0",6)+exif;
        QByteArray marker=QByteArray::fromHex("ffe1"); const int length=static_cast<int>(data.size())+2;
        marker+=static_cast<char>(length>>8); marker+=static_cast<char>(length&255); marker+=data; result.insert(2,marker);
    }
    return result;
}
void tiledImportMatchesFullConversion(const QString& path,const QImage& source,const char* format,engine::TileStore& tiles) {
    expect(source.save(path,format,95),"Save multi-tile import fixture"); const auto encoded=read(path);
    QImage decoded;
    { QImageReader reader(path); reader.setAutoTransform(false); decoded=reader.read().convertToFormat(QImage::Format_RGBA64); }
    expect(!decoded.isNull(),"Full-conversion import oracle");
    for(int orientation=1;orientation<=8;++orientation) {
        write(path,orientedBytes(encoded,orientation,QByteArray(format)=="PNG"));
        const auto document=io::importImage(path,tiles).document;
        const int width=decoded.width(),height=decoded.height();
        expect(document->width==(orientation>=5 ? height : width) && document->height==(orientation>=5 ? width : height),"Multi-tile oriented dimensions");
        const auto& raster=*document->singleLayer().raster;
        expect(raster.tiles.size()==4,"Multi-tile fixture retains four full/partial tiles");
        // Forward source-to-destination mapping is independent of the importer's
        // destination-to-source lookup and tile rectangle construction.
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            int dx=x,dy=y;
            switch(orientation) {
                case 2:dx=width-1-x;break; case 3:dx=width-1-x;dy=height-1-y;break; case 4:dy=height-1-y;break;
                case 5:dx=y;dy=x;break; case 6:dx=height-1-y;dy=x;break;
                case 7:dx=height-1-y;dy=width-1-x;break; case 8:dx=y;dy=width-1-x;break; default:break;
            }
            const auto p=reinterpret_cast<const QRgba64*>(decoded.constScanLine(y))[x];
            const auto expected=engine::unpack(engine::pack(engine::fromStraightSrgb({p.red()/65535.f,p.green()/65535.f,p.blue()/65535.f,p.alpha()/65535.f})));
            expect(raster.pixel(dx,dy)==expected,"Tile conversion preserves full-decoded canonical pixels at every orientation/boundary");
        }
    }
}
void admissionLedger(const MemoryAdmission& admission,std::uint64_t liveTiles=0) {
    const auto memory=admission.snapshot();
    expect(memory.pendingCpu==0 && memory.pendingGpu==0 && memory.committedGpu==0 && memory.committedCpu==liveTiles,
        "Import left pending/transient/encoded/decode charges after releasing their backing");
}
void importAdmission(const QString& path,const QByteArray& encoded,const engine::DocumentPtr& reference) {
    constexpr std::uint64_t transient=64u*1024*1024;
    const auto inputBytes=static_cast<std::uint64_t>(encoded.size())+1;
    const auto decodedBytes=static_cast<std::uint64_t>(reference->width*reference->height)*8;
    for(const bool denyDecode:{false,true}) {
        unsigned probes=0;
        const auto limit=transient+inputBytes+(denyDecode ? decodedBytes : 0)-1;
        auto memory=std::make_shared<MemoryAdmission>(limit,0,[&] {++probes;return MemorySample{256u*1024*1024,0};});
        engine::TileStore bounded(1024*1024,memory); write(path,encoded);
        engine::DocumentPtr published; bool denied=false;
        try {published=io::importImage(path,bounded).document;} catch(const std::length_error&) {denied=true;}
        expect(denied && !published && bounded.usedBytes()==0,"Denied encoded/decode allocation published an image or retained tiles");
        expect(probes==(denyDecode ? 3u : 2u),"Import denial happened after its intended allocation phase");
        admissionLedger(*memory);
    }
    {
        const auto tileBytes=static_cast<std::uint64_t>(reference->singleLayer().raster->tiles.begin()->second->retainedBytes());
        // The encoded backing must retire before this tile is admitted. This
        // budget fits either decode phase or tile phase, but not both backings.
        auto memory=std::make_shared<MemoryAdmission>(transient+decodedBytes+std::max(inputBytes,tileBytes),0,
            [] {return MemorySample{256u*1024*1024,0};});
        engine::TileStore bounded(1024*1024,memory); write(path,encoded);
        {
            const auto output=io::importImage(path,bounded).document;
            expect(output->width==reference->width && output->height==reference->height && output->resolution==reference->resolution,
                "Admitted import changed dimensions or resolution");
            for(int y=0;y<reference->height;++y) for(int x=0;x<reference->width;++x)
                expect(output->singleLayer().raster->pixel(x,y)==reference->singleLayer().raster->pixel(x,y),"Admitted import changed canonical pixels");
            expect(bounded.usedBytes()==tileBytes,"Admitted fixture retained unexpected tile storage");
            admissionLedger(*memory,tileBytes);
        }
        expect(bounded.usedBytes()==0,"Released admitted image retained tile backing"); admissionLedger(*memory);
    }
    for(const bool grow:{false,true}) {
        write(path,encoded); unsigned probes=0;
        auto memory=std::make_shared<MemoryAdmission>(128u*1024*1024,0,[&] {
            if(++probes==2) write(path,grow ? encoded+QByteArray("x") : encoded.left(encoded.size()/2));
            return MemorySample{256u*1024*1024,0};
        });
        engine::TileStore bounded(1024*1024,memory);
        rejects([&]{io::importImage(path,bounded);});
        expect(probes==2 && bounded.usedBytes()==0,"Growing/truncated input reached decode or retained tiles"); admissionLedger(*memory);
    }
    {
        write(path,encoded.left(33)+chunk("IDAT",QByteArray("broken compressed pixels"))+chunk("IEND",{}));
        unsigned probes=0;
        auto memory=std::make_shared<MemoryAdmission>(128u*1024*1024,0,[&] {++probes;return MemorySample{256u*1024*1024,0};});
        engine::TileStore bounded(1024*1024,memory);
        rejects([&]{io::importImage(path,bounded);});
        expect(probes==3 && bounded.usedBytes()==0,"Damaged decoder input bypassed admission or retained tiles"); admissionLedger(*memory);
    }
    {
        write(path,encoded); unsigned probes=0;
        auto memory=std::make_shared<MemoryAdmission>(128u*1024*1024,0,[&] {++probes;return MemorySample{256u*1024*1024,0};});
        engine::TileStore bounded(1,memory);
        rejects([&]{io::importImage(path,bounded);});
        expect(probes==4 && bounded.usedBytes()==0,"Failed tile allocation retained imported backing"); admissionLedger(*memory);
    }
    {
        write(path,encoded); unsigned probes=0; std::stop_source stop;
        auto memory=std::make_shared<MemoryAdmission>(128u*1024*1024,0,[&] {
            if(++probes==3) stop.request_stop();
            return MemorySample{256u*1024*1024,0};
        });
        engine::TileStore bounded(1024*1024,memory);
        rejects([&]{io::importImage(path,bounded,stop.get_token());});
        expect(probes==3 && bounded.usedBytes()==0,"Cancellation after decode reservation retained tiles"); admissionLedger(*memory);
    }
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        QTemporaryDir dir; expect(dir.isValid(),"Temporary directory"); engine::TileStore tiles(32*1024*1024);
        const auto path=dir.filePath("input.png");
        QImage image(2,3,QImage::Format_RGBA64);
        const std::array<QRgba64,6> values{QRgba64::fromRgba64(65535,0,0,65535),QRgba64::fromRgba64(0,65535,0,32768),
            QRgba64::fromRgba64(0,0,65535,65535),QRgba64::fromRgba64(32768,12345,54321,65535),
            QRgba64::fromRgba64(65535,65535,65535,0),QRgba64::fromRgba64(65535,65535,0,65535)};
        for(int y=0;y<3;++y) for(int x=0;x<2;++x) reinterpret_cast<QRgba64*>(image.scanLine(y))[x]=values[static_cast<std::size_t>(y*2+x)];
        image.setDotsPerMeterX(11811); image.setDotsPerMeterY(11811); expect(image.save(path,"PNG"),"Save 16-bit PNG fixture");
        const auto imported=io::importImage(path,tiles); expect(imported.document->width==2 && imported.document->height==3,"Import dimensions");
        expect(imported.information.contains("assumed sRGB") && std::abs(imported.document->resolution-300)<.01,"Untagged warning and resolution");
        for(int y=0;y<3;++y) for(int x=0;x<2;++x) {
            const auto p=values[static_cast<std::size_t>(y*2+x)];
            const auto expected=engine::unpack(engine::pack(engine::fromStraightSrgb({p.red()/65535.f,p.green()/65535.f,p.blue()/65535.f,p.alpha()/65535.f})));
            expect(imported.document->singleLayer().raster->pixel(x,y)==expected,"16-bit source conversion, alpha and zero-alpha canonicalization");
        }
        const auto encoded=read(path);
        importAdmission(dir.filePath("admission.png"),encoded,imported.document);
        for(int orientation=1;orientation<=8;++orientation) {
            auto exif=QByteArray::fromHex("49492a0008000000010012010300010000000100000000000000"); exif[18]=static_cast<char>(orientation);
            auto oriented=encoded; oriented.insert(33,chunk("eXIf",exif)); const auto orientedPath=dir.filePath("oriented.png"); write(orientedPath,oriented);
            const auto output=io::importImage(orientedPath,tiles).document;
            expect(output->width==(orientation>=5 ? 3 : 2) && output->height==(orientation>=5 ? 2 : 3),"Oriented dimensions");
            for(int y=0;y<3;++y) for(int x=0;x<2;++x) {
                int dx=x,dy=y;
                switch(orientation) { case 2:dx=1-x;break; case 3:dx=1-x;dy=2-y;break; case 4:dy=2-y;break;
                    case 5:dx=y;dy=x;break; case 6:dx=2-y;dy=x;break; case 7:dx=2-y;dy=1-x;break; case 8:dx=y;dy=1-x;break; default:break; }
                expect(output->singleLayer().raster->pixel(dx,dy)==imported.document->singleLayer().raster->pixel(x,y),"EXIF orientation applied exactly once");
            }
        }
        QImage tagged(1,1,QImage::Format_RGBA64); tagged.fill(QColor::fromRgbF(.5,.5,.5)); tagged.setColorSpace(QColorSpace::SRgbLinear);
        const auto taggedPath=dir.filePath("linear.png"); expect(tagged.save(taggedPath,"PNG"),"ICC fixture");
        const auto withProfile=io::importImage(taggedPath,tiles).document;
        expect(std::abs(withProfile->singleLayer().raster->pixel(0,0).r-.5f)<.002f && withProfile->singleLayer().raster->sourceProfile,"Linear ICC honored");
        const auto projectPath=dir.filePath("profile.cproj"); io::saveProject(projectPath,withProfile);
        const auto reopened=io::loadProject(projectPath,tiles).document;
        expect(reopened->singleLayer().raster->sourceProfile->bytes==withProfile->singleLayer().raster->sourceProfile->bytes && reopened->singleLayer().raster->sourceProfile->id==withProfile->singleLayer().raster->sourceProfile->id,"Source profile bytes and ID persist");
        const auto jpegPath=dir.filePath("input.jpg"); expect(image.save(jpegPath,"JPEG",95),"JPEG fixture");
        expect(io::importImage(jpegPath,tiles).document->width==2,"JPEG decode");
        QImage tiled16(259,263,QImage::Format_RGBA64);
        const std::array<std::uint16_t,6> alpha{0,1,257,32768,65534,65535};
        for(int y=0;y<tiled16.height();++y) for(int x=0;x<tiled16.width();++x) {
            reinterpret_cast<QRgba64*>(tiled16.scanLine(y))[x]=QRgba64::fromRgba64(
                static_cast<std::uint16_t>(x*251+y*83+31),static_cast<std::uint16_t>(x*17+y*499+211),
                static_cast<std::uint16_t>(x*887+y*13+419),alpha[static_cast<std::size_t>((x+y)%6)]);
        }
        tiledImportMatchesFullConversion(dir.filePath("tiled16.png"),tiled16,"PNG",tiles);
        tiledImportMatchesFullConversion(dir.filePath("tiled8.png"),tiled16.convertToFormat(QImage::Format_RGBA8888),"PNG",tiles);
        tiledImportMatchesFullConversion(dir.filePath("tiled16-rgb.png"),tiled16.convertToFormat(QImage::Format_RGBX64),"PNG",tiles);
        tiledImportMatchesFullConversion(dir.filePath("tiled.jpg"),tiled16.convertToFormat(QImage::Format_RGB888),"JPEG",tiles);
        auto damaged=encoded; damaged[damaged.size()/2]^=1; const auto badPath=dir.filePath("bad.png"); write(badPath,damaged);
        rejects([&]{io::importImage(badPath,tiles);}); write(badPath,encoded.left(encoded.size()-10)); rejects([&]{io::importImage(badPath,tiles);});
        QByteArray compressed(128,'\0'); uLongf length=static_cast<uLongf>(compressed.size());
        const QByteArray invalidProfile("not an ICC profile");
        expect(compress2(reinterpret_cast<Bytef*>(compressed.data()),&length,reinterpret_cast<const Bytef*>(invalidProfile.constData()),static_cast<uLong>(invalidProfile.size()),1)==Z_OK,"Compress malformed profile fixture");
        compressed.resize(static_cast<qsizetype>(length));
        auto badProfile=encoded; badProfile.insert(33,chunk("iCCP",QByteArray("profile\0\0",9)+compressed)); write(badPath,badProfile);
        rejects([&]{io::importImage(badPath,tiles);});
        auto oversized=encoded; auto header=encoded.mid(16,13); QByteArray dimension; append32(dimension,30001); header.replace(0,4,dimension);
        oversized.replace(8,25,chunk("IHDR",header)); write(badPath,oversized); rejects([&]{io::importImage(badPath,tiles);});
        const auto jpeg=read(jpegPath); const QByteArray partialProfile("ICC_PROFILE\0\1\2broken",20);
        QByteArray marker=QByteArray::fromHex("ffe2"); const int segmentLength=static_cast<int>(partialProfile.size())+2;
        marker+=static_cast<char>(segmentLength>>8); marker+=static_cast<char>(segmentLength&255); marker+=partialProfile;
        auto incomplete=jpeg; incomplete.insert(2,marker); write(badPath,incomplete); rejects([&]{io::importImage(badPath,tiles);});
        std::stop_source stop; stop.request_stop(); rejects([&]{io::importImage(path,tiles,stop.get_token());});
        auto raster=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),engine::Extent{0,0,1,1},engine::pack({.5,0,0,.5}));
        engine::DocumentSnapshot document(engine::Id::generate(),engine::Id::generate(),1,1,72,raster);
        Viewport view{1,1,1,1,{.5,.5},false,1,1}; const auto frame=renderCpuImage(document,view,1,1);
        const float bg=engine::decodeSrgb(.85f); const int red=qRed(frame.pixel(0,0)),green=qGreen(frame.pixel(0,0));
        expect(std::abs(static_cast<float>(red)-engine::encodeSrgb(.5f+bg*.5f)*255)<2 && std::abs(static_cast<float>(green)-engine::encodeSrgb(bg*.5f)*255)<2,"Canvas checker composites in linear light before display encoding");
        expect(renderCpuImage(document,view,1,1,[]{return true;}).isNull(),"Obsolete render cancellation");
        auto bottom=document.singleLayer(); bottom.raster=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),engine::Extent{0,0,1,1},engine::pack({0,0,1,1}));
        auto top=document.singleLayer(); top.siblingOrder=1; top.opacity=.5f; bottom.id=engine::Id::generate();
        engine::DocumentSnapshot layered(engine::Id::generate(),1,1,72,std::vector<engine::LayerNode>{top,bottom});
        const auto layeredFrame=renderCpuImage(layered,view,1,1);
        expect(std::abs(static_cast<float>(qRed(layeredFrame.pixel(0,0)))-engine::encodeSrgb(.25f)*255)<2
            && std::abs(static_cast<float>(qBlue(layeredFrame.pixel(0,0)))-engine::encodeSrgb(.75f)*255)<2,"Production preview composites complete layer stack with opacity once");
        Viewport fit; fit.documentWidth=200;fit.documentHeight=100;fit.resize(800,600,2);expect(fit.center.x==100 && fit.center.y==50,"Fit uses imported dimensions");
        std::cout<<"PNG8/16/JPEG, bounded import admission, full-conversion tile boundaries, ICC, eight orientations, alpha, resolution and CPU presentation passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
