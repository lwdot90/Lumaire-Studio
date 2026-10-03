#include "io/image_import.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <algorithm>
#include <lcms2.h>
#include <zlib.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

namespace compositor::io {
using namespace engine;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Metadata { QByteArray icc; int orientation=1; double resolution=72; bool hasResolution=false,grayInput=false; };
unsigned be16(const unsigned char* p) { return (unsigned(p[0])<<8)|p[1]; }
std::uint32_t be32(const unsigned char* p) { return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3]; }
void exif(const QByteArray& bytes, Metadata& meta) {
    check(bytes.size()>=8,"Truncated EXIF metadata");
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    const bool little=bytes.startsWith("II"); check(little || bytes.startsWith("MM"),"Invalid EXIF byte order");
    auto u16=[&](std::size_t offset) -> unsigned {
        check(offset+2<=static_cast<std::size_t>(bytes.size()),"Truncated EXIF field");
        return little ? unsigned(p[offset])|(unsigned(p[offset+1])<<8) : be16(p+offset);
    };
    auto u32=[&](std::size_t offset) -> std::uint32_t {
        check(offset+4<=static_cast<std::size_t>(bytes.size()),"Truncated EXIF field");
        return little ? std::uint32_t(p[offset])|(std::uint32_t(p[offset+1])<<8)|(std::uint32_t(p[offset+2])<<16)|(std::uint32_t(p[offset+3])<<24) : be32(p+offset);
    };
    check(u16(2)==42,"Invalid EXIF TIFF header");
    const std::size_t start=u32(4); const auto count=u16(start);
    check(count<=4096 && start+2+std::size_t(count)*12<=static_cast<std::size_t>(bytes.size()),"Invalid EXIF directory");
    double resolution=0; unsigned unit=2; bool oriented=false;
    for(unsigned i=0;i<count;++i) {
        const auto offset=start+2+std::size_t(i)*12; const auto tag=u16(offset),type=u16(offset+2); const auto n=u32(offset+4);
        if(tag==0x112) {
            check(!oriented && type==3 && n==1,"Invalid EXIF orientation"); oriented=true;
            meta.orientation=static_cast<int>(u16(offset+8)); check(meta.orientation>=1 && meta.orientation<=8,"Invalid EXIF orientation value");
        } else if(tag==0x11a && type==5 && n==1) {
            const std::size_t data=u32(offset+8); const auto denominator=u32(data+4);
            check(denominator!=0,"Invalid EXIF resolution"); resolution=double(u32(data))/denominator;
        } else if(tag==0x128 && type==3 && n==1) unit=u16(offset+8);
    }
    if(resolution>0 && (unit==2 || unit==3)) { meta.resolution=resolution*(unit==3 ? 2.54 : 1.0); meta.hasResolution=true; }
}
QByteArray inflateProfile(const QByteArray& compressed) {
    QByteArray output(4*1024*1024,'\0');
    z_stream state{}; state.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData())); state.avail_in=static_cast<uInt>(compressed.size());
    state.next_out=reinterpret_cast<Bytef*>(output.data()); state.avail_out=static_cast<uInt>(output.size());
    check(inflateInit(&state)==Z_OK,"Cannot initialize ICC decompression");
    const auto rc=inflate(&state,Z_FINISH); const auto length=state.total_out; const auto remaining=state.avail_in; inflateEnd(&state);
    check(rc==Z_STREAM_END && remaining==0,"Malformed or oversized compressed ICC profile"); output.resize(static_cast<qsizetype>(length)); return output;
}
Metadata metadata(const QByteArray& bytes, const QByteArray& format) {
    Metadata meta;
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(format=="png") {
        check(bytes.size()>=8 && bytes.left(8)==QByteArray::fromHex("89504e470d0a1a0a"),"Invalid PNG signature");
        check(bytes.size()>=33 && be32(p+8)==13 && bytes.mid(12,4)=="IHDR","Missing PNG header");
        meta.grayInput=p[25]==0 || p[25]==4;
        qsizetype offset=8,metadataBytes=0,textBytes=0; bool ended=false,profile=false,oriented=false;
        while(offset<bytes.size()) {
            check(offset+12<=bytes.size(),"Truncated PNG chunk"); const auto length=be32(p+offset);
            check(length<=512u*1024*1024 && static_cast<std::uint64_t>(offset)+12+length<=static_cast<std::uint64_t>(bytes.size()),"Invalid PNG chunk length");
            const QByteArray kind=bytes.mid(offset+4,4);
            const bool metadataChunk=kind=="iCCP" || kind=="eXIf" || kind=="pHYs" || kind=="iTXt" || kind=="zTXt" || kind=="tEXt";
            if(metadataChunk) {
                metadataBytes+=length;
                check(length<=4u*1024*1024+65536 && metadataBytes<=16*1024*1024,"Oversized PNG metadata");
            }
            const auto data=metadataChunk ? bytes.mid(offset+8,length) : QByteArray{};
            const auto crc=crc32(0,p+offset+4,length+4);
            check(crc==be32(p+offset+8+length),"PNG chunk checksum mismatch");
            if(kind=="iCCP") {
                check(!profile,"Duplicate PNG ICC profile"); profile=true;
                const auto zero=data.indexOf('\0'); check(zero>=1 && zero<=79 && zero+2<data.size() && data[zero+1]==0,"Invalid PNG ICC chunk");
                meta.icc=inflateProfile(data.mid(zero+2)); check(!meta.icc.isEmpty(),"Empty PNG ICC profile");
            } else if(kind=="eXIf") { check(!oriented,"Duplicate EXIF"); oriented=true; exif(data,meta); }
            else if(kind=="zTXt") {
                const auto zero=data.indexOf('\0'); check(zero>=1 && zero<=79 && zero+2<data.size() && data[zero+1]==0,"Invalid PNG text chunk");
                textBytes+=inflateProfile(data.mid(zero+2)).size(); // Same decompression bound for optional text.
            } else if(kind=="iTXt") {
                const auto zero=data.indexOf('\0'); check(zero>=1 && zero<=79 && zero+3<=data.size(),"Invalid PNG international text");
                const auto languageEnd=data.indexOf('\0',zero+3),translatedEnd=languageEnd<0 ? -1 : data.indexOf('\0',languageEnd+1);
                check(translatedEnd>=0 && (data[zero+1]==0 || data[zero+1]==1) && data[zero+2]==0,"Invalid PNG international text fields");
                textBytes+=data[zero+1]==1 ? inflateProfile(data.mid(translatedEnd+1)).size() : data.size()-translatedEnd-1;
            } else if(kind=="tEXt") textBytes+=data.size();
            else if(kind=="pHYs" && data.size()==9 && data[8]==1) {
                meta.resolution=be32(reinterpret_cast<const unsigned char*>(data.constData()))*.0254; meta.hasResolution=meta.resolution>0;
            } else if(kind=="acTL") throw std::runtime_error("Animated PNG is not supported; import a single frame");
            check(textBytes<=4*1024*1024,"PNG text exceeds aggregate decoded metadata budget");
            offset+=static_cast<qsizetype>(length)+12;
            if(kind=="IEND") { check(length==0 && offset==bytes.size(),"Invalid PNG ending"); ended=true; break; }
        }
        check(ended,"Missing PNG ending");
    } else {
        check(bytes.size()>=4 && p[0]==0xff && p[1]==0xd8 && p[bytes.size()-2]==0xff && p[bytes.size()-1]==0xd9,"Invalid or truncated JPEG");
        std::map<unsigned,QByteArray> pieces; unsigned expected=0; qsizetype profileBytes=0; bool oriented=false;
        qsizetype offset=2;
        while(offset+1<bytes.size()) {
            check(p[offset++]==0xff,"Invalid JPEG marker"); while(offset<bytes.size() && p[offset]==0xff) ++offset;
            check(offset<bytes.size(),"Truncated JPEG marker"); const auto marker=p[offset++];
            if(marker==0xda || marker==0xd9) break;
            check(offset+2<=bytes.size(),"Truncated JPEG length"); const auto length=be16(p+offset);
            check(length>=2 && offset+length<=bytes.size(),"Invalid JPEG segment length"); const auto data=bytes.mid(offset+2,length-2);
            if(marker==0xe2 && data.startsWith(QByteArray("ICC_PROFILE\0",12))) {
                check(data.size()>=14,"Truncated JPEG ICC segment"); const auto index=static_cast<unsigned char>(data[12]),count=static_cast<unsigned char>(data[13]);
                check(index>=1 && count>=1 && index<=count && (!expected || expected==count),"Invalid JPEG ICC sequence"); expected=count;
                profileBytes+=data.size()-14; check(profileBytes<=4*1024*1024,"Oversized JPEG ICC profile");
                check(pieces.emplace(index,data.mid(14)).second,"Duplicate JPEG ICC segment");
            } else if(marker==0xe1 && data.startsWith(QByteArray("Exif\0\0",6))) { check(!oriented,"Duplicate EXIF"); oriented=true; exif(data.mid(6),meta); }
            else if((marker>=0xc0 && marker<=0xcf) && marker!=0xc4 && marker!=0xc8 && marker!=0xcc) {
                check(data.size()>=6 && (data[5]==1 || data[5]==3),"Unsupported JPEG color space (CMYK is not supported)");
                meta.grayInput=data[5]==1;
            }
            offset+=length;
        }
        check(pieces.size()==expected,"Incomplete JPEG ICC profile");
        for(const auto& [index,piece]:pieces) { check(meta.icc.size()+piece.size()<=4*1024*1024,"Oversized JPEG ICC profile"); meta.icc+=piece; }
        if(expected) check(!meta.icc.isEmpty(),"Empty JPEG ICC profile");
    }
    return meta;
}
class ColorTransform {
    cmsHPROFILE source_=nullptr,working_=nullptr;
    cmsHTRANSFORM transform_=nullptr;
public:
    bool gray=false;
    explicit ColorTransform(const QByteArray& profile) {
        if(profile.isEmpty()) return;
        source_=cmsOpenProfileFromMem(profile.constData(),static_cast<cmsUInt32Number>(profile.size()));
        check(source_!=nullptr,"Malformed source ICC profile");
        try {
            const auto space=cmsGetColorSpace(source_); gray=space==cmsSigGrayData;
            check(gray || space==cmsSigRgbData,"Unsupported source ICC color space");
            const cmsCIExyY white{.3127,.3290,1}; const cmsCIExyYTRIPLE primaries{{.64,.33,1},{.30,.60,1},{.15,.06,1}};
            auto gamma=cmsBuildGamma(nullptr,1.0); check(gamma!=nullptr,"Cannot create linear transfer curve");
            cmsToneCurve* curves[]{gamma,gamma,gamma}; working_=cmsCreateRGBProfile(&white,&primaries,curves); cmsFreeToneCurve(gamma);
            check(working_!=nullptr,"Cannot create working color profile");
            transform_=cmsCreateTransform(source_,gray ? TYPE_GRAY_FLT : TYPE_RGB_FLT,working_,TYPE_RGB_FLT,
                INTENT_RELATIVE_COLORIMETRIC,cmsFLAGS_BLACKPOINTCOMPENSATION|cmsFLAGS_NOOPTIMIZE);
            check(transform_!=nullptr,"ICC transform could not be created");
        } catch(...) { if(working_) cmsCloseProfile(working_); cmsCloseProfile(source_); throw; }
    }
    ~ColorTransform() { if(transform_) cmsDeleteTransform(transform_); if(working_) cmsCloseProfile(working_); if(source_) cmsCloseProfile(source_); }
    void convert(const std::vector<float>& input, std::vector<float>& output, std::size_t count) {
        if(transform_) cmsDoTransform(transform_,input.data(),output.data(),static_cast<cmsUInt32Number>(count));
        else for(std::size_t i=0;i<count*3;++i) output[i]=decodeSrgb(input[i]);
    }
};
struct SourcePoint { int x,y; };
SourcePoint sourcePoint(int orientation,int x,int y,int width,int height) {
    switch(orientation) {
        case 2:return {width-1-x,y}; case 3:return {width-1-x,height-1-y}; case 4:return {x,height-1-y};
        case 5:return {y,x}; case 6:return {y,height-1-x};
        case 7:return {width-1-y,height-1-x}; case 8:return {width-1-y,x};
        default:return {x,y};
    }
}
}
ImportedImage importImage(const QString& path, TileStore& tiles, std::stop_token stop) {
    if(stop.stop_requested()) throw std::runtime_error("Import cancelled");
    const auto& admission=tiles.memoryAdmission();
    // Hold the initial metadata/ICC/codec/conversion allowance pending throughout
    // import. It is conservative admission, not measurement of hidden library peaks.
    std::optional<MemoryAdmission::Reservation> transientMemory,decodedMemory;
    Metadata meta; std::unique_ptr<ColorTransform> transform; QImage image;
    {
        QFile file(path); check(file.open(QIODevice::ReadOnly),"Image could not be opened");
        const auto encodedLength=file.size();
        check(encodedLength>0 && encodedLength<=512LL*1024*1024,"Encoded image exceeds 512 MiB limit");
        if(admission) transientMemory=admission->require(64u*1024*1024);
        std::optional<MemoryAdmission::Reservation> encodedMemory;
        if(admission) encodedMemory=admission->require(static_cast<std::uint64_t>(encodedLength)+1);
        const auto encoded=file.read(encodedLength+1);
        if(encodedMemory) encodedMemory->commit();
        check(encoded.size()==encodedLength && file.size()==encodedLength,"Incomplete or growing image file");
        if(stop.stop_requested()) throw std::runtime_error("Import cancelled");
        const QByteArray format=encoded.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ? "png" : encoded.startsWith(QByteArray::fromHex("ffd8")) ? "jpeg" : "";
        check(!format.isEmpty(),"Choose a PNG or JPEG image; other codecs arrive later");
        // Bound and validate metadata before the codec's size probe can parse ICC
        // chunks. Invalid compressed profiles must never silently become sRGB.
        meta=metadata(encoded,format); transform=std::make_unique<ColorTransform>(meta.icc);
        check(!transform->gray || meta.grayInput,"A grayscale ICC profile cannot describe this RGB image");
        QBuffer buffer; buffer.setData(encoded); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer); reader.setDecideFormatFromContent(true); reader.setAutoTransform(false);
        const auto size=reader.size(); Extent{0,0,size.width(),size.height()}.validate();
        // Reader's own bounded allocation limit remains enabled. Reject before
        // decode as well; no decoder can use header dimensions to allocate unboundedly.
        const auto decodedBytes=std::uint64_t(size.width())*std::uint64_t(size.height())*8;
        check(decodedBytes<=1024ULL*1024*1024,"Decoded image exceeds import budget");
        if(admission) decodedMemory=admission->require(decodedBytes);
        if(stop.stop_requested()) throw std::runtime_error("Import cancelled");
        image=reader.read(); check(!image.isNull(),"Image decode failed (damaged file or decoder allocation limit)");
        if(decodedMemory) decodedMemory->commit();
        check(image.size()==size,"Image dimensions changed during decode");
    } // Release encoded input and decoder state before accumulating canonical tiles.
    if(stop.stop_requested()) throw std::runtime_error("Import cancelled");
    auto& color=*transform;
    const auto ppi=meta.hasResolution ? meta.resolution : (image.dotsPerMeterX()>0 ? image.dotsPerMeterX()*.0254 : 72.0);
    const bool transpose=meta.orientation>=5; const int width=transpose ? image.height() : image.width(),height=transpose ? image.width() : image.height();
    const Extent extent{0,0,width,height}; TileMap map;
    for(std::int64_t ty=0;ty<=floorTile(height-1);++ty) for(std::int64_t tx=0;tx<=floorTile(width-1);++tx) {
        if(stop.stop_requested()) throw std::runtime_error("Import cancelled");
        const auto region=tileExtent(extent,{tx,ty}); const auto count=static_cast<std::size_t>(region.width*region.height);
        // Each orientation maps an output tile to one bounded source rectangle.
        // Convert only that rectangle; PNG16 already decoded as RGBA64 needs no copy.
        QImage converted; const QImage* samples=&image; int sourceX=0,sourceY=0;
        if(image.format()!=QImage::Format_RGBA64) {
            const auto first=sourcePoint(meta.orientation,static_cast<int>(region.x),static_cast<int>(region.y),image.width(),image.height());
            const auto last=sourcePoint(meta.orientation,static_cast<int>(region.x+region.width-1),static_cast<int>(region.y+region.height-1),image.width(),image.height());
            sourceX=std::min(first.x,last.x); sourceY=std::min(first.y,last.y);
            const QRect sourceRegion(sourceX,sourceY,std::abs(last.x-first.x)+1,std::abs(last.y-first.y)+1);
            converted=image.copy(sourceRegion).convertToFormat(QImage::Format_RGBA64);
            check(!converted.isNull(),"Cannot allocate decoded pixels"); samples=&converted;
        }
        std::vector<float> input(count*(color.gray ? 1 : 3)),output(count*3);
        std::vector<Pixel> pixels(count);
        for(int y=0;y<region.height;++y) for(int x=0;x<region.width;++x) {
            const auto source=sourcePoint(meta.orientation,static_cast<int>(region.x)+x,static_cast<int>(region.y)+y,image.width(),image.height());
            const auto p=reinterpret_cast<const QRgba64*>(samples->constScanLine(source.y-sourceY))[source.x-sourceX]; const auto index=static_cast<std::size_t>(y*region.width+x);
            pixels[index].a=p.alpha()/65535.f;
            if(color.gray) input[index]=p.red()/65535.f;
            else { input[index*3]=p.red()/65535.f; input[index*3+1]=p.green()/65535.f; input[index*3+2]=p.blue()/65535.f; }
        }
        color.convert(input,output,count);
        for(std::size_t i=0;i<count;++i) { pixels[i].r=output[i*3]*pixels[i].a; pixels[i].g=output[i*3+1]*pixels[i].a; pixels[i].b=output[i*3+2]*pixels[i].a; }
        auto tile=tiles.create(static_cast<int>(region.width),static_cast<int>(region.height),pixels);
        if(!tile->uniform() || tile->pixel(0,0)!=PackedPixel{}) map.emplace(TileCoord{tx,ty},std::move(tile));
    }
    std::shared_ptr<const SourceProfile> profile;
    if(!meta.icc.isEmpty()) {
        const auto* first=reinterpret_cast<const std::uint8_t*>(meta.icc.constData());
        profile=std::make_shared<const SourceProfile>(SourceProfile{Id::generate(),{first,first+meta.icc.size()}});
    }
    auto raster=std::make_shared<const RasterSnapshot>(Id::generate(),extent,PackedPixel{},std::move(map),0,profile);
    auto document=std::make_shared<const DocumentSnapshot>(Id::generate(),Id::generate(),width,height,ppi,raster,0,QFileInfo(path).completeBaseName().toUtf8().toStdString());
    return {document,profile ? "Embedded ICC converted to linear sRGB; source profile retained" : "No embedded ICC profile: assumed sRGB"};
}
}
