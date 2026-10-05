#include "io/adjustment_parameters.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
using namespace compositor::engine;
using namespace compositor::io;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}expect(rejected,"Invalid parameter record accepted");}
std::string encoded(QJsonObject o){return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 auto source=std::make_shared<const RasterSnapshot>(Id::generate(),Extent{0,0,1,1},pack(Pixel{.125f,.25f,.375f,.5f}));
 RasterSnapshot rendered(Id::generate(),source->extent,source->defaultValue,{},3);
 AdjustmentStack stack{source,{{AdjustmentKind::Exposure,1}},1};const auto record=adjustmentParameters(stack,rendered,Sampling::Bilinear);const auto object=QJsonDocument::fromJson(QByteArray::fromStdString(record)).object();
 const auto decoded=readAdjustmentParameters(record,source);expect(decoded.stack.operations==stack.operations && decoded.renderedRevision==3,"Valid record mismatch");
 for(const auto* key:{"version","policy","sampling"}){auto changed=object;changed.insert(key,1.5);rejects([&]{readAdjustmentParameters(encoded(changed),source);});}
 auto duplicate=record;duplicate.insert(1,"\"policy\":1,");rejects([&]{readAdjustmentParameters(duplicate,source);});
 auto unknown=object;unknown.insert("unknown",true);rejects([&]{readAdjustmentParameters(encoded(unknown),source);});
 auto excessive=object;QJsonArray operations;for(int i=0;i<17;++i)operations.append(object.value("operations").toArray()[0]);excessive.insert("operations",operations);rejects([&]{readAdjustmentParameters(encoded(excessive),source);});
 auto curve=object;auto operation=curve.value("operations").toArray()[0].toArray();operation[0]=5;QJsonArray points;for(int i=0;i<17;++i)points.append(QJsonArray{double(i)/16,double(i)/16});operation[3]=points;curve.insert("operations",QJsonArray{operation});rejects([&]{readAdjustmentParameters(encoded(curve),source);});
 auto irrelevant=object;operation=irrelevant.value("operations").toArray()[0].toArray();operation[4]=QJsonArray{.5,0};irrelevant.insert("operations",QJsonArray{operation});rejects([&]{readAdjustmentParameters(encoded(irrelevant),source);});
 for(const auto* revision:{"9223372036854775808","18446744073709551616","03","-1"}){auto changed=object;changed.insert("revision",revision);rejects([&]{readAdjustmentParameters(encoded(changed),source);});}
 auto wrong=object;wrong.insert("source",QString::fromStdString(Id::generate().text()));rejects([&]{readAdjustmentParameters(encoded(wrong),source);});rejects([&]{readAdjustmentParameters(record,{});});
 auto policy=object;policy.insert("policy",99);rejects([&]{readAdjustmentParameters(encoded(policy),source);});
 auto kind=object;operation=kind.value("operations").toArray()[0].toArray();operation[0]=99;kind.insert("operations",QJsonArray{operation});rejects([&]{readAdjustmentParameters(encoded(kind),source);});
 auto old=object;old.insert("version",2);auto oldOperations=old.value("operations").toArray();for(int i=0;i<oldOperations.size();++i){auto op=oldOperations[i].toArray();op.removeLast();oldOperations[i]=op;}old.insert("operations",oldOperations);const auto oldRecord=encoded(old);expect(readAdjustmentParameters(oldRecord,source,4).stack.operations==stack.operations,"Schema4 identity channel defaults");rejects([&]{readAdjustmentParameters(record,source,4);});rejects([&]{readAdjustmentParameters(oldRecord,source,5);});auto oldPolicy=old;oldPolicy.insert("policy",2);rejects([&]{readAdjustmentParameters(encoded(oldPolicy),source,4);});
 AdjustmentParameters channelCurve;channelCurve.kind=AdjustmentKind::Curves;channelCurve.channelCurves[0]={{0,0},{.5,.25},{1,1}};channelCurve.channelCurves[1]={{0,0},{.5,.75},{1,1}};channelCurve.channelCurves[2]={{0,0},{.5,.6},{1,1}};AdjustmentStack channelStack{source,{channelCurve},2};const auto channelRecord=adjustmentParameters(channelStack,rendered,Sampling::Nearest);expect(readAdjustmentParameters(channelRecord,source).stack.operations==channelStack.operations,"Per-channel exact record");const auto channelObject=QJsonDocument::fromJson(QByteArray::fromStdString(channelRecord)).object();
 auto forbiddenPolicy=channelObject;forbiddenPolicy.insert("policy",1);rejects([&]{readAdjustmentParameters(encoded(forbiddenPolicy),source);});channelStack.policy=1;rejects([&]{adjustmentParameters(channelStack,rendered,Sampling::Nearest);});
 for(const auto count:{2,4}){auto changed=channelObject;auto op=changed.value("operations").toArray()[0].toArray();auto channels=op[5].toArray();if(count==2)channels.removeLast();else channels.append(channels[0]);op[5]=channels;changed.insert("operations",QJsonArray{op});rejects([&]{readAdjustmentParameters(encoded(changed),source);});}
 auto tooMany=channelObject;operation=tooMany.value("operations").toArray()[0].toArray();auto channels=operation[5].toArray();channels[0]=points;operation[5]=channels;tooMany.insert("operations",QJsonArray{operation});rejects([&]{readAdjustmentParameters(encoded(tooMany),source);});
 auto nonfinite=channelRecord;const auto coordinate=nonfinite.find("0.25");expect(coordinate!=std::string::npos,"Channel scalar fixture");nonfinite.replace(coordinate,4,"NaN");rejects([&]{readAdjustmentParameters(nonfinite,source);});
 auto outside=channelObject;operation=outside.value("operations").toArray()[0].toArray();channels=operation[5].toArray();channels[0]=QJsonArray{QJsonArray{0,0},QJsonArray{.5,1.1},QJsonArray{1,1}};operation[5]=channels;outside.insert("operations",QJsonArray{operation});rejects([&]{readAdjustmentParameters(encoded(outside),source);});
 std::cout<<"Revisable adjustment codec adversarial cases passed\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
