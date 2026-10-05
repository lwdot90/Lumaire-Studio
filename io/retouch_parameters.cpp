#include "io/retouch_parameters.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <stdexcept>
namespace compositor::io {
using namespace engine;
namespace {
void check(bool v){if(!v) throw std::runtime_error("Invalid retained retouch parameters");}
double number(const QJsonValue& v){check(v.isDouble() && std::isfinite(v.toDouble()));return v.toDouble();}
QJsonArray pair(Coordinate p){check(std::isfinite(p.x)&&std::isfinite(p.y));return {p.x,p.y};}
QJsonValue selection(const std::optional<Selection>& s){if(!s)return QJsonValue(QJsonValue::Null);const auto& b=s->bounds;return QJsonArray{QString::number(b.x),QString::number(b.y),QString::number(b.width),QString::number(b.height),static_cast<int>(s->shape),s->inverted,s->featherRadius};}
std::string encode(const StoredRetouch& r){
 QJsonArray strokes;std::size_t count=0;check(r.stack.policy==1 && !r.stack.strokes.empty()&&r.stack.strokes.size()<=16);
 for(const auto& s:r.stack.strokes){count+=s.points.size();check(count<=32768);QJsonArray points;for(auto p:s.points)points.append(pair(p));
 strokes.append(QJsonArray{static_cast<int>(s.kind),pair(s.sourceAnchor),points,s.diameter,s.hardness,s.opacity,s.healingRadius,QJsonArray{s.localToDocument.a,s.localToDocument.b,s.localToDocument.c,s.localToDocument.d,s.localToDocument.tx,s.localToDocument.ty},selection(s.selection),s.canvasWidth,s.canvasHeight});}
 QJsonObject o{{"version",4},{"kind","raster"},{"policy",1},{"source",QString::fromStdString(r.stack.source->id.text())},{"intermediate",QString::fromStdString(r.intermediateId.text())},{"intermediateRevision",QString::number(r.intermediateRevision)},{"rendered",QString::fromStdString(r.renderedId.text())},{"revision",QString::number(r.renderedRevision)},{"sampling",static_cast<int>(r.sampling)},{"strokes",strokes},{"adjustment",QString::fromStdString(r.adjustment)}};
 auto text=QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();check(text.size()<=2*1024*1024);return text;
}
}
std::string retouchParameters(const RetouchStack& stack,const RasterSnapshot& rendered,const AdjustmentStack* adjustments,Sampling sampling){
 const auto& intermediate=adjustments ? *adjustments->source : rendered;validateRetouchStack(stack,intermediate);
 check(intermediate.revision>=0 && rendered.revision>=0);
 StoredRetouch r{stack,intermediate.id,rendered.id,static_cast<std::uint64_t>(intermediate.revision),static_cast<std::uint64_t>(rendered.revision),sampling,{}};
 if(adjustments) r.adjustment=adjustmentParameters(*adjustments,rendered,sampling);
 return encode(r);
}
StoredRetouch readRetouchParameters(const std::string& text,std::shared_ptr<const RasterSnapshot> source){
 check(source && text.size()<=2*1024*1024);QJsonParseError error;auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(text),&error);check(error.error==QJsonParseError::NoError&&doc.isObject());auto o=doc.object();
 check(o.value("version").toInt(-1)==4&&o.value("kind").toString()=="raster"&&o.value("policy").toInt(-1)==1&&o.value("source").toString().toStdString()==source->id.text());
 auto revision=[&](const char* key){bool ok=false;auto v=o.value(key).toString().toULongLong(&ok);check(ok&&v<=INT64_MAX);return v;};
 auto sample=o.value("sampling").toInt(-1);check(sample>=0&&sample<=2&&o.value("adjustment").isString());
 StoredRetouch r{{source,{},1},Id(o.value("intermediate").toString().toStdString()),Id(o.value("rendered").toString().toStdString()),revision("intermediateRevision"),revision("revision"),static_cast<Sampling>(sample),o.value("adjustment").toString().toStdString()};
 check(o.value("strokes").isArray());for(auto v:o.value("strokes").toArray()){
 check(v.isArray());auto a=v.toArray();check(a.size()==11);RetouchStroke s;auto kind=number(a[0]);check(kind==0||kind==1);s.kind=static_cast<RetouchKind>(static_cast<int>(kind));
 auto point=[](const QJsonValue& p){check(p.isArray()&&p.toArray().size()==2);return Coordinate{number(p.toArray()[0]),number(p.toArray()[1])};};s.sourceAnchor=point(a[1]);check(a[2].isArray());for(auto p:a[2].toArray()){check(s.points.size()<32768);s.points.push_back(point(p));}
 s.diameter=number(a[3]);s.hardness=number(a[4]);s.opacity=number(a[5]);s.healingRadius=number(a[6]);check(a[7].isArray()&&a[7].toArray().size()==6);auto m=a[7].toArray();s.localToDocument={number(m[0]),number(m[1]),number(m[2]),number(m[3]),number(m[4]),number(m[5])};
 auto integer=[](const QJsonValue& element){bool ok=false;auto n=element.toString().toLongLong(&ok);check(ok);return n;};
 if(!a[8].isNull()){check(a[8].isArray()&&a[8].toArray().size()==7);auto sel=a[8].toArray();auto shape=number(sel[4]);check((shape==0||shape==1)&&sel[5].isBool());s.selection=Selection{{integer(sel[0]),integer(sel[1]),integer(sel[2]),integer(sel[3])},static_cast<SelectionShape>(static_cast<int>(shape)),sel[5].toBool(),number(sel[6])};}
 auto w=number(a[9]),h=number(a[10]);check(w>=1&&w<=30000&&h>=1&&h<=30000&&std::floor(w)==w&&std::floor(h)==h);s.canvasWidth=static_cast<int>(w);s.canvasHeight=static_cast<int>(h);r.stack.strokes.push_back(std::move(s));check(r.stack.strokes.size()<=16);
 }
 RasterSnapshot metadata(r.intermediateId,source->extent,source->defaultValue,{},r.intermediateRevision,source->sourceProfile);validateRetouchStack(r.stack,metadata);
 if(r.adjustment.empty())check(r.intermediateId==r.renderedId&&r.intermediateRevision==r.renderedRevision);
 else {auto intermediate=std::make_shared<const RasterSnapshot>(metadata);auto adjustment=readAdjustmentParameters(r.adjustment,intermediate,5);check(adjustment.renderedId==r.renderedId&&adjustment.renderedRevision==r.renderedRevision&&adjustment.sampling==r.sampling);}
 check(encode(r)==text);return r;
}
}
