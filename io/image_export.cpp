#include "io/image_export.h"
#include "rendering/layer_sampler.h"
#include <QFileInfo>
#include <QSaveFile>
#include <png.h>
#include <jpeglib.h>
#include <jerror.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <new>
#include <utility>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace compositor::io {
namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
void checkpoint(const ExportOptions& options,ExportStage stage) {
    if(options.stop.stop_requested()) throw std::runtime_error("Image export cancelled");
    if(options.checkpoint) options.checkpoint(stage);
    if(options.stop.stop_requested()) throw std::runtime_error("Image export cancelled");
}
struct alignas(std::max_align_t) Allocation {
    MemoryAdmission::Reservation reservation;
    Allocation* next=nullptr;
    int pool=0;
    explicit Allocation(MemoryAdmission::Reservation claim):reservation(std::move(claim)) {}
};
struct Codec {
    std::shared_ptr<MemoryAdmission> memory;
    QSaveFile& file;
    Allocation* allocations=nullptr;
    std::jmp_buf jump;
    char error[256]{};
    void (*jpegFreePool)(j_common_ptr,int)=nullptr;
    void (*jpegDestroy)(j_common_ptr)=nullptr;
    unsigned count=0;
    Codec(std::shared_ptr<MemoryAdmission> admission,QSaveFile& output):memory(std::move(admission)),file(output) {}
    ~Codec() {freePool(-1);}
    void* allocate(std::size_t bytes,int pool) noexcept {
        void* raw=nullptr;
        try {
            if(count>=4096 || bytes>std::numeric_limits<std::size_t>::max()-sizeof(Allocation)-64)
                throw std::length_error("Export codec allocation bound exceeded");
            auto claim=memory->require(bytes+sizeof(Allocation)+64);
            raw=std::malloc(bytes+sizeof(Allocation));
            if(!raw) throw std::bad_alloc();
            auto* entry=new(raw) Allocation(std::move(claim));entry->reservation.commit();
            entry->pool=pool;entry->next=allocations;allocations=entry;++count;
            return entry+1;
        } catch(const std::exception& failure) {
            std::free(raw);std::snprintf(error,sizeof(error),"%s",failure.what());return nullptr;
        }
    }
    void freeOne(void* data) noexcept {
        if(!data) return;
        auto* target=static_cast<Allocation*>(data)-1;
        auto** link=&allocations;
        while(*link && *link!=target) link=&(*link)->next;
        if(!*link) return;
        *link=target->next;auto claim=std::move(target->reservation);target->~Allocation();std::free(target);--count;
    }
    void freePool(int pool) noexcept {
        auto** link=&allocations;
        while(*link) {
            auto* entry=*link;
            if(pool<0 || entry->pool==pool) {
                *link=entry->next;auto claim=std::move(entry->reservation);entry->~Allocation();std::free(entry);--count;
            } else link=&entry->next;
        }
    }
};
void pngError(png_structp png,const char* message) {
    auto& codec=*static_cast<Codec*>(png_get_error_ptr(png));
    if(!codec.error[0]) std::snprintf(codec.error,sizeof(codec.error),"%s",message);
    std::longjmp(codec.jump,1);
}
void pngWarning(png_structp,const char*) {}
png_voidp pngAllocate(png_structp png,png_alloc_size_t bytes) {
    return static_cast<Codec*>(png_get_mem_ptr(png))->allocate(bytes,0);
}
void pngFree(png_structp png,png_voidp data) {static_cast<Codec*>(png_get_mem_ptr(png))->freeOne(data);}
void pngWrite(png_structp png,png_bytep data,png_size_t bytes) {
    auto& codec=*static_cast<Codec*>(png_get_io_ptr(png));
    if(codec.file.write(reinterpret_cast<const char*>(data),static_cast<qint64>(bytes))!=static_cast<qint64>(bytes)) png_error(png,"Cannot write PNG export");
}
void pngFlush(png_structp) {}
struct JpegError {jpeg_error_mgr manager;Codec* codec;};
void jpegError(j_common_ptr jpeg) {
    auto& error=*reinterpret_cast<JpegError*>(jpeg->err);
    if(!error.codec->error[0]) (*jpeg->err->format_message)(jpeg,error.codec->error);
    std::longjmp(error.codec->jump,1);
}
void* jpegSmall(j_common_ptr jpeg,int pool,std::size_t bytes) {
    auto& codec=*static_cast<Codec*>(jpeg->client_data);auto* result=codec.allocate(bytes,pool);
    if(!result) ERREXIT(jpeg,JERR_OUT_OF_MEMORY);
    return result;
}
void* jpegLarge(j_common_ptr jpeg,int pool,std::size_t bytes) {return jpegSmall(jpeg,pool,bytes);}
void jpegFreePool(j_common_ptr jpeg,int pool) {
    auto& codec=*static_cast<Codec*>(jpeg->client_data);
    codec.jpegFreePool(jpeg,pool);codec.freePool(pool);
}
void jpegDestroy(j_common_ptr jpeg) {
    auto& codec=*static_cast<Codec*>(jpeg->client_data);codec.jpegDestroy(jpeg);
}
jvirt_sarray_ptr jpegVirtualSamples(j_common_ptr jpeg,int,boolean,JDIMENSION,JDIMENSION,JDIMENSION) {
    ERREXIT(jpeg,JERR_BAD_STATE);return nullptr;
}
jvirt_barray_ptr jpegVirtualBlocks(j_common_ptr jpeg,int,boolean,JDIMENSION,JDIMENSION,JDIMENSION) {
    ERREXIT(jpeg,JERR_BAD_STATE);return nullptr;
}
struct JpegDestination {jpeg_destination_mgr manager;Codec* codec;std::array<JOCTET,16384> bytes;};
void jpegInitDestination(j_compress_ptr jpeg) {
    auto& destination=*reinterpret_cast<JpegDestination*>(jpeg->dest);
    destination.manager.next_output_byte=destination.bytes.data();destination.manager.free_in_buffer=destination.bytes.size();
}
boolean jpegEmptyDestination(j_compress_ptr jpeg) {
    auto& destination=*reinterpret_cast<JpegDestination*>(jpeg->dest);
    if(destination.codec->file.write(reinterpret_cast<const char*>(destination.bytes.data()),destination.bytes.size())!=static_cast<qint64>(destination.bytes.size())) ERREXIT(jpeg,JERR_FILE_WRITE);
    jpegInitDestination(jpeg);return TRUE;
}
void jpegEndDestination(j_compress_ptr jpeg) {
    auto& destination=*reinterpret_cast<JpegDestination*>(jpeg->dest);
    const auto bytes=destination.bytes.size()-destination.manager.free_in_buffer;
    if(destination.codec->file.write(reinterpret_cast<const char*>(destination.bytes.data()),static_cast<qint64>(bytes))!=static_cast<qint64>(bytes)) ERREXIT(jpeg,JERR_FILE_WRITE);
}
std::uint8_t code(float linear) {
    return static_cast<std::uint8_t>(std::lround(255*std::clamp(engine::encodeSrgb(linear),0.f,1.f)));
}
}
void exportImage(const QString& requestedPath,const engine::DocumentPtr& document,const ExportOptions& options) {
    require(bool(document),"Image export requires a document");
    const auto path=QFileInfo(requestedPath).absoluteFilePath();
    const auto extension=QFileInfo(path).suffix().toLower();
    const bool png=extension=="png";
    require(png || extension=="jpg" || extension=="jpeg","Image export requires .png, .jpg or .jpeg");
    require(options.quality>=1 && options.quality<=100,"JPEG quality must be 1..100");
    const auto resolution=options.resolution==0 ? document->resolution : options.resolution;
    require(std::isfinite(resolution) && resolution>0 && resolution<=65535,"Export DPI must be positive and at most 65535");
    require(document->width>0 && document->height>0 && document->width<=32768 && document->height<=32768,"Export dimensions exceed 32768 pixels per side");
    require(fileIdentity(path)==options.expected,"Export destination changed; confirm replacement");
    checkpoint(options,ExportStage::Encode);
    auto memory=options.memory ? options.memory : std::make_shared<MemoryAdmission>(2ULL*1024*1024*1024,512*1024*1024);
    // Initial JPEG memory-manager/control allocations precede callback injection.
    // This fixed allowance covers those small codec controls and stack objects;
    // subsequent codec payload allocations are individually admitted exactly.
    auto controls=memory->require(128*1024);controls.commit();
    auto rowClaim=memory->require(static_cast<std::uint64_t>(document->width)*4+64);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(document->width)*(png ? 4u : 3u));rowClaim.commit();
    auto planClaim=memory->require(CpuLayerSampler::scratchBytes(document->stack.prepared().size()));
    // Sampling caches and the output device mutate during row processing too.
    // Keep those objects on the heap before setjmp, alongside codec state.
    auto cache=std::make_unique<MipCache>(options.mipBytes,memory);
    auto sampler=std::make_unique<CpuLayerSampler>(document->stack,engine::Coordinate{1,1},*cache);
    auto layers=sampler->all();planClaim.commit();
    auto outputOwner=std::make_unique<QSaveFile>(path);auto& output=*outputOwner;
    output.setDirectWriteFallback(false);
    require(output.open(QIODevice::WriteOnly),"Cannot create atomic export temporary file");
    auto codecOwner=std::make_unique<Codec>(memory,output);
    auto& codec=*codecOwner;
    // Codec error recovery jumps only across C calls. Mutable codec state lives
    // on the heap so setjmp does not invalidate changed automatic variables.
    struct State {
        png_structp pngWriter=nullptr;png_infop pngInfo=nullptr;
        jpeg_compress_struct jpeg{};JpegError jpegErrors{};JpegDestination destination{};
        bool jpegCreated=false;
    };
    auto state=std::make_unique<State>();
    auto& pngWriter=state->pngWriter;auto& pngInfo=state->pngInfo;
    auto& jpeg=state->jpeg;auto& jpegErrors=state->jpegErrors;
    auto& destination=state->destination;auto& jpegCreated=state->jpegCreated;
    const auto cleanup=[&] {
        if(pngWriter) png_destroy_write_struct(&pngWriter,&pngInfo);
        if(jpegCreated) {jpeg_destroy_compress(&jpeg);jpegCreated=false;}
    };
    if(setjmp(codec.jump)) {cleanup();throw std::runtime_error(codec.error[0] ? codec.error : "Image codec failed");}
    try {
        if(png) {
            pngWriter=png_create_write_struct_2(PNG_LIBPNG_VER_STRING,&codec,pngError,pngWarning,&codec,pngAllocate,pngFree);
            require(pngWriter!=nullptr,"Cannot initialize PNG codec");
            pngInfo=png_create_info_struct(pngWriter);require(pngInfo!=nullptr,"Cannot initialize PNG metadata");
            png_set_write_fn(pngWriter,&codec,pngWrite,pngFlush);
            png_set_IHDR(pngWriter,pngInfo,static_cast<png_uint_32>(document->width),static_cast<png_uint_32>(document->height),8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
            png_set_sRGB(pngWriter,pngInfo,PNG_sRGB_INTENT_PERCEPTUAL);
            const auto ppm=static_cast<png_uint_32>(std::lround(resolution/0.0254));
            png_set_pHYs(pngWriter,pngInfo,ppm,ppm,PNG_RESOLUTION_METER);png_write_info(pngWriter,pngInfo);
        } else {
            jpeg.err=jpeg_std_error(&jpegErrors.manager);jpegErrors.codec=&codec;jpegErrors.manager.error_exit=jpegError;
            jpegCreated=true;jpeg_create_compress(&jpeg);jpeg.client_data=&codec;
            codec.jpegFreePool=jpeg.mem->free_pool;codec.jpegDestroy=jpeg.mem->self_destruct;
            jpeg.mem->alloc_small=jpegSmall;jpeg.mem->alloc_large=jpegLarge;jpeg.mem->free_pool=jpegFreePool;jpeg.mem->self_destruct=jpegDestroy;
            jpeg.mem->request_virt_sarray=jpegVirtualSamples;jpeg.mem->request_virt_barray=jpegVirtualBlocks;
            destination.codec=&codec;destination.manager.init_destination=jpegInitDestination;
            destination.manager.empty_output_buffer=jpegEmptyDestination;destination.manager.term_destination=jpegEndDestination;
            jpeg.dest=&destination.manager;jpeg.image_width=static_cast<JDIMENSION>(document->width);jpeg.image_height=static_cast<JDIMENSION>(document->height);
            jpeg.input_components=3;jpeg.in_color_space=JCS_RGB;jpeg_set_defaults(&jpeg);
            jpeg_set_quality(&jpeg,options.quality,TRUE);jpeg.optimize_coding=FALSE;
            jpeg.comp_info[0].h_samp_factor=1;jpeg.comp_info[0].v_samp_factor=1;
            jpeg.density_unit=1;jpeg.X_density=static_cast<UINT16>(std::lround(resolution));jpeg.Y_density=jpeg.X_density;
            jpeg_start_compress(&jpeg,TRUE);
        }
        for(int y=0;y<document->height;++y) {
            checkpoint(options,ExportStage::Row);
            for(int x=0;x<document->width;++x) {
                if((x&255)==0 && options.stop.stop_requested()) throw std::runtime_error("Image export cancelled");
                const auto pixel=sampler->evaluate(layers,x+.5,y+.5,{},options.stop);
                require(pixel.has_value(),"Image export cancelled");
                const auto offset=static_cast<std::size_t>(x)*(png ? 4u : 3u);
                const std::array<float,3> rgb{pixel->r,pixel->g,pixel->b};
                for(std::size_t c=0;c<3;++c) row[offset+c]=code(png ? (pixel->a>0 ? rgb[c]/pixel->a : 0.f) : rgb[c]+1-pixel->a);
                if(png) row[offset+3]=static_cast<std::uint8_t>(std::lround(255*std::clamp(pixel->a,0.f,1.f)));
            }
            if(png) png_write_row(pngWriter,row.data());
            else {JSAMPROW data=row.data();require(jpeg_write_scanlines(&jpeg,&data,1)==1,"JPEG row write failed");}
        }
        checkpoint(options,ExportStage::Encode);
        if(png) png_write_end(pngWriter,pngInfo);else jpeg_finish_compress(&jpeg);
        cleanup();checkpoint(options,ExportStage::Replace);
        require(fileIdentity(path)==options.expected,"Export destination changed during encoding; previous file retained");
        require(output.commit(),"Cannot commit image export");
    } catch(...) {cleanup();throw;}
}
}
