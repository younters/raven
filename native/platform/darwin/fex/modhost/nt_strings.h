// SPDX-License-Identifier: MIT
#pragma once
#include "crt.h"
#include <vector>
#include <unistd.h>
namespace RavenNT {
constexpr int32_t invalid_parameter = int32_t(0xc000000d), buffer_small = int32_t(0xc0000023);
struct String { uint16_t length, maximum; uint32_t pad; void *buffer; };
static_assert(sizeof(String) == 16);
std::u16string decode(const char *p, size_t count) {
  std::u16string out;
  for (size_t i = 0; i < count;) {
    uint32_t c = uint8_t(p[i++]);
    if (c >= 128) {
      unsigned n = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : 0;
      if (!n || i+n > count) throw std::runtime_error("invalid UTF-8");
      auto minimum = n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u;
      c &= (1u << (6-n))-1;
      while (n--) { auto next = uint8_t(p[i++]); if ((next & 0xc0) != 0x80) throw std::runtime_error("invalid UTF-8"); c = (c<<6) | (next&63); }
      if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) throw std::runtime_error("invalid UTF-8");
    }
    if (c > 0xffff) { c -= 0x10000; out += char16_t(0xd800+(c>>10)); out += char16_t(0xdc00+(c&1023)); }
    else out += char16_t(c);
  }
  return out;
}
std::string encode(const char16_t *p, size_t count) {
  std::string out;
  for (size_t i=0; i<count; ++i) {
    uint32_t c=p[i];
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i == count || p[i] < 0xdc00 || p[i] > 0xdfff) throw std::runtime_error("invalid UTF-16");
      c=0x10000+((c-0xd800)<<10)+(p[i]-0xdc00);
    } else if (c >= 0xdc00 && c <= 0xdfff) throw std::runtime_error("invalid UTF-16");
    if(c<128) out+=char(c);
    else if(c<2048) { out+=char(0xc0|(c>>6)); out+=char(0x80|(c&63)); }
    else if(c<65536) { out+=char(0xe0|(c>>12)); out+=char(0x80|((c>>6)&63)); out+=char(0x80|(c&63)); }
    else { out+=char(0xf0|(c>>18)); out+=char(0x80|((c>>12)&63)); out+=char(0x80|((c>>6)&63)); out+=char(0x80|(c&63)); }
  }
  return out;
}
int32_t to_unicode(void *out, uint32_t capacity, uint32_t *written, const char *in, uint32_t size) {
  if(written) *written=0;
  if ((!in && size) || (!out && capacity)) return invalid_parameter;
  try {
    auto text=decode(in,size); auto n=uint32_t(text.size()*2);
    if(written) *written=n;
    if (!out) return 0;
    auto copied=std::min(n,capacity & ~1u);
    // Never return a trailing unmatched high surrogate on truncation.
    if (copied<n && copied && text[copied/2-1]>=0xd800 && text[copied/2-1]<=0xdbff) copied-=2;
    memcpy(out,text.data(),copied); if(written) *written=copied;
    return copied<n ? int32_t(0x80000005) : 0;
  } catch (...) { return int32_t(0xc0000162); }
}
int32_t from_unicode(char *out, uint32_t capacity, uint32_t *written, const char16_t *in, uint32_t size) {
  if(written) *written=0;
  if((size&1) || (!in && size) || (!out && capacity)) return invalid_parameter;
  try {
    auto text=encode(in,size/2); auto n=uint32_t(text.size());
    if(written) *written=n;
    if(!out) return 0;
    auto copied=std::min(n,capacity);
    while(copied<n && copied && (uint8_t(text[copied])&0xc0)==0x80) --copied;
    memcpy(out,text.data(),copied); if(written) *written=copied;
    return copied<n ? int32_t(0x80000005) : 0;
  } catch (...) { return int32_t(0xc0000162); }
}
int32_t unicode_size(uint32_t *n,const char *s,uint32_t count) { return n ? to_unicode(nullptr,0,n,s,count) : invalid_parameter; }
int32_t ansi_size(uint32_t *n,const char16_t *s,uint32_t count) { return n ? from_unicode(nullptr,0,n,s,count) : invalid_parameter; }
void init_ansi(String *s,const char *p) { auto n=p?std::min<size_t>(strlen(p),65534):0; *s={uint16_t(n),uint16_t(p?n+1:0),0,const_cast<char*>(p)}; }
void init_unicode(String *s,const char16_t *p) { auto n=p?std::min<size_t>(RavenCRT::length(p),32766)*2:0; *s={uint16_t(n),uint16_t(p?n+2:0),0,const_cast<char16_t*>(p)}; }
void free_string(String *s) { if(s) { free(s->buffer); *s={}; } }
uint8_t create_unicode(String *out,const char *p) {
  *out={};
  try { auto s=decode(p,strlen(p)); if(s.size()>32766) return 0;
    auto *buf=calloc(s.size()+1,2); if(!buf) return 0; memcpy(buf,s.data(),s.size()*2);
    *out={uint16_t(s.size()*2),uint16_t(s.size()*2+2),0,buf}; return 1;
  } catch (...) { return 0; }
}
int32_t unicode_to_ansi(String *out,const String *in,uint8_t allocate) {
  uint32_t count=0; auto status=ansi_size(&count,static_cast<char16_t*>(in->buffer),in->length);
  if(status) return status;
  if(count>65534) return buffer_small;
  if(allocate) { out->buffer=malloc(count+1); if(!out->buffer) return int32_t(0xc0000017); out->maximum=count+1; }
  out->length=0;
  if(out->maximum<count+1) return buffer_small;
  status=from_unicode(static_cast<char*>(out->buffer),count,&count,static_cast<char16_t*>(in->buffer),in->length);
  if(!status) { out->length=count; static_cast<char*>(out->buffer)[count]=0; } return status;
}
struct Bitmap { uint32_t count, pad; uint32_t *bits; };
void clear_bits(Bitmap *b,uint32_t first,uint32_t n) { if(first>b->count || n>b->count-first) std::abort(); for(uint32_t i=first;i<first+n;++i) b->bits[i/32]&=~(1u<<(i%32)); }
uint32_t find_clear(Bitmap *b,uint32_t n,uint32_t hint) {
  if(!n || n>b->count) return UINT32_MAX;
  if(hint>=b->count) hint=0;
  for(unsigned pass=0;pass<2;++pass) {
    uint32_t end=pass?hint:b->count;
    for(uint32_t at=pass?0:hint;at<end && n<=b->count-at;++at) {
      uint32_t j=0; while(j<n && !(b->bits[(at+j)/32] & (1u<<((at+j)%32)))) ++j;
      if(j==n) { for(j=0;j<n;++j) b->bits[(at+j)/32]|=1u<<((at+j)%32); return at; } at+=j;
    }
  } return UINT32_MAX;
}
uint32_t current_directory(uint32_t bytes,char16_t *out) {
  char path[4096]; if(!getcwd(path,sizeof(path))) return 0;
  auto s=decode(path,strlen(path)); auto n=uint32_t(s.size()*2);
  if(bytes<n+2) return n+2;
  memcpy(out,s.data(),n); out[n/2]=0; return n;
}
uint8_t dos_path(const char16_t *in,String *out,char16_t **part,void *relative) {
  if(relative) return 0; // Relative-name object lifetime not supported by this host.
  try {
    std::u16string p(in);
    if(p.empty()) return 0;
    // Standalone namespace: native absolute paths and Z: paths designate host files.
    if(p[0]!=u'/' && !(p.size()>2 && p[1]==u':') && p[0]!=u'\\') {
      char cwd[4096]; if(!getcwd(cwd,sizeof(cwd))) return 0; p=decode(cwd,strlen(cwd))+u"/"+p;
    }
    if(p.size()>32766) return 0;
    auto *buf=static_cast<char16_t*>(calloc(p.size()+1,2)); if(!buf) return 0;
    memcpy(buf,p.data(),p.size()*2); *out={uint16_t(p.size()*2),uint16_t(p.size()*2+2),0,buf};
    if(part) { *part=buf; for(size_t i=0;i<p.size();++i) if(buf[i]==u'/' || buf[i]==u'\\') *part=buf+i+1; }
    return 1;
  } catch(...) { return 0; }
}
void *resolve_strings(const char *name) {
#define NT(n,f) if(!strcmp(name,n)) return reinterpret_cast<void*>(&f)
  NT("RtlInitAnsiString",init_ansi); NT("RtlInitUnicodeString",init_unicode);
  NT("RtlFreeAnsiString",free_string); NT("RtlFreeUnicodeString",free_string);
  NT("RtlCreateUnicodeStringFromAsciiz",create_unicode); NT("RtlUnicodeStringToAnsiString",unicode_to_ansi);
  NT("RtlMultiByteToUnicodeN",to_unicode); NT("RtlUTF8ToUnicodeN",to_unicode);
  NT("RtlUnicodeToMultiByteN",from_unicode); NT("RtlUnicodeToUTF8N",from_unicode);
  NT("RtlMultiByteToUnicodeSize",unicode_size); NT("RtlUnicodeToMultiByteSize",ansi_size);
  NT("RtlClearBits",clear_bits); NT("RtlFindClearBitsAndSet",find_clear);
  NT("RtlGetCurrentDirectory_U",current_directory); NT("RtlDosPathNameToNtPathName_U",dos_path);
#undef NT
  return nullptr;
}
}
