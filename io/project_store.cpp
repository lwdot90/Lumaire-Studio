#include "io/project_store.h"
#include "io/adjustment_parameters.h"
#include "io/retouch_parameters.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "project_schema.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <sqlite3.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <lcms2.h>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <fcntl.h>
#include <unistd.h>

namespace compositor::io {
using namespace engine;
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
void require(bool condition, const char* message) { if(!condition) fail(message); }
std::string layerParameters(bool folder,Sampling sampling) {
    const std::string prefix=folder ? "{\"version\":1,\"kind\":\"folder\",\"values\":" : "{\"version\":1,\"kind\":\"raster\",\"values\":";
    if(sampling==Sampling::Bilinear) return prefix+"{}}";
    require(!folder,"Folder sampling is unsupported");
    if(sampling==Sampling::Nearest) return prefix+"{\"sampling\":\"nearest\"}}";
    if(sampling==Sampling::Lanczos) return prefix+"{\"sampling\":\"lanczos3\"}}";
    fail("Unsupported layer sampling");
}
void cancelled(std::stop_token stop) { if(stop.stop_requested()) fail("Operation cancelled"); }
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256); }
QByteArray bytesOf(const std::vector<std::uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),static_cast<qsizetype>(bytes.size()));
}
std::span<const std::uint8_t> byteSpan(const QByteArray& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size())};
}
QByteArray packedBytes(PackedPixel pixel) {
    QByteArray bytes(8,'\0');
    for(int i=0;i<4;++i) { bytes[i*2]=static_cast<char>(pixel[static_cast<std::size_t>(i)]&255); bytes[i*2+1]=static_cast<char>(pixel[static_cast<std::size_t>(i)]>>8); }
    return bytes;
}
PackedPixel readPacked(const QByteArray& bytes) {
    require(bytes.size()==8,"Invalid asset default length");
    PackedPixel p{};
    for(int i=0;i<4;++i) p[static_cast<std::size_t>(i)]=static_cast<Half>(static_cast<unsigned char>(bytes[i*2])|(static_cast<unsigned>(static_cast<unsigned char>(bytes[i*2+1]))<<8));
    validateCanonical(p); return p;
}
std::string selectionJson(const std::optional<Selection>& selection,int width,int height,int version=6) {
    if(!selection) return "null";
    selection->validate(width,height);
    const auto& b=selection->bounds;
    if(version>=6) {
        QJsonObject o{{"version",2},{"shape",selection->shape==SelectionShape::Rectangle ? "rectangle" : "ellipse"},{"bounds",QJsonArray{QString::number(b.x),QString::number(b.y),QString::number(b.width),QString::number(b.height)}},{"inverted",selection->inverted},{"featherRadius",selection->featherRadius}};
        return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
    }
    require(selection->featherRadius==0,"Legacy selection cannot retain feathering");
    return std::string("{\"version\":1,\"shape\":\"")+(selection->shape==SelectionShape::Rectangle ? "rectangle" : "ellipse")+
        "\",\"bounds\":["+std::to_string(b.x)+","+std::to_string(b.y)+","+std::to_string(b.width)+","+std::to_string(b.height)+
        "],\"inverted\":"+(selection->inverted ? "true" : "false")+"}";
}
std::optional<Selection> readSelection(const std::string& text,int width,int height,int version) {
    require(text.size()<=512,"Oversized selection parameters");
    if(text=="null") return {};
    if(version>=6) {
        QJsonParseError error;auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(text),&error);
        require(error.error==QJsonParseError::NoError&&doc.isObject(),"Invalid selection JSON");auto o=doc.object();
        require(o.value("version").toInt(-1)==2&&o.value("bounds").isArray()&&o.value("bounds").toArray().size()==4&&o.value("inverted").isBool()&&o.value("featherRadius").isDouble(),"Invalid selection parameters");
        std::array<std::int64_t,4> b{};auto a=o.value("bounds").toArray();for(int i=0;i<4;++i){bool ok=false;b[static_cast<std::size_t>(i)]=a[i].toString().toLongLong(&ok);require(ok,"Selection bound overflow");}
        auto shape=o.value("shape").toString();require(shape=="rectangle"||shape=="ellipse","Unsupported selection shape");
        Selection result{{b[0],b[1],b[2],b[3]},shape=="rectangle"?SelectionShape::Rectangle:SelectionShape::Ellipse,o.value("inverted").toBool(),o.value("featherRadius").toDouble()};result.validate(width,height);
        require(selectionJson(result,width,height,version)==text,"Noncanonical selection parameters");return result;
    }
    static const QRegularExpression pattern(QStringLiteral(R"JSON(\A\{"version":1,"shape":"(rectangle|ellipse)","bounds":\[(-?(?:0|[1-9][0-9]*)),(-?(?:0|[1-9][0-9]*)),(-?(?:0|[1-9][0-9]*)),(-?(?:0|[1-9][0-9]*))\],"inverted":(true|false)\}\z)JSON"));
    const auto match=pattern.match(QString::fromUtf8(text));
    require(match.hasMatch(),"Invalid or unsupported selection parameters");
    std::array<std::int64_t,4> values{};
    for(int i=0;i<4;++i) {bool valid=false;values[static_cast<std::size_t>(i)]=match.captured(i+2).toLongLong(&valid);require(valid,"Selection bound overflow");}
    Selection selection{{values[0],values[1],values[2],values[3]},match.captured(1)=="rectangle" ? SelectionShape::Rectangle : SelectionShape::Ellipse,match.captured(6)=="true"};
    selection.validate(width,height);
    require(selectionJson(selection,width,height,version)==text,"Noncanonical selection parameters");
    return selection;
}
void validateMaskPixel(PackedPixel value) {
    validateCanonical(value);
    require(value[0]==0 && value[1]==0 && value[2]==0,"Mask coverage has nonzero RGB");
}
void validateMaskBytes(std::span<const std::uint8_t> bytes,std::stop_token stop) {
    require(bytes.size()%8==0,"Invalid mask canonical length");
    for(std::size_t offset=0;offset<bytes.size();offset+=8) {
        if(offset%4096==0) cancelled(stop);
        PackedPixel value{};
        for(std::size_t c=0;c<4;++c) value[c]=static_cast<Half>(bytes[offset+2*c]|(static_cast<unsigned>(bytes[offset+2*c+1])<<8));
        validateMaskPixel(value);
    }
}
QByteArray matrixBytes(const Affine& matrix) {
    QByteArray result(72,'\0');
    const std::array<double,9> values{matrix.a,matrix.c,matrix.tx,matrix.b,matrix.d,matrix.ty,0,0,1};
    for(int i=0;i<9;++i) {
        const auto bits=std::bit_cast<std::uint64_t>(values[static_cast<std::size_t>(i)]);
        for(int b=0;b<8;++b) result[i*8+b]=static_cast<char>((bits>>(8*b))&255);
    }
    return result;
}
Affine readMatrix(const QByteArray& bytes) {
    require(bytes.size()==72,"Invalid transform length");
    std::array<double,9> values{};
    for(std::size_t i=0;i<9;++i) {
        std::uint64_t bits=0;
        for(int b=0;b<8;++b) bits|=std::uint64_t(static_cast<unsigned char>(bytes[static_cast<qsizetype>(i*8)+b]))<<(8*b);
        values[i]=std::bit_cast<double>(bits); require(std::isfinite(values[i]),"Nonfinite transform");
    }
    require(values[6]==0 && values[7]==0 && values[8]!=0,"Projective transforms are not supported yet");
    Affine result{values[0]/values[8],values[3]/values[8],values[1]/values[8],values[4]/values[8],values[2]/values[8],values[5]/values[8]};
    result.inverse(); return result;
}
void validateProfile(const QByteArray& bytes) {
    require(!bytes.isEmpty() && bytes.size()<=4*1024*1024,"Invalid ICC size");
    auto profile=cmsOpenProfileFromMem(bytes.constData(),static_cast<cmsUInt32Number>(bytes.size()));
    require(profile!=nullptr,"Malformed ICC profile");
    const auto space=cmsGetColorSpace(profile);
    cmsCloseProfile(profile);
    require(space==cmsSigRgbData || space==cmsSigGrayData,"Unsupported ICC color space");
}
class Db {
public:
    sqlite3* handle=nullptr;
    explicit Db(const QByteArray& path, int flags) {
        if(sqlite3_open_v2(path.constData(),&handle,flags,nullptr)!=SQLITE_OK) {
            if(handle) sqlite3_close(handle);
            handle=nullptr; fail("Cannot open project database");
        }
        sqlite3_extended_result_codes(handle,1);
        sqlite3_limit(handle,SQLITE_LIMIT_LENGTH,5*1024*1024);
        sqlite3_limit(handle,SQLITE_LIMIT_SQL_LENGTH,65536);
        sqlite3_limit(handle,SQLITE_LIMIT_COLUMN,64);
        sqlite3_limit(handle,SQLITE_LIMIT_ATTACHED,0);
        sqlite3_limit(handle,SQLITE_LIMIT_EXPR_DEPTH,64);
        sqlite3_limit(handle,SQLITE_LIMIT_COMPOUND_SELECT,8);
        sqlite3_limit(handle,SQLITE_LIMIT_VARIABLE_NUMBER,32);
        sqlite3_db_config(handle,SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,0,nullptr);
        sqlite3_db_config(handle,SQLITE_DBCONFIG_DEFENSIVE,1,nullptr);
    }
    ~Db() { if(handle) sqlite3_close_v2(handle); }
    Db(const Db&)=delete;
    void exec(const char* sql) {
        if(sqlite3_exec(handle,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) fail("Project SQL operation failed");
    }
    void close() { require(sqlite3_close(handle)==SQLITE_OK,"Project close failed"); handle=nullptr; }
};
class Query {
    sqlite3_stmt* statement_=nullptr;
public:
    Query(Db& db, const char* sql) {
        require(sqlite3_prepare_v2(db.handle,sql,-1,&statement_,nullptr)==SQLITE_OK,"Invalid project schema or query");
    }
    ~Query() { sqlite3_finalize(statement_); }
    Query(const Query&)=delete;
    bool row() {
        const int rc=sqlite3_step(statement_);
        require(rc==SQLITE_ROW || rc==SQLITE_DONE,"Corrupt, cancelled or unreadable project");
        return rc==SQLITE_ROW;
    }
    void end() { require(!row(),"Unexpected duplicate project record"); }
    void bind(int i, std::int64_t v) { require(sqlite3_bind_int64(statement_,i,v)==SQLITE_OK,"SQL integer binding failed"); }
    void real(int i, double v) { require(sqlite3_bind_double(statement_,i,v)==SQLITE_OK,"SQL real binding failed"); }
    void text(int i, const std::string& v) { require(sqlite3_bind_text(statement_,i,v.data(),static_cast<int>(v.size()),SQLITE_TRANSIENT)==SQLITE_OK,"SQL text binding failed"); }
    void blob(int i, const QByteArray& v) { require(sqlite3_bind_blob(statement_,i,v.constData(),static_cast<int>(v.size()),SQLITE_TRANSIENT)==SQLITE_OK,"SQL blob binding failed"); }
    bool null(int i) const { return sqlite3_column_type(statement_,i)==SQLITE_NULL; }
    std::int64_t integer(int i) const {
        require(sqlite3_column_type(statement_,i)==SQLITE_INTEGER,"Expected project integer"); return sqlite3_column_int64(statement_,i);
    }
    double real(int i) const {
        const auto t=sqlite3_column_type(statement_,i);
        require(t==SQLITE_FLOAT || t==SQLITE_INTEGER,"Expected project number");
        const auto value=sqlite3_column_double(statement_,i); require(std::isfinite(value),"Nonfinite project number"); return value;
    }
    std::size_t blobSize(int i) const {
        require(sqlite3_column_type(statement_,i)==SQLITE_BLOB,"Expected project blob");
        return static_cast<std::size_t>(sqlite3_column_bytes(statement_,i));
    }
    QByteArray blob(int i) const {
        require(sqlite3_column_type(statement_,i)==SQLITE_BLOB,"Expected project blob");
        return QByteArray(static_cast<const char*>(sqlite3_column_blob(statement_,i)),sqlite3_column_bytes(statement_,i));
    }
    std::string text(int i) const {
        require(sqlite3_column_type(statement_,i)==SQLITE_TEXT,"Expected project text");
        const auto* p=reinterpret_cast<const char*>(sqlite3_column_text(statement_,i));
        return {p,static_cast<std::size_t>(sqlite3_column_bytes(statement_,i))};
    }
};
std::vector<std::array<std::string,4>> schema(Db& db) {
    Query q(db,"SELECT type,name,tbl_name,coalesce(sql,'') FROM sqlite_schema ORDER BY type,name");
    std::vector<std::array<std::string,4>> result;
    while(q.row()) {
        require(result.size()<32,"Too many schema objects");
        result.push_back({q.text(0),q.text(1),q.text(2),q.text(3)});
    }
    return result;
}
std::int64_t validateSchema(Db& db) {
    // Only the authored baseline schema is accepted. Never evaluate supplied
    // DDL, views, triggers or virtual tables, nor silently drop unknown fields.
    Query app(db,"PRAGMA application_id"); require(app.row() && app.integer(0)==1129337418,"Not a Compositor project"); app.end();
    Query version(db,"PRAGMA user_version"); require(version.row(),"Missing project version");
    const auto value=version.integer(0); require(value==1 || value==2 || value==3 || value==4 || value==5 || value==6,"Unsupported project version"); version.end();
    Db trusted(":memory:",SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
    trusted.exec(value==1 ? projectSchemaV1 : value==2 ? projectSchemaV2 : value==3 ? projectSchema : value==4 ? projectSchemaV4 : value==5 ? projectSchemaV5 : projectSchemaV6);
    require(schema(db)==schema(trusted),"Project table structure does not match its declared version");
    return value;
}
struct ReadProgress { std::stop_token stop; std::uint64_t ticks=0; };
int progress(void* context) {
    auto& p=*static_cast<ReadProgress*>(context);
    return p.stop.stop_requested() || ++p.ticks>1000000;
}
struct Fd {
    int value=-1;
    explicit Fd(int fd):value(fd) { require(fd>=0,"Cannot open save directory or lock"); }
    ~Fd() { if(value>=0) ::close(value); }
    Fd(const Fd&)=delete;
};
std::mutex saveMutex;
void checkpoint(const SaveOptions& options, SaveStage stage) {
    cancelled(options.stop); if(options.checkpoint) options.checkpoint(stage);
}
}
std::optional<FileIdentity> fileIdentity(const QString& path) {
    struct stat value{};
    if(::lstat(QFile::encodeName(path).constData(),&value)!=0) {
        if(errno==ENOENT) return {};
        fail("Cannot inspect project path");
    }
    require(S_ISREG(value.st_mode),"Project path is a symlink or not a regular file; choose a regular file");
    return FileIdentity{static_cast<std::uint64_t>(value.st_dev),static_cast<std::uint64_t>(value.st_ino),value.st_size,
        value.st_mtim.tv_sec,value.st_mtim.tv_nsec,value.st_ctim.tv_sec,value.st_ctim.tv_nsec};
}
LoadedProject loadProject(const QString& path, TileStore& tiles, std::stop_token stop) {
    cancelled(stop);
    const auto before=fileIdentity(path);
    require(before && before->size>0 && before->size<=4LL*1024*1024*1024,"Project missing or exceeds 4 GiB limit");
    Db db(QFile::encodeName(path),SQLITE_OPEN_READONLY|SQLITE_OPEN_NOFOLLOW);
    db.exec("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; PRAGMA cache_size=-4096; PRAGMA mmap_size=0; BEGIN");
    ReadProgress state{stop}; sqlite3_progress_handler(db.handle,1000,progress,&state);
    const auto formatVersion=validateSchema(db);
    Query integrity(db,"PRAGMA quick_check(1)"); require(integrity.row() && integrity.text(0)=="ok","Project database is corrupt"); integrity.end();
    Query project(db,formatVersion>=3 ? "SELECT id,format,uuid,width,height,resolution,working_space,saved_revision,required_features_json,selection_json FROM project" : "SELECT id,format,uuid,width,height,resolution,working_space,saved_revision,required_features_json,'null' FROM project");
    require(project.row(),"Project header missing");
    require(project.integer(0)==1 && project.text(1)=="compositor-linux" && project.text(6)=="linear-srgb-extended-v1" && project.text(8)=="[]","Unsupported project features");
    Id docId(project.text(2));
    const auto width=project.integer(3),height=project.integer(4),revision=project.integer(7);
    Extent{0,0,width,height}.validate(); const auto resolution=project.real(5);
    const auto selection=readSelection(project.text(9),static_cast<int>(width),static_cast<int>(height),static_cast<int>(formatVersion)); project.end();
    Query profiles(db,"SELECT uuid,role,icc,checksum FROM profiles");
    std::map<std::string,std::shared_ptr<const SourceProfile>> profileMap;
    while(profiles.row()) {
        const Id id(profiles.text(0)); const auto data=profiles.blob(2);
        require(profileMap.size()<16 && profiles.text(1)=="source","Unsupported profile role or excessive profile count");
        require(hash(data)==profiles.blob(3),"ICC checksum mismatch"); validateProfile(data);
        profileMap.emplace(id.text(),std::make_shared<const SourceProfile>(SourceProfile{id,{byteSpan(data).begin(),byteSpan(data).end()}}));
    }
    std::set<std::string> usedProfiles;
    std::map<std::string,std::shared_ptr<const RasterSnapshot>> assetMap;
    std::map<std::string,TileMap> tileMaps;
    std::uint64_t pixelCount=0,maskPixelCount=0;
    std::set<std::string> maskAssets;
    Query assets(db,"SELECT uuid,origin_x,origin_y,width,height,format,default_value,revision,source_profile FROM assets");
    while(assets.row()) {
        cancelled(stop); require(assetMap.size()<static_cast<std::size_t>(formatVersion>=3 ? 20000 : 10000),"Excess asset count"); Id assetId(assets.text(0));
        const Extent extent{assets.integer(1),assets.integer(2),assets.integer(3),assets.integer(4)}; extent.validate();
        const bool isMask=assets.text(5)=="mask-rgba16f-le";
        require(assets.text(5)=="rgba16f-le" || (formatVersion>=3 && isMask),"Unsupported asset format");
        auto& count=isMask ? maskPixelCount : pixelCount;
        count+=static_cast<std::uint64_t>(extent.width)*static_cast<std::uint64_t>(extent.height);
        require(count<=100000000,"Aggregate color or mask extents exceed 100 MP");
        if(isMask) maskAssets.insert(assetId.text());
        std::shared_ptr<const SourceProfile> profile;
        require(!isMask || assets.null(8),"Mask coverage must not have a source profile");
        if(!assets.null(8)) {
            const auto key=Id(assets.text(8)).text(); const auto found=profileMap.find(key);
            require(found!=profileMap.end(),"Missing source ICC profile"); profile=found->second; usedProfiles.insert(key);
        }
        const auto defaultValue=readPacked(assets.blob(6)); if(isMask) validateMaskPixel(defaultValue);
        assetMap.emplace(assetId.text(),std::make_shared<const RasterSnapshot>(assetId,extent,defaultValue,TileMap{},assets.integer(7),profile));
    }
    require(usedProfiles.size()==profileMap.size(),"Orphan profile");
    struct MaskRecord {std::string asset; bool enabled; Affine transform;};
    std::map<std::string,MaskRecord> maskRecords;
    Query masks(db,"SELECT uuid,asset_uuid,enabled,linked,transform,exterior_coverage FROM masks");
    while(masks.row()) {
        cancelled(stop); require(formatVersion>=3 && maskRecords.size()<10000,"Unsupported or excessive mask records");
        const auto key=Id(masks.text(0)).text(),asset=Id(masks.text(1)).text();
        require(maskAssets.contains(asset),"Mask record must reference a coverage asset");
        const auto enabled=masks.integer(2);
        require((enabled==0 || enabled==1) && masks.integer(3)==1 && masks.real(5)==1,"Unsupported mask enable, linking or exterior coverage");
        require(maskRecords.emplace(key,MaskRecord{asset,enabled==1,readMatrix(masks.blob(4))}).second,"Duplicate mask record");
    }
    std::set<std::string> usedMasks;
    Query layers(db,"SELECT uuid,parent_uuid,sibling_order,type,name,visible,opacity,blend_mode,transform,asset_uuid,mask_uuid,clipping_source_uuid,parameters_json FROM layers");
    std::vector<LayerNode> nodes;
    std::set<std::string> usedAssets;
    std::size_t jsonBytes=2;
    std::map<std::string,std::string> adjustmentJson;
    std::map<std::string,std::string> retouchJson;
    while(layers.row()) {
        cancelled(stop); require(nodes.size()<10000,"Excess layer count"); LayerNode node{Id(layers.text(0))};
        if(!layers.null(1)) node.parent=Id(layers.text(1));
        const auto order=layers.integer(2); require(order>=0 && order<10000,"Invalid sibling order"); node.siblingOrder=static_cast<int>(order);
        const auto type=layers.text(3); require(type=="raster" || type=="folder","Unsupported layer type"); node.folder=type=="folder";
        node.name=layers.text(4); const auto visible=layers.integer(5); const auto opacity=layers.real(6);
        require((visible==0 || visible==1) && opacity>=0 && opacity<=1 && node.name.size()<=4096,"Invalid layer properties");
        require(QString::fromUtf8(node.name).toUtf8().toStdString()==node.name,"Invalid UTF-8 layer name");
        node.visible=visible==1; node.opacity=static_cast<float>(opacity); node.blend=parseBlendMode(layers.text(7)); node.localToDocument=readMatrix(layers.blob(8));
        if(formatVersion==1) {
            require(static_cast<int>(node.blend)<=static_cast<int>(BlendMode::Luminosity),"Version 1 does not support extended blend modes");
            require(!node.folder || opacity==1,"Version 1 does not support folder opacity");
        }
        require(layers.null(11),"Clipping is not supported yet");
        const auto parameters=layers.text(12); jsonBytes+=parameters.size();
        require(jsonBytes<=4*1024*1024,"Excess layer parameter bytes");
        if(parameters==layerParameters(node.folder,Sampling::Bilinear)) node.sampling=Sampling::Bilinear;
        else if(!node.folder && parameters==layerParameters(false,Sampling::Nearest)) node.sampling=Sampling::Nearest;
        else if(!node.folder && parameters==layerParameters(false,Sampling::Lanczos)) node.sampling=Sampling::Lanczos;
        else if(formatVersion==6 && !node.folder && QJsonDocument::fromJson(QByteArray::fromStdString(parameters)).object().value("version").toInt(-1)==4) retouchJson.emplace(node.id.text(),parameters);
        else if(formatVersion>=4 && !node.folder) adjustmentJson.emplace(node.id.text(),parameters);
        else fail("Unsupported layer parameters");
        if(node.folder) require(layers.null(9),"Folder must not have an asset");
        else {
            const auto key=Id(layers.text(9)).text(); const auto found=assetMap.find(key);
            require(found!=assetMap.end() && !maskAssets.contains(key),"Missing or wrong-kind layer asset"); node.raster=found->second; usedAssets.insert(key);
            if(retouchJson.contains(node.id.text())) {
                const auto stored=readRetouchParameters(parameters,node.raster);
                require(!assetMap.contains(stored.intermediateId.text())&&!assetMap.contains(stored.renderedId.text()),"Retouch derived identity conflicts with stored asset");node.sampling=stored.sampling;
            }
            if(adjustmentJson.contains(node.id.text())) {
                const auto stored=readAdjustmentParameters(parameters,node.raster,static_cast<int>(formatVersion));
                require(!assetMap.contains(stored.renderedId.text()),"Derived raster identity conflicts with stored source or mask");
                node.sampling=stored.sampling;
            }
        }
        if(!layers.null(10)) {
            require(formatVersion>=3 && !node.folder,"Unsupported folder or legacy mask");
            const auto key=Id(layers.text(10)).text(); const auto found=maskRecords.find(key);
            require(found!=maskRecords.end(),"Missing layer mask");
            const auto asset=assetMap.at(found->second.asset);
            require(asset->extent==node.raster->extent && found->second.transform==node.localToDocument,"Mask extent or linked transform does not match its layer");
            node.mask=asset; node.maskEnabled=found->second.enabled;
            require(usedMasks.insert(key).second,"Mask placement record is shared between layers"); usedAssets.insert(found->second.asset);
        }
        nodes.push_back(std::move(node));
    }
    require(usedMasks.size()==maskRecords.size(),"Orphan mask record");
    require(usedAssets.size()==assetMap.size(),"Orphan raster or mask asset");
    // Validate the entire graph before allocating any decoded tile payloads.
    const LayerStack validated(nodes);
    Query payloads(db,"SELECT asset_uuid,tile_x,tile_y,width,height,encoding,decoded_size,checksum,payload FROM tiles");
    while(payloads.row()) {
        cancelled(stop); const auto key=Id(payloads.text(0)).text(); const auto found=assetMap.find(key);
        require(found!=assetMap.end(),"Orphan tile"); const auto extent=found->second->extent; auto& map=tileMaps[key];
        const auto maxTiles=(floorTile(extent.x+extent.width-1)-floorTile(extent.x)+1)*(floorTile(extent.y+extent.height-1)-floorTile(extent.y)+1);
        require(static_cast<std::int64_t>(map.size())<maxTiles,"Excess tile count");
        const TileCoord coord{payloads.integer(1),payloads.integer(2)}; const auto region=tileExtent(extent,coord);
        const auto decoded=region.width*region.height*8;
        require(payloads.integer(3)==region.width && payloads.integer(4)==region.height && payloads.integer(6)==decoded,"Invalid tile dimensions or decoded length");
        const auto encoding=payloads.text(5);
        const auto payloadSize=payloads.blobSize(8);
        require(payloadSize<=static_cast<std::size_t>(decoded)+65536,"Oversized tile payload");
        require(encoding=="constant" || encoding=="raw" || encoding=="zstd","Unsupported tile encoding");
        // Cover the copied SQL blob, canonical expansion, checksum and bounded
        // decompressor. Destination TileStore separately admits retained bytes.
        std::optional<MemoryAdmission::Reservation> scratch;
        const auto workspace=encoding=="zstd" ? ZSTD_estimateDCtxSize()+1024*1024 : 0;
        if(tiles.memoryAdmission()) scratch.emplace(tiles.memoryAdmission()->require(
            payloadSize+static_cast<std::uint64_t>(decoded)+workspace+256));
        const auto payload=payloads.blob(8);
        QByteArray raw;
        if(encoding=="constant") {
            require(payload.size()==8,"Invalid constant tile"); readPacked(payload);
            raw.resize(decoded); for(qsizetype i=0;i<raw.size();i+=8) std::memcpy(raw.data()+i,payload.constData(),8);
        } else if(encoding=="raw") { require(payload.size()==decoded,"Invalid raw tile length"); raw=payload; }
        else if(encoding=="zstd") {
            require(payload.size()>=4 && payload.left(4)==QByteArray::fromHex("28b52ffd"),"Invalid zstd frame");
            const auto size=static_cast<std::size_t>(payload.size());
            require(ZSTD_findFrameCompressedSize(payload.constData(),size)==size && ZSTD_getDictID_fromFrame(payload.constData(),size)==0,"Invalid or concatenated zstd frame");
            ZSTD_frameHeader frame{};
            require(ZSTD_getFrameHeader(&frame,payload.constData(),size)==0 && frame.frameType==ZSTD_frame
                && frame.windowSize<=1024*1024,"Invalid or oversized zstd frame window");
            auto ctx=std::unique_ptr<ZSTD_DCtx,decltype(&ZSTD_freeDCtx)>(ZSTD_createDCtx(),ZSTD_freeDCtx);
            require(ctx!=nullptr,"Cannot allocate tile decompressor");
            require(!ZSTD_isError(ZSTD_DCtx_setParameter(ctx.get(),ZSTD_d_windowLogMax,20)),"Cannot set tile window limit");
            raw.resize(decoded);
            require(ZSTD_decompressDCtx(ctx.get(),raw.data(),static_cast<std::size_t>(decoded),payload.constData(),size)==static_cast<std::size_t>(decoded),"Invalid compressed tile or decoded length");
        } else fail("Unsupported tile encoding");
        require(hash(raw)==payloads.blob(7),"Tile checksum mismatch");
        if(maskAssets.contains(key)) validateMaskBytes(byteSpan(raw),stop);
        auto tile=tiles.fromCanonical(static_cast<int>(region.width),static_cast<int>(region.height),byteSpan(raw));
        // Complete persistence before retaining the tile in the new snapshot.
        // Spill-enabled loading keeps canonical residency bounded to one row.
        if(tile->spillable()) tile->spill(stop);
        require(map.emplace(coord,std::move(tile)).second,"Duplicate tile coordinate");
    }
    for(auto& [key,asset]:assetMap)
        asset=std::make_shared<const RasterSnapshot>(asset->id,asset->extent,asset->defaultValue,std::move(tileMaps[key]),asset->revision,asset->sourceProfile);
    for(auto& node:nodes) {
        if(node.raster) node.raster=assetMap.at(node.raster->id.text());
        if(node.mask) node.mask=assetMap.at(node.mask->id.text());
    }
    std::map<std::string,std::pair<std::string,std::shared_ptr<const RasterSnapshot>>> renderedAssets;
    std::map<std::string,std::shared_ptr<const RetouchStack>> retainedRetouchStacks;
    for(auto& node:nodes) if(const auto found=retouchJson.find(node.id.text());found!=retouchJson.end()) {
        cancelled(stop);auto stored=readRetouchParameters(found->second,node.raster);
        RasterSnapshot metadata(stored.intermediateId,stored.stack.source->extent,stored.stack.source->defaultValue,{},stored.intermediateRevision,stored.stack.source->sourceProfile);
        const auto intermediateKey=retouchParameters(stored.stack,metadata,nullptr,Sampling::Bilinear);
        std::shared_ptr<const RasterSnapshot> intermediate;
        if(const auto cached=renderedAssets.find(stored.intermediateId.text());cached!=renderedAssets.end()) {
            require(cached->second.first==intermediateKey,"Conflicting retouch intermediate identity");intermediate=cached->second.second;
        } else {
            const auto computed=evaluateRetouchStack(stored.stack,tiles,stop);
            intermediate=std::make_shared<const RasterSnapshot>(stored.intermediateId,computed->extent,computed->defaultValue,computed->tiles,stored.intermediateRevision,computed->sourceProfile);
            renderedAssets.emplace(stored.intermediateId.text(),std::make_pair(intermediateKey,intermediate));
        }
        if(const auto retained=retainedRetouchStacks.find(stored.intermediateId.text());retained!=retainedRetouchStacks.end()) {
            node.retouch=retained->second;
        } else {
            // Parsing is covered by worker scratch; retained vector capacity is
            // charged independently for the entire immutable backing lifetime.
            std::uint64_t bytes=sizeof(RetouchStack)+128;
            bytes+=static_cast<std::uint64_t>(stored.stack.strokes.capacity())*sizeof(RetouchStroke);
            for(const auto& stroke:stored.stack.strokes)
                bytes+=static_cast<std::uint64_t>(stroke.points.capacity())*sizeof(Coordinate);
            std::shared_ptr<MemoryAdmission::Reservation> charge;
            if(tiles.memoryAdmission()) charge=std::make_shared<MemoryAdmission::Reservation>(tiles.memoryAdmission()->require(bytes));
            auto ownedStack=std::shared_ptr<const RetouchStack>(new RetouchStack(std::move(stored.stack)),
                [charge](const RetouchStack* stack){delete stack;});
            if(charge) charge->commit();
            node.retouch=ownedStack;
            retainedRetouchStacks.emplace(stored.intermediateId.text(),std::move(ownedStack));
        }
        node.raster=intermediate;node.sampling=stored.sampling;
        if(!stored.adjustment.empty()) {
            adjustmentJson.emplace(node.id.text(),stored.adjustment);
        }
    }
    for(auto& node:nodes) if(const auto found=adjustmentJson.find(node.id.text());found!=adjustmentJson.end()) {
        cancelled(stop);
        auto stored=readAdjustmentParameters(found->second,node.raster,static_cast<int>(formatVersion));
        require(!assetMap.contains(stored.renderedId.text()),"Derived raster identity conflicts with stored source or mask");
        RasterSnapshot cacheMetadata(stored.renderedId,stored.stack.source->extent,stored.stack.source->defaultValue,{},stored.renderedRevision,stored.stack.source->sourceProfile);
        const auto cacheIdentity=adjustmentParameters(stored.stack,cacheMetadata,Sampling::Bilinear);
        const auto cached=renderedAssets.find(stored.renderedId.text());
        if(cached!=renderedAssets.end()) {
            require(cached->second.first==cacheIdentity,"Conflicting derived raster parameters");
            node.raster=cached->second.second;
        } else {
            const auto computed=evaluateAdjustmentStack(stored.stack,tiles,stop);
            node.raster=std::make_shared<const RasterSnapshot>(stored.renderedId,computed->extent,computed->defaultValue,computed->tiles,stored.renderedRevision,computed->sourceProfile);
            renderedAssets.emplace(stored.renderedId.text(),std::make_pair(cacheIdentity,node.raster));
        }
        node.sampling=stored.sampling;
        node.adjustments=std::make_shared<const AdjustmentStack>(std::move(stored.stack));
    }
    auto document=std::make_shared<const DocumentSnapshot>(docId,static_cast<int>(width),static_cast<int>(height),resolution,std::move(nodes),revision,selection);
    cancelled(stop); require(fileIdentity(path)==before,"Project changed during reading; retry");
    return {document,*before};
}
FileIdentity saveProject(const QString& requestedPath, const DocumentPtr& document, const SaveOptions& options) {
    require(document!=nullptr,"No document to save"); cancelled(options.stop);
    std::map<std::string,std::shared_ptr<const RasterSnapshot>> assets;
    std::map<std::string,std::shared_ptr<const SourceProfile>> profiles;
    struct SavedMask {std::shared_ptr<const RasterSnapshot> raster; bool enabled; Affine transform;};
    std::map<std::string,SavedMask> masks;
    std::set<std::string> maskAssets;
    std::uint64_t colorPixels=0,maskPixels=0;
    std::map<std::string,std::string> derivedParameters;
    std::map<std::string,std::string> serializedParameters;
    std::size_t parameterBytes=2;
    const auto serializedSelection=selectionJson(document->selection,document->width,document->height);
    for(const auto& node:document->layers()) {
        cancelled(options.stop);
        const auto parameters=node.retouch ? retouchParameters(*node.retouch,*node.raster,node.adjustments.get(),node.sampling) : node.adjustments ? adjustmentParameters(*node.adjustments,*node.raster,node.sampling) : layerParameters(node.folder,node.sampling);
        require(parameters.size()<=4*1024*1024-parameterBytes,"Excess layer parameter bytes");
        parameterBytes+=parameters.size();
        serializedParameters.emplace(node.id.text(),parameters); // Validate before creating a replacement file.
        require(QString::fromUtf8(node.name).toUtf8().toStdString()==node.name,"Invalid UTF-8 layer name");
        require(!node.folder || !node.mask,"Folder masks are unsupported");
        if(!node.raster) continue;
        const auto retained=node.retouch ? node.retouch->source : node.adjustments ? node.adjustments->source : node.raster;
        if(node.retouch) {
            const auto& intermediate=node.adjustments ? *node.adjustments->source : *node.raster;
            const auto serialized=retouchParameters(*node.retouch,intermediate,nullptr,Sampling::Bilinear);
            const auto [entry,inserted]=derivedParameters.emplace(intermediate.id.text(),serialized);
            require(inserted||entry->second==serialized,"Conflicting retouch intermediate identity");
        }
        if(node.adjustments) {
            const auto serialized=adjustmentParameters(*node.adjustments,*node.raster,Sampling::Bilinear);
            const auto [entry,inserted]=derivedParameters.emplace(node.raster->id.text(),serialized);
            require(inserted || entry->second==serialized,"Conflicting derived raster identities");
        }
        const auto colorKey=retained->id.text();
        require(!maskAssets.contains(colorKey),"Color and mask asset IDs conflict");
        const auto [color,insertedColor]=assets.emplace(colorKey,retained);
        require(insertedColor || color->second==retained,"Conflicting color asset versions");
        if(insertedColor) colorPixels+=static_cast<std::uint64_t>(retained->extent.width)*static_cast<std::uint64_t>(retained->extent.height);
        if(node.mask) {
            const auto key=node.mask->id.text();
            require(node.mask->extent==node.raster->extent && !node.mask->sourceProfile,"Mask extent or profile is unsupported");
            validateMaskPixel(node.mask->defaultValue);
            require(masks.emplace(node.id.text(),SavedMask{node.mask,node.maskEnabled,node.localToDocument}).second,"Duplicate mask placement identity");
            require(!assets.contains(key) || maskAssets.contains(key),"Color and mask asset IDs conflict");
            const auto [maskAsset,insertedMask]=assets.emplace(key,node.mask);
            require(insertedMask || maskAsset->second==node.mask,"Conflicting mask asset versions");
            maskAssets.insert(key);
            if(insertedMask) maskPixels+=static_cast<std::uint64_t>(node.mask->extent.width)*static_cast<std::uint64_t>(node.mask->extent.height);
        }
        if(const auto& profile=retained->sourceProfile) {
            const auto [found,inserted]=profiles.emplace(profile->id.text(),profile);
            require(inserted || found->second->bytes==profile->bytes,"Conflicting source profile IDs");
        }
    }
    for(const auto& [key,parameters]:derivedParameters) require(!assets.contains(key),"Derived raster identity conflicts with source or mask");
    require(profiles.size()<=16,"Too many source profiles");
    require(colorPixels<=100000000 && maskPixels<=100000000 && masks.size()<=10000 && assets.size()<=20000,"Aggregate color or mask asset limit exceeded");
    std::lock_guard guard(saveMutex); // Serializes this process; sidecar flock coordinates other writers.
    const QFileInfo info(requestedPath);
    require(!info.fileName().isEmpty() && info.fileName()!="." && info.fileName()!="..","Choose a project filename");
    const auto directory=info.absoluteDir().canonicalPath(); require(!directory.isEmpty(),"Save directory does not exist");
    const auto path=directory+"/"+info.fileName();
    Fd dir(::open(QFile::encodeName(directory).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    struct statfs filesystem{}; require(::fstatfs(dir.value,&filesystem)==0,"Cannot inspect save filesystem");
    require(filesystem.f_type!=NFS_SUPER_MAGIC && filesystem.f_type!=CIFS_SUPER_MAGIC && filesystem.f_type!=SMB2_SUPER_MAGIC
        && filesystem.f_type!=SMB_SUPER_MAGIC && filesystem.f_type!=V9FS_MAGIC && filesystem.f_type!=FUSE_SUPER_MAGIC,
        "Save to a local filesystem first; network/FUSE filesystem durability is not supported");
    const auto lockPath=directory+"/."+info.fileName()+".lock";
    Fd lock(::open(QFile::encodeName(lockPath).constData(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));
    require(::flock(lock.value,LOCK_EX|LOCK_NB)==0,"Another process is saving this project");
    require(fileIdentity(path)==options.expected,"Destination changed externally; use Save As or confirm replacement");
    mode_t mode=0600;
    if(options.expected) { struct stat old{}; require(::lstat(QFile::encodeName(path).constData(),&old)==0,"Cannot inspect destination mode"); mode=old.st_mode&0777; }
    QTemporaryFile temporary(directory+"/.compositor-save-XXXXXX");
    require(temporary.open(),"Cannot create sibling temporary project");
    require(::fchmod(temporary.handle(),mode)==0,"Cannot preserve project permissions");
    checkpoint(options,SaveStage::Write);
    {
        Db db(QFile::encodeName(temporary.fileName()),SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW);
        db.exec("PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA cache_size=-4096; PRAGMA mmap_size=0");
        db.exec(projectSchemaV6); db.exec("PRAGMA user_version=6; BEGIN IMMEDIATE");
        {
            Query q(db,"INSERT INTO project(id,format,uuid,width,height,resolution,working_space,saved_revision,required_features_json,selection_json) VALUES(1,'compositor-linux',?,?,?,?,'linear-srgb-extended-v1',?,'[]',?)");
            q.text(1,document->id.text()); q.bind(2,document->width); q.bind(3,document->height); q.real(4,document->resolution); q.bind(5,document->revision); q.text(6,serializedSelection); q.end();
        }
        for(const auto& [key,profile]:profiles) {
            cancelled(options.stop); const auto bytes=bytesOf(profile->bytes); validateProfile(bytes);
            Query q(db,"INSERT INTO profiles VALUES(?,'source',?,?)"); q.text(1,key); q.blob(2,bytes); q.blob(3,hash(bytes)); q.end();
        }
        for(const auto& [key,asset]:assets) {
            cancelled(options.stop); const auto& raster=*asset;
            Query q(db,"INSERT INTO assets VALUES(?,?,?,?,?,?,?,?,?)");
            q.text(1,raster.id.text()); q.bind(2,raster.extent.x); q.bind(3,raster.extent.y); q.bind(4,raster.extent.width); q.bind(5,raster.extent.height);
            q.text(6,maskAssets.contains(key) ? "mask-rgba16f-le" : "rgba16f-le");
            q.blob(7,packedBytes(raster.defaultValue)); q.bind(8,raster.revision); if(raster.sourceProfile) q.text(9,raster.sourceProfile->id.text()); q.end();
        }
        for(const auto& [key,mask]:masks) {
            cancelled(options.stop); Query q(db,"INSERT INTO masks VALUES(?,?,?,1,?,1)");
            q.text(1,key); q.text(2,mask.raster->id.text()); q.bind(3,mask.enabled ? 1 : 0); q.blob(4,matrixBytes(mask.transform)); q.end();
        }
        for(const auto& node:document->layers()) {
            cancelled(options.stop); const std::string type=node.folder ? "folder" : "raster";
            Query q(db,"INSERT INTO layers VALUES(?,?,?,?,?,?,?,?,?,?,?,NULL,?)");
            q.text(1,node.id.text()); if(node.parent) q.text(2,node.parent->text()); q.bind(3,node.siblingOrder); q.text(4,type);
            q.text(5,node.name); q.bind(6,node.visible ? 1 : 0); q.real(7,node.opacity); q.text(8,std::string(blendIdentifier(node.blend)));
            q.blob(9,matrixBytes(node.localToDocument)); if(node.raster) q.text(10,(node.retouch ? node.retouch->source : node.adjustments ? node.adjustments->source : node.raster)->id.text());
            if(node.mask) q.text(11,node.id.text());
            q.text(12,serializedParameters.at(node.id.text())); q.end();
        }
        for(const auto& [key,asset]:assets) {
        const auto& raster=*asset;
        for(const auto& [coord,tile]:raster.tiles) {
            cancelled(options.stop);
            const auto decoded=static_cast<std::size_t>(tile->width())*static_cast<std::size_t>(tile->height())*8;
            const auto capacity=tile->uniform() ? std::size_t{8} : ZSTD_compressBound(decoded);
            const auto workspace=tile->uniform() ? std::size_t{0} : ZSTD_estimateCCtxSize(1);
            require(!ZSTD_isError(workspace),"Cannot bound tile compression workspace");
            std::optional<MemoryAdmission::Reservation> scratch;
            // SQLite TRANSIENT owns a second payload copy until Query retires.
            if(options.memory) scratch.emplace(options.memory->require(decoded+2*capacity+workspace+256));
            const bool restoreDiskOnly=tile->spillable() && tile->spillStatus().spilled;
            struct RetireRead {
                TilePtr tile; bool restore;
                ~RetireRead() {if(restore) try {tile->spill();} catch(...) {}}
            } retirement{tile,restoreDiskOnly};
            QByteArray raw(static_cast<qsizetype>(decoded),'\0');
            {
                const auto lease=tile->read(options.stop);
                lease.copyCanonicalBytes({reinterpret_cast<std::uint8_t*>(raw.data()),decoded},options.stop);
            }
            if(restoreDiskOnly) {tile->spill(options.stop); retirement.restore=false;}
            if(maskAssets.contains(key)) validateMaskBytes(byteSpan(raw),options.stop);
            QByteArray payload; std::string encoding;
            if(tile->uniform()) { payload=raw.left(8); encoding="constant"; }
            else {
                payload.resize(static_cast<qsizetype>(ZSTD_compressBound(static_cast<std::size_t>(raw.size()))));
                const auto length=ZSTD_compress(payload.data(),static_cast<std::size_t>(payload.size()),raw.constData(),static_cast<std::size_t>(raw.size()),1);
                require(!ZSTD_isError(length),"Tile compression failed"); payload.resize(static_cast<qsizetype>(length)); encoding="zstd";
            }
            Query q(db,"INSERT INTO tiles VALUES(?,?,?,?,?,?,?,?,?)"); q.text(1,raster.id.text()); q.bind(2,coord.x); q.bind(3,coord.y);
            q.bind(4,tile->width()); q.bind(5,tile->height()); q.text(6,encoding); q.bind(7,raw.size()); q.blob(8,hash(raw)); q.blob(9,payload); q.end();
            checkpoint(options,SaveStage::Tile);
        }
        }
        checkpoint(options,SaveStage::Commit); db.exec("COMMIT");
        { Query check(db,"PRAGMA foreign_key_check"); require(!check.row(),"Project reference validation failed"); }
        db.close();
    }
    checkpoint(options,SaveStage::FileSync);
    require(::fsync(temporary.handle())==0,"Project file sync failed; previous file retained");
    checkpoint(options,SaveStage::Replace);
    require(fileIdentity(path)==options.expected,"Destination changed during save; previous file retained");
    const auto sourceName=QFile::encodeName(temporary.fileName()),destinationName=QFile::encodeName(path);
    const int replaced=options.expected ? ::rename(sourceName.constData(),destinationName.constData())
        : ::renameat2(AT_FDCWD,sourceName.constData(),AT_FDCWD,destinationName.constData(),RENAME_NOREPLACE);
    require(replaced==0,"Atomic project replacement failed; previous file retained");
    temporary.setAutoRemove(false);
    try {
        // Cancellation after rename must be reported as uncertain durability,
        // not as a pre-replacement failure. The caller keeps the document dirty.
        if(options.checkpoint) options.checkpoint(SaveStage::DirectorySync);
        require(::fsync(dir.value)==0,"Directory sync failed");
        const auto identity=fileIdentity(path); require(identity.has_value(),"Saved project disappeared"); return *identity;
    } catch(...) { fail("Project was replaced, but final durability is uncertain; keep this document open and save again"); }
}
}
