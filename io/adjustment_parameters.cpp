#include "io/adjustment_parameters.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <stdexcept>
namespace compositor::io {
using namespace engine;
namespace {
void check(bool value) {if(!value) throw std::runtime_error("Invalid revisable adjustment parameters");}
bool defaultChannels(const AdjustmentParameters& p) {
 for(const auto& curve:p.channelCurves) if(curve!=std::vector<CurvePoint>{{0,0},{1,1}}) return false;
 return true;
}
bool identityChannels(const AdjustmentParameters& p) {
 for(const auto& curve:p.channelCurves) {
  if(curve.size()<2 || curve.size()>16 || curve.front().input!=0 || curve.back().input!=1) return false;
  for(std::size_t i=0;i<curve.size();++i) if(!std::isfinite(curve[i].input) || curve[i].input!=curve[i].output || (i && curve[i].input<=curve[i-1].input)) return false;
 }
 return true;
}
QJsonArray points(const std::vector<CurvePoint>& curve) {
 check(curve.size()>=2 && curve.size()<=16);
 QJsonArray result;for(const auto& p:curve) {check(std::isfinite(p.input) && std::isfinite(p.output));result.append(QJsonArray{p.input,p.output});}return result;
}
QJsonArray operation(const AdjustmentParameters& p,bool channels) {
 check(std::isfinite(p.value));
 check(p.kind==AdjustmentKind::Levels || p.levels==LevelsParameters{});
 check(p.kind==AdjustmentKind::Curves || p.curve==std::vector<CurvePoint>{{0,0},{1,1}});
 check(p.kind==AdjustmentKind::Curves || defaultChannels(p));
 check(p.kind==AdjustmentKind::ColorBalance || p.colorBalance==ColorBalanceParameters{});
 check(static_cast<int>(p.kind)<=3 || p.value==0);

 QJsonArray result{static_cast<int>(p.kind),p.value,QJsonArray{p.levels.inputBlack,p.levels.inputWhite,p.levels.gamma,p.levels.outputBlack,p.levels.outputWhite},points(p.curve),QJsonArray{p.colorBalance.warmth,p.colorBalance.tint}};
 if(channels) {QJsonArray values;for(const auto& curve:p.channelCurves) values.append(points(curve));result.append(values);}
 else check(defaultChannels(p));
 return result;
}
double number(const QJsonValue& value) {check(value.isDouble() && std::isfinite(value.toDouble()));return value.toDouble();}
std::string encode(const AdjustmentStack& stack,const RasterSnapshot& rendered,Sampling sampling,int formatVersion) {
 check(!stack.operations.empty());
 validateAdjustmentStack(stack,rendered);
 check(formatVersion==4 || formatVersion==5);
 check(stack.policy==1 || (formatVersion==5 && stack.policy==2));
 QJsonArray operations; for(const auto& p:stack.operations) {if(stack.policy==1) check(identityChannels(p));operations.append(operation(p,formatVersion==5));}
 QJsonObject object{{"version",formatVersion==4 ? 2 : 3},{"kind","raster"},{"policy",static_cast<int>(stack.policy)},{"source",QString::fromStdString(stack.source->id.text())},{"rendered",QString::fromStdString(rendered.id.text())},{"revision",QString::number(rendered.revision)},{"sampling",static_cast<int>(sampling)},{"operations",operations}};
 const auto result=QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
 check(result.size()<=static_cast<std::size_t>(formatVersion==4 ? 32768 : 65536));return result;
}
}
std::string adjustmentParameters(const AdjustmentStack& stack,const RasterSnapshot& rendered,Sampling sampling) {return encode(stack,rendered,sampling,5);}
StoredAdjustment readAdjustmentParameters(const std::string& text,std::shared_ptr<const RasterSnapshot> source,int formatVersion) {
 if(formatVersion==6) formatVersion=5;
 check((formatVersion==4 || formatVersion==5) && source!=nullptr && text.size()<=static_cast<std::size_t>(formatVersion==4 ? 32768 : 65536)); QJsonParseError error; const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(text),&error);
 check(error.error==QJsonParseError::NoError && document.isObject());const auto o=document.object();
 check(o.value("version").toInt(-1)==(formatVersion==4 ? 2 : 3) && o.value("kind").toString()=="raster" && (o.value("policy").toInt(-1)==1 || (formatVersion==5 && o.value("policy").toInt(-1)==2)) && o.value("source").toString().toStdString()==source->id.text());
 bool valid=false;const auto revision=o.value("revision").toString().toULongLong(&valid);check(valid && revision<=INT64_MAX);
 const auto sampling=o.value("sampling").toInt(-1);check(sampling>=0 && sampling<=2);
 StoredAdjustment result{{source,{},static_cast<std::uint32_t>(o.value("policy").toInt())},Id(o.value("rendered").toString().toStdString()),revision,static_cast<Sampling>(sampling)};
 check(o.value("operations").isArray());const auto operations=o.value("operations").toArray();check(!operations.isEmpty() && operations.size()<=16);
 for(const auto& value:operations) {
  check(value.isArray());const auto a=value.toArray();check(a.size()==(formatVersion==4 ? 5 : 6));AdjustmentParameters p;const auto kind=number(a[0]);check(kind>=0 && kind<=6 && std::floor(kind)==kind);p.kind=static_cast<AdjustmentKind>(static_cast<int>(kind));p.value=number(a[1]);
  check(a[2].isArray() && a[2].toArray().size()==5);const auto l=a[2].toArray();p.levels={number(l[0]),number(l[1]),number(l[2]),number(l[3]),number(l[4])};
  check(a[3].isArray());const auto curve=a[3].toArray();check(curve.size()>=2 && curve.size()<=16);p.curve.clear();for(const auto& point:curve){check(point.isArray() && point.toArray().size()==2);p.curve.push_back({number(point.toArray()[0]),number(point.toArray()[1])});}
  check(a[4].isArray() && a[4].toArray().size()==2);p.colorBalance={number(a[4].toArray()[0]),number(a[4].toArray()[1])};
  if(formatVersion==5) {
   check(a[5].isArray() && a[5].toArray().size()==3);const auto channels=a[5].toArray();
   for(int channel=0;channel<3;++channel) {
    check(channels[channel].isArray());const auto channelPoints=channels[channel].toArray();check(channelPoints.size()>=2 && channelPoints.size()<=16);
    auto& points=p.channelCurves[static_cast<std::size_t>(channel)];points.clear();
    for(const auto& point:channelPoints) {check(point.isArray() && point.toArray().size()==2);points.push_back({number(point.toArray()[0]),number(point.toArray()[1])});}
   }
  }
  result.stack.operations.push_back(std::move(p));
 }
 RasterSnapshot metadata(result.renderedId,source->extent,source->defaultValue,{},result.renderedRevision,source->sourceProfile);
 check(encode(result.stack,metadata,result.sampling,formatVersion)==text);return result;
}
}
