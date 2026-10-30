// SPDX-License-Identifier: MIT
#pragma once
#include "nt_strings.h"
#include "vm.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <map>
#include <memory>
#include <mutex>
namespace RavenNT {
constexpr int32_t bad_handle=int32_t(0xc0000008), not_supported=int32_t(0xc00000bb);
struct ObjectAttributes { uint32_t length,pad; void *root; String *name; uint32_t attributes,pad2; void *security,*qos; };
struct IoStatus { int64_t status; uint64_t information; };
int32_t os_error(int error) {
  switch(error) {case 0:return 0;case ENOENT:return int32_t(0xc0000034); case ENOTDIR:return int32_t(0xc000003a);
    case EACCES:case EPERM:return int32_t(0xc0000022); case EEXIST:return int32_t(0xc0000035);
    case ENOMEM:case ENOSPC:return int32_t(0xc0000017); case EBADF:return bad_handle;
    case EINVAL:return invalid_parameter; default:return int32_t(0xc0000185); }
}
struct ThreadControl;
struct Handle {
  enum Kind {file,section,key,thread} kind;
  std::shared_ptr<ThreadControl> control;
  uint32_t access=0;
  int fd=-1; DIR *directory=nullptr; std::string path;
  size_t section_size=0; uint32_t section_protection=0;
  explicit Handle(Kind k):kind(k) {}
  ~Handle(){ if(directory) closedir(directory); if(fd>=0) close(fd); }
};
std::shared_ptr<Handle> (*pseudo_handle)(void*)=nullptr;
std::mutex handles_mutex;
std::map<uintptr_t,std::shared_ptr<Handle>> handles;
uintptr_t next_handle=0x100;
void *new_handle(std::shared_ptr<Handle> h) { std::lock_guard<std::mutex> g(handles_mutex); auto id=next_handle;next_handle+=4;handles[id]=std::move(h);return reinterpret_cast<void*>(id); }
std::shared_ptr<Handle> get_handle(void *h) { if(intptr_t(h)==-2 && pseudo_handle)return pseudo_handle(h); std::lock_guard<std::mutex> g(handles_mutex);auto i=handles.find(uintptr_t(h));return i==handles.end()?nullptr:i->second; }
int32_t close_handle(void *h) { std::lock_guard<std::mutex> g(handles_mutex);return handles.erase(uintptr_t(h))?0:bad_handle; }
int32_t duplicate(void *source_process,void *h,void *target_process,void **out,uint32_t access,uint32_t attributes,uint32_t options) {
  (void)access;
  if(out)*out=nullptr;
  if(intptr_t(source_process)!=-1 || intptr_t(target_process)!=-1 || !out || attributes || (options&~3u)) return not_supported;
  auto object=get_handle(h); if(!object) return bad_handle;
  *out=new_handle(object); if(options&1) close_handle(h); return 0;
}
std::string object_path(const ObjectAttributes *o) {
  if(!o || !o->name || (o->name->length&1)) throw std::runtime_error("invalid object path");
  auto path=encode(static_cast<char16_t*>(o->name->buffer),o->name->length/2);
  if(path.compare(0,4,"\\??\\")==0) path.erase(0,4);
  if(path.size()>=2 && path[1]==':') { if(path[0]!='Z' && path[0]!='z') throw std::runtime_error("unmapped drive"); path.erase(0,2); }
  std::replace(path.begin(),path.end(),'\\','/');
  if(o->root) { auto root=get_handle(o->root); if(!root || root->kind!=Handle::file) throw std::runtime_error("invalid root"); path=root->path+"/"+path; }
  return path;
}
int32_t create_file(void **out,uint32_t access,ObjectAttributes *object,IoStatus *io,const int64_t *allocation,
                    uint32_t attributes,uint32_t sharing,uint32_t disposition,uint64_t options,uint64_t ea,uint64_t ea_size) {
  options=uint32_t(options);ea_size=uint32_t(ea_size);
  if(out)*out=nullptr; if(io)*io={invalid_parameter,0};
  if(!out || !io) return invalid_parameter;
  (void)attributes;(void)sharing;
  if(ea || ea_size || (options&0x1000) || (allocation && *allocation<0)) return io->status=not_supported;
  std::string path;try {path=object_path(object);}catch(...){return io->status=invalid_parameter;}
  int flags=O_CLOEXEC;
  bool write=access&(0x40000000u|0x2u|0x4u),read=access&(0x80000000u|0x1u);
  flags|=write?(read?O_RDWR:O_WRONLY):O_RDONLY;
  switch(disposition) {case 0:case 5:flags|=O_CREAT|O_TRUNC;break;case 1:break;case 2:flags|=O_CREAT|O_EXCL;break;case 3:flags|=O_CREAT;break;case 4:flags|=O_TRUNC;break;default:return io->status=invalid_parameter;}
  if(options&1) { if(disposition==2 || disposition==3) {if(mkdir(path.c_str(),0700) && (errno!=EEXIST || disposition==2)) return io->status=os_error(errno);}flags=O_RDONLY|O_DIRECTORY|O_CLOEXEC; }
  int fd=open(path.c_str(),flags,0600);if(fd<0)return io->status=os_error(errno);
  auto h=std::make_shared<Handle>(Handle::file);h->fd=fd;h->path=path;h->access=access;
  if(allocation && *allocation && ftruncate(fd,*allocation)) return io->status=os_error(errno);
  *out=new_handle(h);*io={0,disposition==2?2u:1u};return 0;
}
int32_t open_file(void **out,uint32_t access,ObjectAttributes *o,IoStatus *io,uint32_t sharing,uint32_t options) {return create_file(out,access,o,io,nullptr,0,sharing,1,options,0,0);}
int32_t read_file(void *handle,void *event,void *apc,void *context,IoStatus *io,void *buffer,uint32_t size,const int64_t *offset,uint64_t key) {
  (void)context;if(!io)return invalid_parameter;*io={0,0};
  auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  if(event || apc || key)return io->status=not_supported;
  auto n=offset && *offset>=0?pread(h->fd,buffer,size,*offset):read(h->fd,buffer,size);
  if(n<0)return io->status=os_error(errno);io->information=n;
  if(!n && size)return io->status=int32_t(0xc0000011);return 0;
}
int32_t write_file(void *handle,void *event,void *apc,void *context,IoStatus *io,const void *buffer,uint32_t size,const int64_t *offset,uint64_t key) {
  (void)context;if(!io)return invalid_parameter;*io={0,0};auto h=get_handle(handle);
  if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  if(event || apc || key)return io->status=not_supported;
  auto n=offset && *offset>=0?pwrite(h->fd,buffer,size,*offset):write(h->fd,buffer,size);
  if(n<0)return io->status=os_error(errno);io->information=n;return 0;
}
int64_t nt_time(timespec t) {return 116444736000000000ll+int64_t(t.tv_sec)*10000000+t.tv_nsec/100;}
struct FileBasic {int64_t created,accessed,written,changed;uint32_t attributes,pad;};
FileBasic basic(const struct stat &s) {return {nt_time(s.st_birthtimespec),nt_time(s.st_atimespec),nt_time(s.st_mtimespec),nt_time(s.st_ctimespec),S_ISDIR(s.st_mode)?0x10u:0x80u,0};}
int32_t query_attributes(ObjectAttributes *o,FileBasic *out) {
  if(!out)return invalid_parameter;try {auto path=object_path(o);struct stat s{};if(stat(path.c_str(),&s))return os_error(errno);*out=basic(s);return 0;}catch(...){return invalid_parameter;}
}
int32_t query_file(void *handle,IoStatus *io,void *output,uint32_t length,uint32_t klass) {
  if(!io || !output)return invalid_parameter;*io={0,0};auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  struct stat st{};if(fstat(h->fd,&st))return io->status=os_error(errno);
  if(klass==4) {if(length<40)return io->status=buffer_small;*static_cast<FileBasic*>(output)=basic(st);io->information=40;}
  else if(klass==5) {struct Standard {int64_t allocated,eof;uint32_t links;uint8_t deleting,directory;uint16_t pad;};if(length<24)return io->status=buffer_small;*static_cast<Standard*>(output)={st.st_blocks*512,st.st_size,uint32_t(st.st_nlink),0,uint8_t(S_ISDIR(st.st_mode)),0};io->information=24;}
  else if(klass==14) {if(length<8)return io->status=buffer_small;auto pos=lseek(h->fd,0,SEEK_CUR);if(pos<0)return io->status=os_error(errno);*static_cast<int64_t*>(output)=pos;io->information=8;}
  else return io->status=int32_t(0xc0000003);
  return 0;
}
int32_t set_file(void *handle,IoStatus *io,const void *input,uint32_t length,uint32_t klass) {
  if(!io || !input)return invalid_parameter;*io={0,0};auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  if((klass==14 || klass==20) && length>=8) {auto value=*static_cast<const int64_t*>(input);if(value<0)return io->status=invalid_parameter;auto result=klass==14?lseek(h->fd,value,SEEK_SET):ftruncate(h->fd,value);if(result<0)return io->status=os_error(errno);return 0;}
  return io->status=not_supported;
}
int32_t lock_file(void *handle,void *event,void *apc,void *,IoStatus *io,const int64_t *offset,const int64_t *length,uint32_t key,uint64_t fail,uint64_t exclusive) {
  fail=uint8_t(fail);exclusive=uint8_t(exclusive);
  if(!io || !offset || !length)return invalid_parameter;*io={0,0};auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  if(event || apc || key)return io->status=not_supported;
  struct flock lock{};lock.l_type=exclusive?F_WRLCK:F_RDLCK;lock.l_whence=SEEK_SET;lock.l_start=*offset;lock.l_len=*length;
  if(fcntl(h->fd,fail?F_SETLK:F_SETLKW,&lock)<0)return io->status=os_error(errno);return 0;
}
int32_t unlock_file(void *handle,IoStatus *io,const int64_t *offset,const int64_t *length,uint32_t key) {
  if(!io || !offset || !length)return invalid_parameter;*io={0,0};auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;if(key)return io->status=not_supported;
  struct flock lock{};lock.l_type=F_UNLCK;lock.l_whence=SEEK_SET;lock.l_start=*offset;lock.l_len=*length;
  if(fcntl(h->fd,F_SETLK,&lock)<0)return io->status=os_error(errno);return 0;
}
int32_t create_section(void **out,uint32_t,ObjectAttributes *object,const int64_t *size,uint32_t protection,uint32_t attributes,void *file) {
  if(out)*out=nullptr;if(!out)return invalid_parameter;if(object || (attributes&~0x8000000u))return not_supported;
  auto h=std::make_shared<Handle>(Handle::section);h->section_protection=protection;
  if(file) {auto f=get_handle(file);if(!f || f->kind!=Handle::file)return bad_handle;h->fd=dup(f->fd);if(h->fd<0)return os_error(errno);struct stat st{};if(fstat(h->fd,&st))return os_error(errno);h->section_size=size?*size:st.st_size;}
  else {if(!size || *size<=0)return invalid_parameter;h->section_size=*size;}
  *out=new_handle(h);return 0;
}
std::map<void*,size_t> views;
int32_t map_section(void *handle,void *process,void **base,uint64_t zero,size_t commit,int64_t *offset,size_t *size,uint32_t inherit,uint64_t allocation,uint64_t protection) {
  allocation=uint32_t(allocation);protection=uint32_t(protection);
  (void)commit;(void)inherit;
  if(intptr_t(process)!=-1 || !base || !size)return invalid_parameter;
  if(zero || allocation || *base || (offset && (*offset<0 || *offset%getpagesize())))return not_supported;
  auto h=get_handle(handle);if(!h || h->kind!=Handle::section)return bad_handle;
  auto at=offset?uint64_t(*offset):0;if(at>=h->section_size)return invalid_parameter;
  auto n=*size?*size:h->section_size-at;if(n>h->section_size-at)return invalid_parameter;
  auto prot=RavenVM::prot(protection);if(prot<0)return invalid_parameter;
  void *p=mmap(nullptr,n,prot,MAP_PRIVATE|(h->fd<0?MAP_ANON:0),h->fd,at);if(p==MAP_FAILED)return os_error(errno);
  {std::lock_guard<std::mutex> g(handles_mutex);views[p]=n;}*base=p;*size=n;return 0;
}
int32_t unmap_section(void *process,void *base) {if(intptr_t(process)!=-1)return bad_handle;std::lock_guard<std::mutex> g(handles_mutex);auto i=views.find(base);if(i==views.end())return invalid_parameter;if(munmap(base,i->second))return os_error(errno);views.erase(i);return 0;}
int32_t query_directory(void *handle,void *event,void *apc,void *,IoStatus *io,void *out,uint32_t length,uint32_t klass,uint64_t single,uint64_t pattern_value,uint64_t restart) {
  single=uint8_t(single);restart=uint8_t(restart);
  (void)single;
  if(!io || !out)return invalid_parameter;*io={0,0};auto h=get_handle(handle);if(!h || h->kind!=Handle::file)return io->status=bad_handle;
  if(event || apc)return io->status=not_supported;
  size_t header=klass==12?12:klass==1?64:klass==2?68:klass==3?94:0;
  if(!header)return io->status=int32_t(0xc0000003);
  if(!h->directory){h->directory=fdopendir(dup(h->fd));if(!h->directory)return io->status=os_error(errno);}
  if(restart)rewinddir(h->directory);
  std::string pattern="*";
  if(auto *p=reinterpret_cast<String*>(pattern_value))pattern=encode(static_cast<char16_t*>(p->buffer),p->length/2);
  while(true){auto position=telldir(h->directory);auto *entry=readdir(h->directory);if(!entry)return io->status=int32_t(0x80000006);
    if(fnmatch(pattern.c_str(),entry->d_name,0))continue;
    auto name=decode(entry->d_name,strlen(entry->d_name));auto size=name.size()*2;
    if(header+size>length){seekdir(h->directory,position);return io->status=buffer_small;}
    memset(out,0,header);auto *bytes=static_cast<uint8_t*>(out);auto name_length=uint32_t(size);
    memcpy(bytes+(klass==12?8:60),&name_length,4);memcpy(bytes+header,name.data(),size);
    if(klass!=12){struct stat st{};if(fstatat(h->fd,entry->d_name,&st,0))return io->status=os_error(errno);auto b=basic(st);memcpy(bytes+8,&b,32);int64_t eof=st.st_size,allocated=st.st_blocks*512;memcpy(bytes+40,&eof,8);memcpy(bytes+48,&allocated,8);memcpy(bytes+56,&b.attributes,4);}
    io->information=header+size;return 0;
  }
}
void *resolve_io(const char *name,unsigned &stack) {
#define NT(n,f,s) if(!strcmp(name,n)){stack=s;return reinterpret_cast<void*>(&f);}
  NT("NtQueryDirectoryFile",query_directory,3);
  NT("NtClose",close_handle,0); NT("NtDuplicateObject",duplicate,0);
  NT("NtCreateFile",create_file,3);NT("NtOpenFile",open_file,0);
  NT("NtReadFile",read_file,1);NT("NtWriteFile",write_file,1);
  NT("NtQueryAttributesFile",query_attributes,0);NT("NtQueryInformationFile",query_file,0);NT("NtSetInformationFile",set_file,0);
  NT("NtLockFile",lock_file,2);NT("NtUnlockFile",unlock_file,0);
  NT("NtCreateSection",create_section,0);NT("NtMapViewOfSection",map_section,2);NT("NtUnmapViewOfSection",unmap_section,0);
#undef NT
  return nullptr;
}
}
