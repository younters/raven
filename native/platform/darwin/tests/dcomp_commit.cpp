// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/darwin/dcomp/commit.h"
#include "desktop/objects.h"
#include <limits>
#include <stdexcept>
#include <cstring>

using namespace raven;
static void check(bool value) { if (!value) throw std::runtime_error("composition commit contract failed"); }
int main()
{
	DesktopObjects objects;
	const auto stream=objects.add(DesktopWindow{}), other=objects.add(DesktopWindow{}), target=objects.add(DesktopWindow{});
	objects.window(stream)->properties[u"RAVENDCOMPSWAPCHAIN"]=1;
	objects.window(other)->properties[u"RAVENDCOMPSWAPCHAIN"]=1;
	objects.window(other)->properties[u"RAVENDCOMPDEVICE"]=2;
	objects.window(other)->properties[u"RAVENDCOMPTARGET"]=target;
	raven_dcomp_item item{};
	item.stream=stream;item.target=target;item.sx=item.sy=1;
	const auto publish=[&](uint32_t magic, uint32_t count, std::span<const raven_dcomp_item> items) {
		return darwin::publish_composition(objects,1,magic,count,items,[](uint64_t h) { return h==999; });
	};
	check(publish(RAVEN_DCOMP_COMMIT_MAGIC,1,{&item,1})==0);
	const auto snapshot=objects.window(stream)->properties;
	const auto reject=[&](raven_dcomp_item bad) {
		check(publish(RAVEN_DCOMP_COMMIT_MAGIC,1,{&bad,1})==0x80070057U);
		check(objects.window(stream)->properties==snapshot);
	};
	check(publish(0,1,{&item,1})==0x80070057U);
	check(publish(RAVEN_DCOMP_COMMIT_MAGIC,129,{})==0x80070057U);
	raven_dcomp_item duplicate[]={item,item};
	check(publish(RAVEN_DCOMP_COMMIT_MAGIC,2,duplicate)==0x80070057U);
	auto bad=item;bad.stream=999;reject(bad);
	bad=item;bad.stream=target;reject(bad);
	bad=item;bad.target=0;reject(bad);
	bad=item;bad.target=998;reject(bad);
	for (float value : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
		for (unsigned field=0;field<20;++field) {
			bad=item;
			// All twenty floats between stream/target and order are protocol fields.
			std::memcpy(reinterpret_cast<char *>(&bad)+16+field*4,&value,4);
			reject(bad);
		}
	}
	for (float value : {0.f,-1.f,16385.f}) {
		bad=item;bad.sx=value;reject(bad);bad=item;bad.sy=value;reject(bad);
	}
	bad=item;bad.linear=2;reject(bad);
	for (auto &radius : item.radius) { radius=-1;reject(item);radius=0; }
	check(objects.window(stream)->properties==snapshot);
	// A batch is all or nothing: a valid first visual is not published when a later one fails.
	const auto second=objects.add(DesktopWindow{});
	objects.window(second)->properties[u"RAVENDCOMPSWAPCHAIN"]=1;
	auto moved=item;moved.x+=5;
	auto broken=item;broken.stream=second;broken.sx=0;
	raven_dcomp_item mixed[]={moved,broken};
	check(publish(RAVEN_DCOMP_COMMIT_MAGIC,2,mixed)==0x80070057U);
	check(objects.window(stream)->properties==snapshot && !objects.window(second)->properties.contains(u"RAVENDCOMPDEVICE"));
	check(publish(RAVEN_DCOMP_COMMIT_MAGIC,0,{})==0);
	check(objects.window(stream)->properties.at(u"RAVENDCOMPTARGET")==0);
	check(objects.window(other)->properties.at(u"RAVENDCOMPTARGET")==target);
	item.target=999;check(publish(RAVEN_DCOMP_COMMIT_MAGIC,1,{&item,1})==0);
}
