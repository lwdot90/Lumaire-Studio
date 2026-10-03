#include "io/image_export.h"
#include <QCoreApplication>
#include <QImageReader>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <cmath>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
using namespace compositor::io;
namespace {
void expect(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class F> void rejects(F action) {bool rejected=false;try {action();}catch(const std::exception&){rejected=true;}expect(rejected,"Export accepted invalid/failing request");}
QByteArray bytes(const QString& path) {QFile file(path);expect(file.open(QIODevice::ReadOnly),"Cannot read fixture");return file.readAll();}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir directory;expect(directory.isValid(),"Cannot create export fixture");
        auto memory=std::make_shared<MemoryAdmission>(64*1024*1024,0,[]{return MemorySample{1024*1024*1024,0};});
        TileStore tiles(1024*1024,memory);
        const std::array<Pixel,4> values{{fromStraightSrgb({1,0,0,.5f}),fromStraightSrgb({0,1,0,1}),Pixel{},fromStraightSrgb({.25f,.5f,.75f,1})}};
        TileMap tileMap;tileMap.emplace(TileCoord{0,0},tiles.create(2,2,values));
        auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,2,2},PackedPixel{},tileMap);
        auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),2,2,144,raster);
        ExportOptions options;options.memory=memory;options.quality=100;
        const auto png=directory.filePath("alpha.png");exportImage(png,document,options);
        QImageReader pngReader(png);auto decoded=pngReader.read();expect(!decoded.isNull() && decoded.width()==2 && decoded.height()==2,"PNG dimensions/decode failed");
        auto first=decoded.pixelColor(0,0);expect(first.red()==255 && first.green()==0 && first.blue()==0 && first.alpha()==128,"PNG straight alpha/color mismatch");
        expect(decoded.pixelColor(0,1).alpha()==0,"PNG transparent pixel became opaque");
        // Identity bilinear placement must sample each pixel center. Integer
        // document coordinates would blend the image with its transparent edge.
        const auto green=decoded.pixelColor(1,0);
        expect(green.red()==0 && green.green()==255 && green.blue()==0 && green.alpha()==255,"Identity bilinear export shifted/blended the upper-right pixel");
        const auto mixed=decoded.pixelColor(1,1);
        expect(std::abs(mixed.red()-64)<=1 && std::abs(mixed.green()-128)<=1 && std::abs(mixed.blue()-191)<=1 && mixed.alpha()==255,"Identity bilinear export shifted/blended the lower-right pixel");
        expect(std::abs(decoded.dotsPerMeterX()-std::lround(144/0.0254))<=1,"PNG DPI metadata changed");
        // First-row sampling happens before png_write_row allocates filter /
        // compression storage. Deny that codec callback allocation to exercise
        // the actual C error/longjmp path with mutated sampler state.
        bool denyCodec=false,codecProbeFailed=false;
        auto codecMemory=std::make_shared<MemoryAdmission>(64*1024*1024,0,[&] {
            if(denyCodec) {codecProbeFailed=true;return MemorySample{0,0};}
            return MemorySample{1024*1024*1024,0};
        });
        ExportOptions codecFailure;codecFailure.memory=codecMemory;codecFailure.expected=fileIdentity(png);
        codecFailure.checkpoint=[&](ExportStage stage){if(stage==ExportStage::Row) denyCodec=true;};
        const auto beforeCodecFailure=bytes(png);
        rejects([&]{exportImage(png,document,codecFailure);});
        expect(codecProbeFailed,"PNG codec allocation failure seam was not reached after sampling");
        expect(bytes(png)==beforeCodecFailure,"PNG codec longjmp published partial output");
        const auto codecLedger=codecMemory->snapshot();
        expect(codecLedger.committedCpu==0 && codecLedger.pendingCpu==0,"PNG codec longjmp retained allocation charges");
        const auto jpeg=directory.filePath("white.jpeg");exportImage(jpeg,document,options);
        QImageReader jpegReader(jpeg);decoded=jpegReader.read();expect(!decoded.isNull() && decoded.width()==2 && decoded.height()==2,"JPEG dimensions/decode failed");
        first=decoded.pixelColor(0,0);
        expect(std::abs(first.red()-255)<=4 && std::abs(first.green()-188)<=4 && std::abs(first.blue()-188)<=4 && first.alpha()==255,"JPEG white flatten/color mismatch");
        const auto transparent=decoded.pixelColor(0,1);expect(transparent.red()>=251 && transparent.green()>=251 && transparent.blue()>=251,"JPEG transparent pixel was not white");
        expect(std::abs(decoded.dotsPerMeterX()-std::lround(144/0.0254))<=2,"JPEG DPI metadata changed");
        const auto originalJpeg=bytes(jpeg);
        options.expected=fileIdentity(jpeg);
        std::stop_source jpegCancellation;options.stop=jpegCancellation.get_token();unsigned jpegRows=0;
        options.checkpoint=[&](ExportStage stage){if(stage==ExportStage::Row && ++jpegRows==2) jpegCancellation.request_stop();};
        rejects([&]{exportImage(jpeg,document,options);});expect(bytes(jpeg)==originalJpeg,"Cancelled JPEG export replaced original");
        options.stop={};options.checkpoint={};options.expected={};
        const auto conflict=directory.filePath("conflict.png");exportImage(conflict,document,options);
        options.expected=fileIdentity(conflict);
        const QByteArray external("External replacement during encoding");
        options.checkpoint=[&](ExportStage stage) {
            if(stage!=ExportStage::Replace) return;
            QFile changed(conflict);expect(changed.open(QIODevice::WriteOnly|QIODevice::Truncate),"Cannot modify conflict fixture");
            expect(changed.write(external)==external.size(),"Cannot write external conflict fixture");changed.close();
        };
        rejects([&]{exportImage(conflict,document,options);});
        expect(bytes(conflict)==external,"Final identity check overwrote an external change");
        options.checkpoint={};
        const auto original=bytes(png);options.expected=fileIdentity(png);
        std::stop_source cancellation;options.stop=cancellation.get_token();unsigned rows=0;
        options.checkpoint=[&](ExportStage stage){if(stage==ExportStage::Row && ++rows==2) cancellation.request_stop();};
        rejects([&]{exportImage(png,document,options);});expect(bytes(png)==original,"Cancelled export replaced original");
        options.stop={};options.checkpoint=[](ExportStage stage){if(stage==ExportStage::Replace) throw std::runtime_error("Injected export replacement failure");};
        rejects([&]{exportImage(png,document,options);});expect(bytes(png)==original,"Failed export replaced original");
        options.checkpoint={};auto wrong=*options.expected;++wrong.size;options.expected=wrong;
        rejects([&]{exportImage(png,document,options);});expect(bytes(png)==original,"Identity conflict replaced original");
        options.expected={};rejects([&]{exportImage(png,document,options);});
        const auto baseline=memory->snapshot().committedCpu;
        auto denied=std::make_shared<MemoryAdmission>(1,0,[]{return MemorySample{1024*1024*1024,0};});options.memory=denied;
        const auto failed=directory.filePath("denied.png");rejects([&]{exportImage(failed,document,options);});expect(!QFile::exists(failed),"Admission denial published export");
        expect(denied->snapshot().committedCpu==0 && denied->snapshot().pendingCpu==0,"Admission denial leaked charges");
        options.memory=memory;options.quality=0;rejects([&]{exportImage(failed,document,options);});options.quality=100;
        rejects([&]{exportImage(directory.filePath("invalid.tiff"),document,options);});
        // Independent standard Multiply result: opaque sRGB .5 × .5 = .25.
        auto bottom=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1,1},pack(fromStraightSrgb({.5f,.5f,.5f,1})));
        auto top=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1,1},pack(fromStraightSrgb({.5f,.5f,.5f,1})));
        std::vector<LayerNode> layers{{Id::generate(),{},0,false,true,1,BlendMode::Normal,{},Sampling::Nearest,bottom},
            {Id::generate(),{},1,false,true,1,BlendMode::Multiply,{},Sampling::Nearest,top}};
        auto layered=std::make_shared<const DocumentSnapshot>(Id::generate(),1,1,96,layers);
        const auto composite=directory.filePath("composite.PNG");exportImage(composite,layered,options);
        decoded=QImageReader(composite).read();expect(!decoded.isNull() && std::abs(decoded.pixelColor(0,0).red()-64)<=1,"Multilayer export did not flatten Multiply");
        expect(memory->snapshot().committedCpu==baseline && memory->snapshot().pendingCpu==0,"Export retained scratch charges");
        std::cout<<"PNG alpha/color/DPI, JPEG white/color/DPI, atomic rollback, admission and layered export passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
