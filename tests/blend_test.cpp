#include "core/blend.h"
#include "core/hard_mix_thresholds.h"
#include <bit>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace compositor::engine;
namespace {
void expect(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
bool near(float a,float b) { return std::abs(a-b)<2e-6f; }
template<class F> void rejects(F f) { bool rejected=false;try {f();} catch(const std::exception&) {rejected=true;} expect(rejected,"Expected invalid blend rejection"); }
}
int main() {
    try {
        const std::array<Pixel,9> colors{{{0,0,0,1},{1,1,1,1},{1,0,0,1},{0,1,0,1},{0,0,1,1},
            {.5f,.5f,.5f,1},{.2f,.6f,.8f,1},{.6f,.6f,.2f,1},{.4f,.1f,.4f,1}}};
        for(std::size_t m=0;m<blendModeCount;++m) {
            const auto mode=static_cast<BlendMode>(m); expect(parseBlendMode(blendIdentifier(mode))==mode,"Mode identifier roundtrip");
            for(float sa:{0.f,.25f,.5f,1.f}) for(float ba:{0.f,.25f,.5f,1.f}) for(auto sourceColor:colors) for(auto backdropColor:colors) {
                sourceColor.a=sa; backdropColor.a=ba;
                const auto s=fromStraightSrgb(sourceColor),b=fromStraightSrgb(backdropColor); const auto output=composite(s,b,mode);
                expect(near(output.a,sa+ba*(1-sa)),"Alpha independent of artistic blend mode");
                for(float c:{output.r,output.g,output.b}) expect(std::isfinite(c) && c>=-1e-6f && c<=output.a+1e-6f,"Bounded blend stays premultiplied");
                if(sa==0) expect(output==b,"Transparent source is identity");
                if(ba==0) expect(output==s,"Transparent backdrop is identity");
            }
        }
        // Analytic, opaque encoded overlap cases, deliberately not linear RGB
        // multiplication. All channels equal isolates the separable formula.
        const auto source=fromStraightSrgb({.8f,.8f,.8f,1}),backdrop=fromStraightSrgb({.25f,.25f,.25f,1});
        const std::array<float,9> codes{.8f,.2f,.85f,.4f,.25f,.8f,.55f,1.f,.0625f};
        for(int mode=0;mode<9;++mode) {
            const auto result=composite(source,backdrop,static_cast<BlendMode>(mode));
            expect(near(result.r,decodeSrgb(codes[static_cast<std::size_t>(mode)])),"Separable analytic fixture");
        }
        static_assert(static_cast<int>(BlendMode::Luminosity)==12);
        static_assert(blendModeCount==24);
        const std::array<float,11> extensionCodes{.05f,1.f,.4f,.7f,.625f,.85f,.6f,1.f,.65f,0.f,.3125f};
        for(std::size_t i=0;i<extensionCodes.size();++i) {
            const auto mode=static_cast<BlendMode>(13+i);
            const auto result=composite(source,backdrop,mode);
            expect(near(result.r,decodeSrgb(extensionCodes[i])),"Extended separable analytic fixture");
            const auto soft=composite(fromStraightSrgb({.8f,.8f,.8f,.25f}),backdrop,mode);
            expect(near(soft.r,.25f*result.r+.75f*backdrop.r),"New modes apply source alpha after artistic overlap");
            const auto bothSoft=composite(fromStraightSrgb({.8f,.8f,.8f,.25f}),
                fromStraightSrgb({.25f,.25f,.25f,.5f}),mode);
            expect(near(bothSoft.r,.125f*source.r+.125f*result.r+.375f*backdrop.r),
                "New modes preserve both non-overlap contributions");
        }
        // Columns: source/backdrop black/black, white/black, black/white,
        // white/white. Pin exact precedence at the singular Burn/Dodge corners.
        const std::array<std::array<float,4>,11> endpoints{{
            {0,0,0,1}, {0,1,1,1}, {0,0,1,1}, {0,1,0,1}, {0,0,1,1},
            {0,1,0,1}, {0,1,0,1}, {0,0,1,1}, {0,1,1,0}, {0,0,1,0}, {1,0,1,1}}};
        for(std::size_t i=0;i<endpoints.size();++i) for(int corner=0;corner<4;++corner) {
            const float sc=static_cast<float>(corner%2),dc=static_cast<float>(corner/2);
            const auto result=composite({sc,sc,sc,1},{dc,dc,dc,1},static_cast<BlendMode>(13+i));
            expect(result.r==endpoints[i][static_cast<std::size_t>(corner)],"New-mode exact black/white endpoints");
        }
        const auto lightSource=fromStraightSrgb({.75f,.75f,.75f,1});
        const auto softLow=composite(lightSource,fromStraightSrgb({.1f,.1f,.1f,1}),BlendMode::SoftLight);
        const auto softHigh=composite(lightSource,fromStraightSrgb({.64f,.64f,.64f,1}),BlendMode::SoftLight);
        expect(near(softLow.r,decodeSrgb(.198f)),"Soft Light cubic branch analytic value");
        expect(near(softHigh.r,decodeSrgb(.72f)),"Soft Light square-root branch analytic value");
        const auto hardThreshold=composite(fromStraightSrgb({.5f,.5f,.5f,1}),
            fromStraightSrgb({.5f,.5f,.5f,1}),BlendMode::HardMix);
        expect(hardThreshold.r==1,"Hard Mix includes exact half threshold");
        // Independently chosen complementary encoded code centers classify
        // below a frozen threshold as black, and equality/above as white.
        const std::array<std::array<std::uint32_t,2>,8> hardMixBoundaries{{
            {0x371e8391u,0x3f7fdb9cu},
            {0x37edc55au,0x3f7fb73au},
            {0x3ba88520u,0x3f5d1969u},
            {0x3c6ab9d2u,0x3f3d24edu},
            {0x3d503037u,0x3f05c3f9u},
            {0x3e5b0ffdu,0x3e5b2d9au},
            {0x3f05b783u,0x3d506372u},
            {0x3f7fc96bu,0x379e8391u},
        }};
        for(const auto& bits:hardMixBoundaries) {
            const float boundary=std::bit_cast<float>(bits[0])*.5f;
            const float center=std::bit_cast<float>(bits[1])*.5f;
            const std::array<float,3> channels{std::nextafter(boundary,0.f),boundary,std::nextafter(boundary,1.f)};
            for(std::size_t side=0;side<channels.size();++side) {
                const float channel=channels[side];
                const auto result=composite({channel,channel,channel,.5f},{center,center,center,.5f},BlendMode::HardMix);
                const float expected=.5f*channel+(side==0 ? 0.f : .25f)+.5f*center;
                expect(result==Pixel{expected,expected,expected,.75f},"Hard Mix frozen threshold nextafter classification");
            }
        }
        for(std::size_t m=0;m<blendModeCount;++m) {
            const auto mode=static_cast<BlendMode>(m);
            expect(composite({2,-1,0,.5f},{},mode)==Pixel{2,-1,0,.5f},"Extended source survives absent overlap");
            expect(composite({}, {2,-1,0,.5f},mode)==Pixel{2,-1,0,.5f},"Extended backdrop survives absent overlap");
        }
        const Pixel red{1,0,0,1},blue{0,0,1,1};
        const auto hue=composite(red,blue,BlendMode::Hue),color=composite(red,blue,BlendMode::Color);
        expect(near(hue.r,decodeSrgb(.11f/.3f)) && near(hue.g,0) && near(hue.b,0),"Hue uses encoded W3C luminosity");
        expect(hue==color,"Primary color and hue fixture");
        const auto lum=composite(red,blue,BlendMode::Luminosity);
        expect(near(lum.r,decodeSrgb(.19f/.89f)) && near(lum.g,decodeSrgb(.19f/.89f)) && near(lum.b,1),"Luminosity clips saturation while preserving target luminosity");
        const auto gray=fromStraightSrgb({.5,.5,.5,1});
        const auto saturated=composite(red,gray,BlendMode::Saturation);
        expect(near(saturated.r,gray.r) && near(saturated.g,gray.g) && near(saturated.b,gray.b),"Achromatic backdrop saturation is stable");
        const auto halfGray=unpack(pack(fromStraightSrgb({.5f,.5f,.5f,.25f})));
        const auto grayOnBlack=composite(halfGray,{0,0,0,.25f},BlendMode::Color);
        expect(near(grayOnBlack.r,halfGray.r*.75f) && grayOnBlack.r==grayOnBlack.g && grayOnBlack.g==grayOnBlack.b,
            "Half-quantized gray over black has no zero-denominator luminosity clip");
        for(auto mode:{BlendMode::Hue,BlendMode::Saturation,BlendMode::Color,BlendMode::Luminosity})
            for(float code:{0.f,.00001f,.1f,.5f,.99999f,1.f}) for(float background:{0.f,.5f,1.f}) {
                const auto s=unpack(pack(fromStraightSrgb({code,code,code,.25f})));
                const auto b=unpack(pack(fromStraightSrgb({background,background,background,.5f})));
                const auto result=composite(s,b,mode);
                expect(std::isfinite(result.r) && result.r==result.g && result.g==result.b,"Neutral nonseparable blends remain finite and achromatic");
            }
        for(auto mode:{BlendMode::ColorBurn,BlendMode::ColorDodge}) {
            Pixel transparent=fromStraightSrgb({.8f,.3f,.6f,.25f});
            Pixel opaque=fromStraightSrgb({.8f,.3f,.6f,1});
            const auto soft=composite(transparent,backdrop,mode),hard=composite(opaque,backdrop,mode);
            expect(near(soft.r,.25f*hard.r+.75f*backdrop.r),"Burn/Dodge soft alpha regression from SeparableBlend.swift");
        }
        expect(composite({1,1,1,1},{0,0,0,1},BlendMode::ColorDodge).r==0,"Dodge black takes precedence at white source");
        expect(composite({0,0,0,1},{1,1,1,1},BlendMode::ColorBurn).r==1,"Burn white takes precedence at black source");
        const auto extended=composite({2,-1,0,.5},{0,0,0,.5},BlendMode::Multiply);
        expect(extended.r==1 && extended.g==-.5f,"Extended non-overlap RGB is not clipped");
        rejects([]{parseBlendMode("unknown");}); rejects([]{composite({},{},static_cast<BlendMode>(99));});
        rejects([]{composite({0,0,0,2},{},BlendMode::Normal);});
        rejects([]{composite({std::numeric_limits<float>::infinity(),0,0,1},{},BlendMode::Normal);});
        std::cout<<"24 scalar blend modes, alpha endpoints, analytic colors and soft-alpha composition passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
