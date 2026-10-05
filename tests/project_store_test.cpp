#include "io/project_store.h"
#include "project_schema.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <sqlite3.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <iostream>
#include <cstring>

using namespace compositor;
void expect(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } expect(rejected,"Expected rejection"); }
QByteArray read(const QString& path) { QFile f(path); expect(f.open(QIODevice::ReadOnly),"Read test file"); return f.readAll(); }
void change(const QString& path, const char* sql) {
    sqlite3* db=nullptr; expect(sqlite3_open(QFile::encodeName(path).constData(),&db)==SQLITE_OK,"Open test database");
    const auto rc=sqlite3_exec(db,sql,nullptr,nullptr,nullptr); sqlite3_close(db); expect(rc==SQLITE_OK,"Mutate test record");
}
void legacyCopy(const QString& source,const QString& destination,const char* authoredSchema=io::projectSchemaV1) {
    sqlite3* raw=nullptr;
    expect(sqlite3_open(QFile::encodeName(destination).constData(),&raw)==SQLITE_OK,"Create authored version 1 fixture");
    auto db=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>(raw,sqlite3_close);
    expect(sqlite3_exec(raw,authoredSchema,nullptr,nullptr,nullptr)==SQLITE_OK,"Create version 1 table definitions");
    sqlite3_stmt* statement=nullptr;
    expect(sqlite3_prepare_v2(raw,"ATTACH DATABASE ? AS fixture",-1,&statement,nullptr)==SQLITE_OK,"Prepare fixture attachment");
    auto query=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>(statement,sqlite3_finalize);
    const auto path=QFile::encodeName(source);
    expect(sqlite3_bind_text(statement,1,path.constData(),static_cast<int>(path.size()),SQLITE_TRANSIENT)==SQLITE_OK,"Bind fixture path");
    expect(sqlite3_step(statement)==SQLITE_DONE,"Attach trusted fixture"); query.reset();
    expect(sqlite3_exec(raw,"BEGIN; INSERT INTO project SELECT id,format,uuid,width,height,resolution,working_space,saved_revision,required_features_json FROM fixture.project; INSERT INTO profiles SELECT * FROM fixture.profiles; INSERT INTO assets SELECT * FROM fixture.assets; INSERT INTO masks SELECT * FROM fixture.masks; INSERT INTO layers SELECT * FROM fixture.layers; INSERT INTO tiles SELECT * FROM fixture.tiles; INSERT INTO preview SELECT * FROM fixture.preview; COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"Copy baseline records into authored version 1 schema");
}
void invalidMaskRGB(const QString& path) {
    sqlite3* raw=nullptr; expect(sqlite3_open(QFile::encodeName(path).constData(),&raw)==SQLITE_OK,"Open mask pixel corruption fixture");
    auto db=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>(raw,sqlite3_close);
    sqlite3_stmt* statement=nullptr;
    expect(sqlite3_prepare_v2(raw,"SELECT tiles.asset_uuid,tiles.tile_x,tiles.tile_y,tiles.width,tiles.height,tiles.payload FROM tiles JOIN assets ON assets.uuid=tiles.asset_uuid WHERE assets.format='mask-rgba16f-le' AND tiles.encoding='constant' LIMIT 1",-1,&statement,nullptr)==SQLITE_OK,"Select constant mask pixel");
    auto query=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>(statement,sqlite3_finalize);
    expect(sqlite3_step(statement)==SQLITE_ROW,"Constant coverage fixture exists");
    const std::string key=reinterpret_cast<const char*>(sqlite3_column_text(statement,0));
    const auto x=sqlite3_column_int64(statement,1),y=sqlite3_column_int64(statement,2);
    const int decoded=sqlite3_column_int(statement,3)*sqlite3_column_int(statement,4)*8;
    QByteArray payload(static_cast<const char*>(sqlite3_column_blob(statement,5)),sqlite3_column_bytes(statement,5));
    expect(payload.size()==8,"Constant coverage is one canonical pixel");
    payload[0]=0; payload[1]=static_cast<char>(0x30); // Finite 0.125 RGB, valid as color but invalid coverage.
    QByteArray expanded(decoded,'\0');
    for(int offset=0;offset<decoded;offset+=8) std::memcpy(expanded.data()+offset,payload.constData(),8);
    const auto checksum=QCryptographicHash::hash(expanded,QCryptographicHash::Sha256); query.reset();
    expect(sqlite3_prepare_v2(raw,"UPDATE tiles SET payload=?,checksum=? WHERE asset_uuid=? AND tile_x=? AND tile_y=?",-1,&statement,nullptr)==SQLITE_OK,"Prepare authenticated invalid coverage");
    query.reset(statement);
    expect(sqlite3_bind_blob(statement,1,payload.constData(),8,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_blob(statement,2,checksum.constData(),32,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_text(statement,3,key.data(),static_cast<int>(key.size()),SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_int64(statement,4,x)==SQLITE_OK && sqlite3_bind_int64(statement,5,y)==SQLITE_OK && sqlite3_step(statement)==SQLITE_DONE,"Install checksum-valid nonzero mask RGB");
}
void oversizedWindow(const QString& path) {
    sqlite3* raw=nullptr; expect(sqlite3_open(QFile::encodeName(path).constData(),&raw)==SQLITE_OK,"Open oversized window fixture");
    auto db=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>(raw,sqlite3_close);
    sqlite3_stmt* statement=nullptr;
    expect(sqlite3_prepare_v2(raw,"SELECT payload,decoded_size FROM tiles WHERE encoding='zstd' LIMIT 1",-1,&statement,nullptr)==SQLITE_OK,"Read compressed fixture");
    auto query=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>(statement,sqlite3_finalize);
    expect(sqlite3_step(statement)==SQLITE_ROW,"Compressed fixture exists");
    QByteArray payload(static_cast<const char*>(sqlite3_column_blob(statement,0)),sqlite3_column_bytes(statement,0));
    QByteArray original(sqlite3_column_int(statement,1),'\0');
    expect(ZSTD_decompress(original.data(),static_cast<std::size_t>(original.size()),payload.constData(),static_cast<std::size_t>(payload.size()))==static_cast<std::size_t>(original.size()),"Original fixture decompresses");
    query.reset();
    expect(payload.size()>5 && (static_cast<unsigned char>(payload[4])&0xc0)!=0,"Fixture uses multibyte frame content size");
    if((static_cast<unsigned char>(payload[4])&0x20)!=0) {
        payload[4]=static_cast<char>(static_cast<unsigned char>(payload[4])&~0x20u);
        payload.insert(5,QByteArray(1,static_cast<char>(0x58))); // WindowLog=21, two MiB.
    } else payload[5]=static_cast<char>(0x58);
    ZSTD_frameHeader frame{};
    expect(ZSTD_getFrameHeader(&frame,payload.constData(),static_cast<std::size_t>(payload.size()))==0 && frame.windowSize==2*1024*1024,"Fixture header declares oversized window");
    QByteArray decoded(original.size(),'\0');
    expect(ZSTD_decompress(decoded.data(),static_cast<std::size_t>(decoded.size()),payload.constData(),static_cast<std::size_t>(payload.size()))==static_cast<std::size_t>(decoded.size()) && decoded==original,"Oversized-window frame remains decodable and checksum-equivalent");
    expect(sqlite3_prepare_v2(raw,"UPDATE tiles SET payload=? WHERE encoding='zstd'",-1,&statement,nullptr)==SQLITE_OK,"Prepare oversized payload");
    query.reset(statement);
    expect(sqlite3_bind_blob(statement,1,payload.constData(),static_cast<int>(payload.size()),SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_step(statement)==SQLITE_DONE,"Install oversized frame");
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        expect(argc>1,"Inject an absolute disk-backed fixture directory");
        QTemporaryDir dir; expect(dir.isValid(),"Temporary directory");
        const auto path=dir.filePath("test.cproj"); engine::TileStore tiles(16*1024*1024);
        auto blank=engine::blankDocument(257,258); engine::DocumentHistory history(blank);
        auto edit=history.begin();
        std::vector<engine::Pixel> pixels(256*256);
        for(std::size_t i=0;i<pixels.size();++i) pixels[i]={static_cast<float>(i%100)/100,-.2f,1.5f,.75f};
        edit.write(tiles,{0,0},pixels); edit.replace({1,1},tiles.constant(1,2,{0,.5f,0,.5f})); history.commit(edit,"Pixels");
        const auto document=history.current();
        const auto identity=io::saveProject(path,document);
        const auto loaded=io::loadProject(path,tiles);
        expect(loaded.identity==identity && loaded.document->id==document->id && loaded.document->singleLayer().id==document->singleLayer().id,"IDs and file identity preserved");
        expect(loaded.document->singleLayer().raster->id==document->singleLayer().raster->id && loaded.document->revision==document->revision,"Asset and revision preserved");
        for(const auto& [coord,tile]:document->singleLayer().raster->tiles) expect(loaded.document->singleLayer().raster->tiles.at(coord)->canonicalBytes()==tile->canonicalBytes(),"Canonical bytes preserved");
        const auto legacyPath=dir.filePath("legacy.cproj");
        legacyCopy(path,legacyPath);
        const auto legacy=io::loadProject(legacyPath,tiles).document;
        expect(legacy->singleLayer().raster->tiles.at({0,0})->canonicalBytes()==document->singleLayer().raster->tiles.at({0,0})->canonicalBytes(),"Version 1 pixels remain exact");
        io::saveProject(legacyPath,legacy,{io::fileIdentity(legacyPath)});
        expect(io::loadProject(legacyPath,tiles).document->id==document->id,"Version 1 migrates on save");
        const auto layeredPath=dir.filePath("layered.cproj");
        auto nodes=document->layers();
        engine::LayerNode folder{engine::Id::generate()}; folder.folder=true; folder.siblingOrder=1; folder.name="Folder";
        auto copy=nodes[0]; copy.id=engine::Id::generate(); copy.parent=folder.id; copy.opacity=.375f; copy.blend=engine::BlendMode::ColorDodge;
        copy.name="Transformed copy"; copy.localToDocument=engine::Affine::placement(11,-23,128.5,129,257,258,35,true,false);
        // Deliberately store child before parent; deferred references are valid.
        nodes.push_back(copy); nodes.push_back(folder);
        auto layered=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,300,nodes,12);
        io::saveProject(layeredPath,layered); const auto roundtrip=io::loadProject(layeredPath,tiles).document;
        expect(roundtrip->layers().size()==3 && roundtrip->resolution==300 && roundtrip->revision==12,"Layered metadata roundtrip");
        const auto& restored=roundtrip->layer(copy.id);
        expect(restored.parent==folder.id && restored.name==copy.name && restored.opacity==copy.opacity && restored.blend==copy.blend
            && restored.localToDocument==copy.localToDocument,"Layer placement and appearance preserved");
        expect(restored.raster==roundtrip->layer(nodes[0].id).raster,"Shared asset remains shared after reopen");
        for(const auto& [coord,tile]:copy.raster->tiles) expect(restored.raster->tiles.at(coord)->canonicalBytes()==tile->canonicalBytes(),"Layered bytes exact");
        auto extendedNodes=nodes; extendedNodes.back().opacity=.5f;
        extendedNodes.front().blend=engine::BlendMode::SoftLight;
        const auto extendedPath=dir.filePath("extended.cproj");
        const auto extended=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,300,extendedNodes,15);
        io::saveProject(extendedPath,extended);
        const auto extendedRoundtrip=io::loadProject(extendedPath,tiles).document;
        expect(extendedRoundtrip->layer(folder.id).opacity==.5f && extendedRoundtrip->layer(nodes.front().id).blend==engine::BlendMode::SoftLight,"Version 2 persists folder opacity and new blends");
        const auto genuineV2Path=dir.filePath("legacy-v2.cproj");
        legacyCopy(extendedPath,genuineV2Path,io::projectSchemaV2);
        const auto genuineV2=io::loadProject(genuineV2Path,tiles).document;
        expect(!genuineV2->selection && !genuineV2->layer(nodes.front().id).mask && genuineV2->layer(folder.id).opacity==.5f && genuineV2->layer(nodes.front().id).blend==engine::BlendMode::SoftLight,"Genuine version 2 appearance remains unchanged");
        io::saveProject(genuineV2Path,genuineV2,{io::fileIdentity(genuineV2Path)});
        const auto migratedV2=io::loadProject(genuineV2Path,tiles).document;
        expect(migratedV2->id==genuineV2->id && migratedV2->revision==genuineV2->revision && migratedV2->layer(folder.id).opacity==.5f && migratedV2->layer(nodes.front().id).blend==engine::BlendMode::SoftLight && !migratedV2->selection && !migratedV2->layer(nodes.front().id).mask,"Version 2 migrates to current writer without changing appearance or adding state");
        change(extendedPath,"PRAGMA user_version=1");
        rejects([&]{io::loadProject(extendedPath,tiles);});
        change(extendedPath,"UPDATE layers SET blend_mode='normal'");
        rejects([&]{io::loadProject(extendedPath,tiles);}); // Folder opacity alone needs version 2.
        change(extendedPath,"UPDATE layers SET opacity=1 WHERE type='folder'");
        rejects([&]{io::loadProject(extendedPath,tiles);}); // Version 2 DDL cannot masquerade as version 1.
        const auto legacyLayersPath=dir.filePath("legacy-layers.cproj");
        legacyCopy(extendedPath,legacyLayersPath);
        expect(io::loadProject(legacyLayersPath,tiles).document->layers().size()==3,"Authored version 1 baseline graph remains readable");
        for(const char* sql:{"UPDATE layers SET parent_uuid='00000000-0000-0000-0000-000000000000' WHERE parent_uuid IS NOT NULL",
            "UPDATE layers SET sibling_order=sibling_order+2", "UPDATE layers SET parameters_json='{}' WHERE type='folder'",
            "DELETE FROM layers", "UPDATE layers SET mask_uuid='00000000-0000-0000-0000-000000000000'"}) {
            const auto bad=dir.filePath("bad-layered.cproj"); expect(QFile::copy(layeredPath,bad),"Copy layered fixture"); change(bad,sql);
            const auto memory=tiles.usedBytes(); rejects([&]{io::loadProject(bad,tiles);}); expect(memory==tiles.usedBytes(),"Failed layered load releases tiles");
            expect(QFile::remove(bad),"Remove owned layered fixture");
        }
        engine::TileMap coverageTiles;
        coverageTiles.emplace(engine::TileCoord{0,0},tiles.constant(256,256,{0,0,0,.25f}));
        const std::array<engine::Pixel,2> edgeCoverage{engine::Pixel{0,0,0,.5f},engine::Pixel{0,0,0,.75f}};
        coverageTiles.emplace(engine::TileCoord{1,1},tiles.create(1,2,edgeCoverage));
        auto coverage=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),document->singleLayer().raster->extent,
            engine::pack({0,0,0,1}),std::move(coverageTiles),3);
        auto maskNodes=document->layers(); maskNodes.front().mask=coverage;
        engine::Selection selected{{1,2,40,20},engine::SelectionShape::Ellipse,true};
        auto selectedMasked=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,72,maskNodes,16,selected);
        engine::DocumentHistory maskHistory(selectedMasked);
        auto disable=maskHistory.begin(); maskNodes.front().maskEnabled=false; disable.setLayers(maskNodes);
        expect(maskHistory.commit(disable,"Disable mask") && !maskHistory.current()->singleLayer().maskEnabled,"Mask disable is an immutable command");
        expect(maskHistory.undo() && maskHistory.current()==selectedMasked && maskHistory.redo(),"Mask command undo/redo preserves snapshots");
        const auto maskPath=dir.filePath("selection-mask.cproj");
        io::saveProject(maskPath,maskHistory.current()); const auto restoredMask=io::loadProject(maskPath,tiles).document;
        expect(restoredMask->selection==selectedMasked->selection && !restoredMask->singleLayer().maskEnabled && restoredMask->singleLayer().mask->id==coverage->id,"Selection and disabled mask metadata roundtrip");
        expect(restoredMask->singleLayer().mask->defaultValue==coverage->defaultValue,"Mask default coverage exact");
        for(const auto& [coord,tile]:coverage->tiles)
            expect(restoredMask->singleLayer().mask->tiles.at(coord)->canonicalBytes()==tile->canonicalBytes(),"Mask canonical coverage exact");
        auto sharedMaskNodes=maskHistory.current()->layers();
        auto duplicateMaskLayer=sharedMaskNodes.front(); duplicateMaskLayer.id=engine::Id::generate();
        duplicateMaskLayer.siblingOrder=1; duplicateMaskLayer.maskEnabled=true; duplicateMaskLayer.localToDocument.tx=12;
        sharedMaskNodes.push_back(duplicateMaskLayer);
        const auto sharedMaskDocument=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,72,sharedMaskNodes,18,selected);
        const auto sharedMaskPath=dir.filePath("shared-mask.cproj"); io::saveProject(sharedMaskPath,sharedMaskDocument);
        const auto sharedMaskRoundtrip=io::loadProject(sharedMaskPath,tiles).document;
        const auto& disabledShared=sharedMaskRoundtrip->layer(sharedMaskNodes.front().id);
        const auto& movedShared=sharedMaskRoundtrip->layer(duplicateMaskLayer.id);
        expect(disabledShared.mask==movedShared.mask && !disabledShared.maskEnabled && movedShared.maskEnabled && movedShared.localToDocument==duplicateMaskLayer.localToDocument,"Shared mask pixels retain independent per-layer placement and enable state");
        for(const char* sql:{
            "UPDATE layers SET mask_uuid=(SELECT mask_uuid FROM layers ORDER BY sibling_order LIMIT 1),transform=(SELECT transform FROM layers ORDER BY sibling_order LIMIT 1); DELETE FROM masks WHERE uuid NOT IN (SELECT mask_uuid FROM layers)",
            "UPDATE masks SET transform=(SELECT transform FROM layers WHERE sibling_order=1) WHERE uuid=(SELECT mask_uuid FROM layers WHERE sibling_order=0)"}) {
            const auto badShared=dir.filePath("bad-shared-mask.cproj"); expect(QFile::copy(sharedMaskPath,badShared),"Copy shared placement corruption fixture"); change(badShared,sql);
            const auto memory=tiles.usedBytes(); rejects([&]{io::loadProject(badShared,tiles);});
            expect(tiles.usedBytes()==memory,"Shared placement or valid linked-transform mismatch fails without leaked tiles");
            expect(QFile::remove(badShared),"Remove owned bad shared-mask fixture");
        }
        auto emptySelection=maskHistory.begin(); emptySelection.setSelection(engine::Selection{{257,258,0,0},engine::SelectionShape::Rectangle,false});
        expect(maskHistory.commit(emptySelection,"Empty selection"),"Explicit empty selection is a command");
        io::saveProject(maskPath,maskHistory.current(),{io::fileIdentity(maskPath)});
        const auto emptySelected=io::loadProject(maskPath,tiles).document;
        expect(emptySelected->selection && emptySelected->selection->bounds.width==0 && emptySelected->selection->coverage(1,1)==0,"Explicit empty selection differs from no selection");
        expect(maskHistory.undo() && maskHistory.current()->selection==selectedMasked->selection && maskHistory.redo(),"Selection command undo/redo exact");
        auto clearSelection=maskHistory.begin(); clearSelection.setSelection({});
        expect(maskHistory.commit(clearSelection,"Clear selection"),"Clear selection command");
        io::saveProject(maskPath,maskHistory.current(),{io::fileIdentity(maskPath)});
        expect(!io::loadProject(maskPath,tiles).document->selection,"No selection remains distinct on reopen");
        const auto maskBefore=read(maskPath); const auto maskIdentity=io::fileIdentity(maskPath);
        rejects([&] {
            engine::TileMap invalidCoverage;
            invalidCoverage.emplace(engine::TileCoord{0,0},tiles.constant(256,256,{.125f,0,0,.5f}));
            auto invalidMask=std::make_shared<const engine::RasterSnapshot>(engine::Id::generate(),coverage->extent,
                engine::pack({0,0,0,1}),std::move(invalidCoverage));
            auto invalidNodes=document->layers(); invalidNodes.front().mask=invalidMask;
            auto invalidDocument=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,72,invalidNodes,20);
            io::saveProject(maskPath,invalidDocument,{maskIdentity});
        });
        expect(read(maskPath)==maskBefore && io::fileIdentity(maskPath)==maskIdentity,"Invalid coverage save preserves exact prior project");
        for(const char* sql:{
            "UPDATE project SET selection_json='{\"bounds\":[\"0\",\"0\",\"1\",\"1\"],\"featherRadius\":257,\"inverted\":false,\"shape\":\"rectangle\",\"version\":2}'",
            "UPDATE project SET selection_json='{\"bounds\":[\"0\",\"0\",\"1\",\"1\"],\"featherRadius\":-1,\"inverted\":false,\"shape\":\"rectangle\",\"version\":2}'",
            "UPDATE project SET selection_json='{\"bounds\":[\"0\",\"0\",\"1\",\"1\"],\"featherRadius\":0,\"inverted\":false,\"shape\":\"triangle\",\"version\":2}'",
            "UPDATE project SET selection_json='{\"bounds\":[\"0\",\"0\",\"1\",\"1\"],\"extra\":true,\"featherRadius\":0,\"inverted\":false,\"shape\":\"rectangle\",\"version\":2}'",
            "UPDATE project SET selection_json='{\"version\":1,\"shape\":\"triangle\",\"bounds\":[0,0,1,1],\"inverted\":false}'",
            "UPDATE project SET selection_json='{\"version\":1,\"shape\":\"rectangle\",\"bounds\":[0,0,1.5,1],\"inverted\":false}'",
            "UPDATE project SET selection_json='{\"version\":1,\"shape\":\"rectangle\",\"bounds\":[0,0,1,1],\"inverted\":0}'",
            "UPDATE project SET selection_json='{\"version\":1,\"shape\":\"rectangle\",\"bounds\":[0,0,1,1],\"inverted\":false,\"inverted\":true}'",
            "UPDATE project SET selection_json='{\"version\":1,\"shape\":\"rectangle\",\"bounds\":[0,0,9223372036854775808,1],\"inverted\":false}'",
            "UPDATE masks SET linked=0", "UPDATE masks SET exterior_coverage=0", "UPDATE masks SET transform=zeroblob(72)",
            "UPDATE assets SET origin_x=1 WHERE format='mask-rgba16f-le'",
            "UPDATE assets SET default_value=x'003c00000000003c' WHERE format='mask-rgba16f-le'",
            "UPDATE assets SET default_value=x'0000000000000040' WHERE format='mask-rgba16f-le'",
            "UPDATE layers SET mask_uuid=NULL", "UPDATE layers SET type='folder',asset_uuid=NULL"}) {
            const auto bad=dir.filePath("bad-mask.cproj"); expect(QFile::copy(maskPath,bad),"Copy mask corruption fixture"); change(bad,sql);
            const auto memory=tiles.usedBytes(); rejects([&]{io::loadProject(bad,tiles);});
            expect(tiles.usedBytes()==memory,"Failed mask load releases all staged color and mask tiles"); expect(QFile::remove(bad),"Remove owned bad mask fixture");
        }
        const auto invalidMaskPixelsPath=dir.filePath("bad-mask-pixels.cproj");
        expect(QFile::copy(maskPath,invalidMaskPixelsPath),"Copy mask pixel fixture"); invalidMaskRGB(invalidMaskPixelsPath);
        const auto beforeInvalidMask=tiles.usedBytes(); rejects([&]{io::loadProject(invalidMaskPixelsPath,tiles);});
        expect(tiles.usedBytes()==beforeInvalidMask,"Checksum-valid invalid mask RGB aborts all staged payloads");
        const auto emptyPath=dir.filePath("empty.cproj");
        auto empty=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,72,std::vector<engine::LayerNode>{},13);
        io::saveProject(emptyPath,empty); expect(io::loadProject(emptyPath,tiles).document->layers().empty(),"Empty layer graph roundtrip");
        for(auto mode:{engine::Sampling::Nearest,engine::Sampling::Lanczos}) {
            auto sampled=nodes;sampled[0].sampling=mode;
            auto filtered=std::make_shared<const engine::DocumentSnapshot>(document->id,257,258,72,sampled,14);
            io::saveProject(layeredPath,filtered,{io::fileIdentity(layeredPath)});
            expect(io::loadProject(layeredPath,tiles).document->layer(sampled[0].id).sampling==mode,"Layer sampling lost during save/reopen");
        }
        const auto badSampling=dir.filePath("bad-sampling.cproj");expect(QFile::copy(layeredPath,badSampling),"Copy sampling fixture");
        change(badSampling,"UPDATE layers SET parameters_json='{\"version\":1,\"kind\":\"raster\",\"values\":{\"sampling\":\"unknown\"}}' WHERE type='raster'");
        rejects([&]{io::loadProject(badSampling,tiles);});
        history.markSaved(document); expect(!history.dirty(),"Successful save marks captured revision");
        const auto original=read(path);
        for(auto stage:{io::SaveStage::Write,io::SaveStage::Commit,io::SaveStage::FileSync,io::SaveStage::Replace}) {
            io::SaveOptions options{identity,{},[stage](io::SaveStage reached){ if(reached==stage) throw std::runtime_error("Injected save failure"); }};
            rejects([&]{io::saveProject(path,blank,options);});
            expect(read(path)==original && io::fileIdentity(path)==identity,"Failed save preserves exact prior file");
        }
        // Tile-sized scratch is admitted and retired for every streamed row.
        auto admitted=std::make_shared<MemoryAdmission>(8*1024*1024,0,[]{return MemorySample{64*1024*1024,0};});
        const auto streamedPath=dir.filePath("streamed.cproj");
        io::SaveOptions streamed; streamed.memory=admitted;
        unsigned streamedTiles=0;
        streamed.checkpoint=[&](io::SaveStage stage){if(stage==io::SaveStage::Tile) ++streamedTiles;};
        const auto streamedIdentity=io::saveProject(streamedPath,document,streamed);
        expect(streamedTiles==document->singleLayer().raster->tiles.size(),"Each tile is streamed separately");
        expect(admitted->snapshot().pendingCpu==0 && admitted->snapshot().committedCpu==0,"Streamed save retires scratch");
        engine::TileStore admittedTiles(8*1024*1024,admitted);
        {
            const auto saved=io::loadProject(streamedPath,admittedTiles).document;
            for(const auto& [coord,tile]:document->singleLayer().raster->tiles)
                expect(saved->singleLayer().raster->tiles.at(coord)->canonicalBytes()==tile->canonicalBytes(),"Admitted streamed bytes exact");
        }
        expect(admittedTiles.usedBytes()==0 && admitted->snapshot().pendingCpu==0 && admitted->snapshot().committedCpu==0,"Streamed load retires all staged bytes");
        QTemporaryDir spillDir(QString::fromLocal8Bit(argv[1])+"/project-spill-XXXXXX");
        expect(spillDir.isValid(),"Injected spill fixture directory");
        io::SpillLimits spillLimits; spillLimits.maxBytes=1024*1024; spillLimits.maxPayloadBytes=65536;
        spillLimits.maxEntries=32; spillLimits.minFreeBytes=0;
        auto spillStore=std::make_shared<io::SpillStore>(spillDir.path().toStdString(),spillLimits);
        engine::TileStore spillTiles(1024*1024,admitted,spillStore);
        engine::DocumentHistory spillHistory(engine::blankDocument(513,8)); auto spillEdit=spillHistory.begin();
        for(int x=0;x<3;++x) {
            const int w=x==2 ? 1 : 256; std::vector<engine::Pixel> small(static_cast<std::size_t>(w)*8);
            for(std::size_t i=0;i<small.size();++i) small[i]={float(i%11)/11,.2f,.1f,1};
            auto tile=spillTiles.create(w,8,small); spillEdit.replace({x,0},tile); tile->spill();
        }
        spillHistory.commit(spillEdit,"Disk-backed pixels"); const auto spillDocument=spillHistory.current();
        const auto spillPath=dir.filePath("disk-backed.cproj"); io::SaveOptions spillSave; spillSave.memory=admitted;
        io::saveProject(spillPath,spillDocument,spillSave);
        for(const auto& [coord,tile]:spillDocument->singleLayer().raster->tiles)
            expect(!tile->spillStatus().resident && tile->spillStatus().spilled,"Saving retires each rehydrated source tile");
        {
            const auto restoredSpill=io::loadProject(spillPath,spillTiles).document;
            for(const auto& [coord,tile]:restoredSpill->singleLayer().raster->tiles)
                expect(!tile->spillStatus().resident && tile->spillStatus().spilled,"Loading spills each tile before retaining the next");
            for(const auto& [coord,tile]:restoredSpill->singleLayer().raster->tiles) {
                const auto source=spillDocument->singleLayer().raster->tiles.at(coord);
                expect(tile->canonicalBytes()==source->canonicalBytes(),"Disk-backed streamed roundtrip is exact");
                tile->spill(); source->spill();
            }
        }
        auto denied=std::make_shared<MemoryAdmission>(32,0,[]{return MemorySample{64*1024*1024,0};});
        io::SaveOptions denySave; denySave.expected=streamedIdentity; denySave.memory=denied;
        const auto streamedOriginal=read(streamedPath);
        rejects([&]{io::saveProject(streamedPath,document,denySave);});
        expect(read(streamedPath)==streamedOriginal && io::fileIdentity(streamedPath)==streamedIdentity,"Scratch denial preserves destination");
        engine::TileStore deniedTiles(8*1024*1024,denied);
        rejects([&]{io::loadProject(streamedPath,deniedTiles);});
        expect(deniedTiles.usedBytes()==0 && denied->snapshot().pendingCpu==0,"Load denial releases scratch");
        streamed.expected=streamedIdentity;
        streamed.checkpoint=[](io::SaveStage stage){if(stage==io::SaveStage::Tile) throw std::runtime_error("Injected tile-stream failure");};
        rejects([&]{io::saveProject(streamedPath,document,streamed);});
        expect(read(streamedPath)==streamedOriginal && admitted->snapshot().pendingCpu==0,"Tile-stream failure rolls back and retires scratch");
        std::stop_source streamingStop; streamed.stop=streamingStop.get_token();
        streamed.checkpoint=[&](io::SaveStage stage){if(stage==io::SaveStage::Tile) streamingStop.request_stop();};
        rejects([&]{io::saveProject(streamedPath,document,streamed);});
        expect(read(streamedPath)==streamedOriginal && admitted->snapshot().pendingCpu==0,"Mid-stream cancellation preserves destination");
        rejects([&]{io::saveProject(path,blank);}); // Existing file must be explicitly expected.
        std::stop_source stop; stop.request_stop();
        rejects([&]{io::loadProject(path,tiles,stop.get_token());});
        rejects([&]{io::saveProject(path,blank,{identity,stop.get_token(),{}});});
        const auto replaced=io::saveProject(path,blank,{identity});
        expect(io::loadProject(path,tiles).document->revision==0,"Successful atomic replacement");
        rejects([&]{io::saveProject(path,document,{identity});}); // Stale identity.
        auto current=io::saveProject(path,document,{replaced});
        for(const char* sql:{"UPDATE tiles SET checksum=zeroblob(32)",
            "UPDATE tiles SET decoded_size=8", "UPDATE tiles SET payload=x'28b52ffd' WHERE encoding='zstd'",
            "UPDATE tiles SET payload=CAST(payload||x'00' AS BLOB) WHERE encoding='zstd'",
            "UPDATE project SET required_features_json='[\"future\"]'", "PRAGMA user_version=99",
            "CREATE VIEW surprise AS SELECT * FROM tiles", "UPDATE layers SET parameters_json='{}'",
            "UPDATE assets SET origin_x=9223372036854775807", "UPDATE layers SET transform=zeroblob(72)"}) {
            const auto bad=dir.filePath("bad.cproj"); expect(QFile::copy(path,bad),"Copy malformed fixture"); change(bad,sql);
            const auto memory=tiles.usedBytes(); rejects([&]{io::loadProject(bad,tiles);});
            expect(tiles.usedBytes()==memory,"Failed load releases staged tiles"); expect(QFile::remove(bad),"Remove owned fixture");
        }
        const auto oversized=dir.filePath("oversized-window.cproj");
        expect(QFile::copy(path,oversized),"Copy oversized-window fixture"); oversizedWindow(oversized);
        const auto beforeOversized=tiles.usedBytes(); rejects([&]{io::loadProject(oversized,tiles);});
        expect(tiles.usedBytes()==beforeOversized,"Oversized frame rejection releases staged tiles");
        const auto truncated=dir.filePath("short.cproj");
        { QFile f(truncated); expect(f.open(QIODevice::WriteOnly),"Truncated fixture"); f.write(original.left(200)); }
        rejects([&]{io::loadProject(truncated,tiles);});
        const auto linked=dir.filePath("link.cproj"); expect(QFile::link(path,linked),"Create owned symlink fixture");
        rejects([&]{io::loadProject(linked,tiles);}); rejects([&]{io::saveProject(linked,blank);});
        rejects([&]{io::saveProject(path,blank,{current,{},[](io::SaveStage stage){ if(stage==io::SaveStage::DirectorySync) throw std::runtime_error("sync"); }});});
        expect(io::loadProject(path,tiles).document->revision==0,"Post-rename uncertainty explicitly permits new file");
        std::cout<<"native roundtrip, corruption rejection, cancellation, identity and atomic-save failure tests passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
