#include "test_host.hpp"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <functional>
#include <filesystem>
#include <algorithm>

using namespace dxvk::rt;
using namespace dxvk::rt::testing;
namespace {
Vec3 sub(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float k) {return {a.x*k,a.y*k,a.z*k};}
Vec3 add(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
float dot(Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 normalize(Vec3 a) {return mul(a,1/std::sqrt(dot(a,a)));}
// Reference ONLY for small automated test. Never part of production rendering.
uint32_t oracle(const std::vector<Triangle>& mesh,Vec3 origin,Vec3 ray,float& t) {
  uint32_t id=0;
  for (auto tri : mesh) {
    Vec3 e1=sub(tri.b,tri.a),e2=sub(tri.c,tri.a),p=cross(ray,e2);
    float det=dot(e1,p); if (std::abs(det)<1e-8f) continue;
    Vec3 s=sub(origin,tri.a);float u=dot(s,p)/det; if (u<0 || u>1) continue;
    Vec3 q=cross(s,e1);float v=dot(ray,q)/det; if (v<0 || u+v>1) continue;
    float z=dot(e2,q)/det; if (z>.01f && z<t) {t=z;id=tri.id;}
  }
  return id;
}
std::vector<Triangle> fixture() {
  std::vector<Triangle> out;
  auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 e,uint32_t id) {
    out.push_back({a,id,b,0,c}); out.push_back({a,id,c,0,e});
  };
  quad({10,-30,-30},{10,30,-30},{10,30,30},{10,-30,30},1);
  quad({5,-1,-1},{5,1,-1},{5,1,1},{5,-1,1},2);
  return out;
}
template<class T> T pixel(const std::vector<uint8_t>& data,size_t index) {
  T value; std::memcpy(&value,data.data()+index*sizeof(T),sizeof(T)); return value;
}
void require(bool test,const char* message) {if (!test) throw std::runtime_error(message);}
void writeBmp(const std::filesystem::path& path,const std::vector<uint8_t>& rgba,uint32_t w,uint32_t h) {
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+w*h*4;
  BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(w);info.biHeight=-LONG(h);info.biPlanes=1;info.biBitCount=32;
  std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&file),sizeof(file));f.write(reinterpret_cast<const char*>(&info),sizeof(info));
  for(size_t p=0;p<size_t(w)*h;++p) {
    uint8_t color[4]{};
    for(size_t c=0;c<3;++c) color[2-c]=uint8_t(std::pow(std::clamp(pixel<float>(rgba,p*4+c),0.f,1.f),1/2.2f)*255.f+.5f);
    color[3]=255;f.write(reinterpret_cast<const char*>(color),4);
  }
  if (!f) throw std::runtime_error("Cannot write test image");
}
}
int main(int argc,char** argv) {
  try {
    bool validation=false; std::filesystem::path output;
    for (int i=1;i<argc;++i) {
      std::string arg=argv[i]; if(arg=="--validation") validation=true;
      else if(arg=="--output" && i+1<argc) output=argv[++i];
      else throw std::runtime_error("Usage: rt_test [--validation] [--output existing-directory]");
    }
    {
      Host host;host.init(validation);
      auto mesh=fixture(); Scene scene(host.d,mesh);
      constexpr uint32_t w=96,h=64;
      Frame a(host.d,w,h),b(host.d,w,h),off(host.d,w,h);
      Camera camera;camera.aspect=float(w)/float(h); Camera other=camera;other.origin.y=.25f;
      Camera noShadow=camera;noShadow.flags=Trace;
      Camera disabled=camera;disabled.flags=0;
      host.run([&](auto cmd) {
        scene.recordBuild(cmd); a.initialize(cmd);b.initialize(cmd);off.initialize(cmd);
        scene.recordTrace(cmd,a,camera,other,0);scene.recordTrace(cmd,b,noShadow,other,1);scene.recordTrace(cmd,off,disabled,other,2);
      });
      auto color=host.read(*a.images[0],16),base=host.read(*b.images[0],16),depth=host.read(*a.images[1],4);
      auto ids=host.read(*a.images[3],4),motion=host.read(*a.images[2],8),disabledIds=host.read(*off.images[3],4);
      uint32_t matches=0,shadows=0,foreground=0,shadowMatches=0;
      const auto light=normalize(Vec3{-.7f,-.3f,.4f});
      for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x) {
        const size_t p=size_t(y)*w+x;
        Vec3 ray=normalize(Vec3{1,((float(x)+.5f)/w*2-1)*camera.aspect,-((float(y)+.5f)/h*2-1)});
        float distance=camera.farPlane;uint32_t expected=oracle(mesh,camera.origin,ray,distance);
        require(pixel<uint32_t>(ids,p)==expected,"GPU object ID differs from CPU oracle");
        require(pixel<uint32_t>(disabledIds,p)==0,"RT disable still produced a hit");
        if (!expected) continue;
        float expectedDepth=distance*ray.x;
        require(std::abs(pixel<float>(depth,p)-expectedDepth)<.002f,"GPU depth differs from CPU oracle");
        float mv=-.25f/expectedDepth*(float(w)/(2*camera.aspect));
        require(std::abs(pixel<float>(motion,p*2)-mv)<.002f && std::abs(pixel<float>(motion,p*2+1))<.002f,"GPU motion reprojection mismatch");
        float c=pixel<float>(color,p*4),unshadowed=pixel<float>(base,p*4);
        Vec3 hitPoint=mul(ray,distance);hitPoint.x-=.02f;
        float shadowT=camera.farPlane;bool blocked=oracle(mesh,hitPoint,light,shadowT)!=0;
        require(std::isfinite(c) && std::isfinite(unshadowed),"Nonfinite color");
        bool dark=unshadowed-c>.05f;
        require(dark==blocked,"GPU shadow differs from CPU oracle");
        ++shadowMatches;if (dark) ++shadows;if (expected==2) ++foreground;++matches;
      }
      require(matches>1000 && foreground>10 && shadows>10,"Insufficient hit/shadow coverage");
      auto metadata=a.metadata(1,1,camera);
      require(metadata.color.image==encode(a.images[0]->handle) && metadata.width==w,"FGDS image identity mismatch");
      Camera reflective=camera;reflective.flags|=Reflections;
      host.run([&](auto cmd){scene.recordTrace(cmd,off,reflective,other,2);});
      auto reflected=host.read(*off.images[0],16);uint32_t differences=0;
      for(size_t i=0;i<size_t(w)*h*4;++i) {
        require(std::isfinite(pixel<float>(reflected,i)),"Nonfinite reflection");
        if(std::abs(pixel<float>(reflected,i)-pixel<float>(color,i))>.005f) ++differences;
      }
      require(differences>100,"Reflection toggle has no effect");
      if(!output.empty()) {
        writeBmp(output/"rt-shadows.bmp",color,w,h);writeBmp(output/"rt-no-shadows.bmp",base,w,h);
        writeBmp(output/"rt-reflections.bmp",reflected,w,h);
      }
      std::cout << "PASS primary/depth/motion=" << matches << " foreground=" << foreground << " shadow_oracle=" << shadowMatches
        << " shadow_pixels=" << shadows << " reflection_changed_channels=" << differences << " RT_off=no_hits\n";
    }
    require(validationErrors.load()==0,"Vulkan validation errors including resource cleanup");
    return 0;
  } catch(const Unsupported& e) {std::cout << "SKIP: " << e.what() << '\n';return 77;}
    catch(const std::exception& e) {std::cerr << "FAIL: " << e.what() << '\n';return 1;}
}
