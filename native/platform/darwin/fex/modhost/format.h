// SPDX-License-Identifier: MIT
#pragma once
#include "nt_strings.h"
#include <cstdio>
#include <climits>
namespace RavenFormat {
struct Args {const uint64_t *p;uint64_t next(){return *p++;}};
std::string render(const char *format,Args &args,bool wide) {
  std::string output;
  for(const char *p=format;*p;) {
    if(*p!='%'){output+=*p++;continue;}++p;if(*p=='%'){output+=*p++;continue;}
    std::string spec="%";
    while(*p && strchr("-+ #0",*p))spec+=*p++;
    if(*p=='*'){spec+=std::to_string(int32_t(args.next()));++p;}else while(isdigit(uint8_t(*p)))spec+=*p++;
    int precision=-1;
    if(*p=='.'){spec+=*p++;if(*p=='*'){precision=int32_t(args.next());++p;spec+=std::to_string(std::max(0,precision));}else {precision=0;while(isdigit(uint8_t(*p))){precision=precision*10+(*p-'0');spec+=*p++;if(precision>1000000)throw std::runtime_error("format precision too large");}}}
    std::string length;
    if(!strncmp(p,"I64",3)){length="ll";p+=3;}else if(!strncmp(p,"I32",3)){p+=3;}else if(*p && strchr("hljztLw",*p)){length+=*p++;if(*p==length[0] && (*p=='l'||*p=='h'))length+=*p++;}
    char conversion=*p++;if(!conversion)throw std::runtime_error("incomplete format");
    auto value=args.next();
    auto append=[&](auto v,const std::string &f){int n=snprintf(nullptr,0,f.c_str(),v);if(n<0 || n>1000000)throw std::runtime_error("format failure");std::vector<char> b(n+1);snprintf(b.data(),b.size(),f.c_str(),v);output.append(b.data(),n);};
    if(conversion=='s' || conversion=='S') {
      bool unicode=length=="l" || length=="w" || (wide?(conversion=='s' && length!="h"):conversion=='S');
      std::string text;
      if(!value)text="(null)";
      else if(unicode){auto *s=reinterpret_cast<const char16_t*>(value);text=RavenNT::encode(s,RavenCRT::length(s));}
      else text=reinterpret_cast<const char*>(value);
      append(text.c_str(),spec+"s");
    }else if(conversion=='c' || conversion=='C') {
      bool unicode=length=="l" || length=="w" || (wide?conversion=='c':conversion=='C');
      if(unicode){char16_t c=value;auto text=RavenNT::encode(&c,1);append(text.c_str(),spec+"s");}
      else append(int(value),spec+"c");
    }else if(strchr("diouxX",conversion)) {
      bool large=length=="ll" || length=="j" || length=="z" || length=="t";
      if(large){if(conversion=='d'||conversion=='i')append(static_cast<long long>(value),spec+"ll"+conversion);else append(static_cast<unsigned long long>(value),spec+"ll"+conversion);}
      else if(conversion=='d'||conversion=='i') {int v=length=="h"?int16_t(value):length=="hh"?int8_t(value):int32_t(value);append(v,spec+conversion);}
      else {unsigned v=length=="h"?uint16_t(value):length=="hh"?uint8_t(value):uint32_t(value);append(v,spec+conversion);}
    }else if(strchr("fFeEgGaA",conversion)){double v;memcpy(&v,&value,8);append(v,spec+conversion);}
    else if(conversion=='p')append(reinterpret_cast<void*>(value),spec+"p");
    else if(conversion=='n'){if(length=="ll")*reinterpret_cast<int64_t*>(value)=output.size();else if(length=="h")*reinterpret_cast<int16_t*>(value)=output.size();else if(length=="hh")*reinterpret_cast<int8_t*>(value)=output.size();else *reinterpret_cast<int32_t*>(value)=output.size();}
    else throw std::runtime_error("unsupported format conversion");
  }
  return output;
}
int output(char *out,size_t count,const char *format,const uint64_t *values,bool legacy=true) {
  try {Args args{values};auto result=render(format,args,false);auto n=result.size();
    if(out && count){auto copy=std::min(n,count);memcpy(out,result.data(),copy);if(n<count)out[n]=0;}
    return n>=count && legacy?-1:int(n);
  }catch(...){errno=EINVAL;if(count && out)*out=0;return -1;}
}
int wide_output(char16_t *out,size_t count,const char16_t *format,const uint64_t *values) {
  try {auto f=RavenNT::encode(format,RavenCRT::length(format));Args args{values};auto text=render(f.c_str(),args,true);auto result=RavenNT::decode(text.data(),text.size());auto n=result.size();
    if(out && count){auto copy=std::min(n,count);memcpy(out,result.data(),copy*2);if(n<count)out[n]=0;}return n>=count?-1:int(n);
  }catch(...){errno=EINVAL;if(count && out)*out=0;return -1;}
}
std::vector<uint64_t> varargs(uint64_t a,uint64_t b,const uint64_t *stack,size_t size) {
  std::vector<uint64_t> v{a,b};if(size && !stack)throw std::runtime_error("missing EC varargs stack");
  if(size)v.insert(v.end(),stack,stack+size/8);return v;
}
int print(char *out,const char *format,uint64_t a,uint64_t b,const uint64_t *stack,uint64_t size) {auto v=varargs(a,b,stack,size);return output(out,SIZE_MAX,format,v.data());}
int wide_print(char16_t *out,const char16_t *format,uint64_t a,uint64_t b,const uint64_t *stack,uint64_t size) {auto v=varargs(a,b,stack,size);return wide_output(out,SIZE_MAX,format,v.data());}
int vprint(char *out,const char *format,const uint64_t *v) {return output(out,SIZE_MAX,format,v);}
int vlimit(char *out,size_t count,const char *format,const uint64_t *v) {return output(out,count,format,v);}
int wide_vlimit(char16_t *out,size_t count,const char16_t *format,const uint64_t *v) {return wide_output(out,count,format,v);}
int scan(const char *input,const char *format,uint64_t a,uint64_t b,const uint64_t *stack,uint64_t size) {
  auto values=varargs(a,b,stack,size);size_t arg=0;int assigned=0;
  for(const char *p=format;*p;) {
    if(isspace(uint8_t(*p))){while(isspace(uint8_t(*p)))++p;while(isspace(uint8_t(*input)))++input;continue;}
    if(*p!='%'){if(*input++!=*p++)break;continue;}++p;if(*p=='%'){++p;if(*input++!='%')break;continue;}
    std::string spec="%";bool suppress=*p=='*';if(suppress)spec+=*p++;
    while(isdigit(uint8_t(*p)))spec+=*p++;
    std::string length;
    if(!strncmp(p,"I64",3)){length="ll";p+=3;}else if(*p && strchr("hljztL",*p)){length+=*p++;if(*p==length[0] && (*p=='h'||*p=='l'))length+=*p++;}
    char c=*p++;spec+=length;spec+=c;
    if(c=='['){if(*p=='^')spec+=*p++;if(*p==']')spec+=*p++;while(*p && *p!=']')spec+=*p++;if(*p)spec+=*p++;}
    if(!c || (!suppress && arg>=values.size())){errno=EINVAL;return -1;}
    int used=-1;void *dest=suppress?nullptr:reinterpret_cast<void*>(values[arg++]);
    // Windows long is 32 bits. Darwin int matches its storage and scan format.
    if(length=="l" && strchr("diouxXn",c))spec.erase(spec.size()-2,1);
    auto with_count=spec+"%n";
    if(suppress)sscanf(input,with_count.c_str(),&used);
    else sscanf(input,with_count.c_str(),dest,&used);
    if(used<0)break;input+=used;if(!suppress && c!='n')++assigned;
  }
  return assigned;
}
void *resolve(const char *name) {
#define F(n,f) if(!strcmp(name,n))return reinterpret_cast<void*>(&f)
  F("sprintf",print);F("swprintf",wide_print);F("vsprintf",vprint);F("_vsnprintf",vlimit);F("_vsnwprintf",wide_vlimit);F("sscanf",scan);
#undef F
  return nullptr;
}
}
