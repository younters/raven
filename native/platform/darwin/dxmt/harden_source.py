#!/usr/bin/env python3
"""Instrument the pinned native wire readers; fail on unclassified pointers.

Run only on a private source copy. The compiled count is derived from the real
array and argument sizes from the exact types consumed by its entry points.
"""
from pathlib import Path
import re
import hashlib
import json


def functions(text):
    # Mask literals/comments without changing offsets; ignore nested control flow.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: re.sub(r'[^\n]', ' ', m[0]), text, flags=re.S)
    previous = 0
    for m in re.finditer(r'\b(\w+)\(([^;{}]*?)\)\s*\{', masked, re.S):
        if m.start() < previous or m[1] in ('if', 'while', 'switch', 'for', 'available'): continue
        depth, end = 1, m.end()
        while depth and end < len(text):
            depth += (masked[end] == '{') - (masked[end] == '}'); end += 1
        previous = end
        yield m[1], m[2], m.end(), end - 1


def harden(root):
    pins = json.loads(Path(__file__).with_name('checked_sources.json').read_text())
    if not any(all((root / name).is_file() and hashlib.sha256((root/name).read_bytes()).hexdigest() == digest
                   for name, digest in pin.items()) for pin in pins.values()):
        raise ValueError('native reader sources differ from the reviewed pins; audit the range schema before rebuilding')
    native = root / 'src/winemetal/unix/winemetal_unix.c'
    original = native.read_text()
    entries = [x.strip().lstrip('&') for x in original.split('const void *__wine_unix_call_funcs[] = {')[1].split('};')[0].split(',') if x.strip()]
    sizes = {'_MTLCommandBuffer_renderCommandEncoder': 'sizeof(struct unixcall_generic_obj_uint64_obj_ret)'}
    # Classified non-struct pointer spans. Multiplication is performed in the
    # checked helper, never in unchecked guest arithmetic.
    strings = {'output_url','name','url','path','directory','executable','reason','site'}
    for file in [native, native.parent/'cache.c']:
        created = re.findall(r'CREATES\((\w+)\)\(void \*obj\)', file.read_text())
        text = re.sub(r'CREATES\((\w+)\)\(void \*obj\)', r'\1(void *obj)', file.read_text())
        edits=[]
        for name, signature, begin, end in functions(text):
            body=text[begin:end]
            top=re.search(r'struct (\w+) \*\w+ = (?:obj|args);',body)
            if name in entries:
                if top:sizes[name]='sizeof(struct '+top[1]+')'
                elif 'NSObject **' in signature or '*(obj_handle_t *)obj' in body:sizes[name]='sizeof(void *)'
                elif name not in sizes:
                    sizes[name]='0' if '(void)' in signature or not re.search(r'\b(obj|args)\b',body) else None
            # Pointer bindings establish the exact fixed-size pointee. This also
            # covers integer-encoded WMTRenderPassInfo pointers and command bodies.
            bindings={}
            for m in re.finditer(r'((?:const\s+)?struct\s+\w+(?:\s+const)?)\s*\*\s*(\w+)\s*=\s*(?:\([^;=]+?\)\s*)?([\w]+->\w+\.ptr)\s*[;,]',body):
                bindings[m[3]]=(f'sizeof({m[1].replace("const", "").strip()})',0 if 'const' in m[1] else 1)
            def replace(m):
                expr=m[0]; var,field=expr[:-4].split('->')
                before=body[max(0,m.start()-12):m.start()];after=body[m.end():m.end()+8]
                # Reading a pointer value for a null test or assigning an output
                # does not dereference the pointed-to memory.
                if re.match(r'\s*=(?!=)',after) or re.search(r'(?:if\s*\(|&&\s*)$',before) or re.match(r'\s*(?:!=|==)',after):return expr
                if field in strings:return f'raven_unix_string({expr})'
                if field=='buffer_ptr' and 'params->encoding' in body:return f'raven_unix_string({expr})'
                rules={
                    'bytes':(f'{var}->length','1',0),
                    'viewports':(f'{var}->viewport_count','sizeof(MTLViewport)',0),
                    'scissor_rects':(f'{var}->rect_count','sizeof(MTLScissorRect)',0),
                    'binary_archives_for_lookup':(f'{var}->num_binary_archives_for_lookup','sizeof(void *)',0),
                    'memory':(f'{var}->length','1',1),
                    'allocations':('params->count','sizeof(void *)',0),
                    'attachments':('params->num_attachments','sizeof(struct WMTSampleBufferAttachmentInfo)',0),
                    'data_out':('params->data_length','1',1),
                    'source':('params->length','1',0),
                    'key':('params->key_length','1',0),
                    'lines':('params->length','1',0),
                    'records':('params->count','sizeof(struct WMTFlightEntry)',1),
                    'functions':(f'{var}->function_count','sizeof(void *)',0),
                    'geometries':('info->geometry_count','sizeof(struct WMTAccelerationStructureGeometry)',0),
                    'next':('1','sizeof(struct wmtcmd_base)',0),
                    'cmd_head':('1','sizeof(struct wmtcmd_base)',0),
                    'head':('1','sizeof(struct wmtcmd_base)',0),
                    'render':('1','sizeof(struct WMTRenderPassInfo)',0),
                    'render_info':('1','sizeof(struct WMTRenderPassInfo)',0),
                    'samples':('1','sizeof(struct WMTSampleBufferAttachmentInfo)',0),
                }
                if field=='data':
                    if 'replaceregion' in (top[1] if top else ''):
                        # A slice includes all rows; image pitch is used for 3D.
                        rules[field]=('params->size.depth > 1 ? params->bytes_per_image : params->bytes_per_row', 'params->size.depth > 1 ? params->size.depth : params->size.height',int('getBytes' in name))
                    else:rules[field]=('params->length','1',0)
                if field=='buffer':
                    rules[field]=('params->buffer_size' if top and top[1]=='unixcall_enumerate' else 'params->size','sizeof(void *)' if top and top[1]=='unixcall_enumerate' else '1',1)
                if field=='operations':
                    kind='Copy' if 'copyMappings' in name else 'Update'
                    rules[field]=('params->count',f'params->texture ? sizeof(MTL4{kind}SparseTextureMappingOperation) : sizeof(MTL4{kind}SparseBufferMappingOperation)',0)
                if field=='constants':rules[field]=('params->num_constants','sizeof(struct WMTFunctionConstant)',0)
                if field in rules:count,stride,write=rules[field]
                elif expr in bindings:stride,write=bindings[expr];count='1'
                else:raise ValueError(f'unclassified pointer {file.name}:{name}: {expr}')
                helper = 'raven_unix_optional' if field in ('next', 'cmd_head', 'head', 'samples') else 'raven_unix_checked'
                return f'{helper}({expr}, {count}, {stride}, {write})'
            body=re.sub(r'\b\w+->\w+\.ptr',replace,body)
            # Every discriminated command payload gets its own full extent check.
            body=re.sub(r'(struct (wmtcmd_\w+) \*body =\s*)\(struct \2 \*\)next;',r'\1raven_unix_checked(next, 1, sizeof(struct \2), 0);',body)
            # Integer-encoded pointers (arg is otherwise frequently a scalar).
            body=re.sub(r'((?:const )?struct (\w+) \*(?:info|desc) = )\((?:const )?struct \2 \*\)params->arg;',r'\1raven_unix_checked((void *)params->arg, 1, sizeof(struct \2), 0);',body)
            # Raw pointers in the shader ABI and integer-encoded byte spans.
            checks = []
            if top and name in entries and name.startswith('thunk_'):
                schema = top[1]
                if schema == 'sm50_initialize_params':
                    checks += ['raven_unix_checked(params->bytecode, params->bytecode_size, 1, 0);',
                               'raven_unix_checked(params->shader, 1, sizeof(*params->shader), 1);',
                               'raven_unix_optional(params->reflection, 1, sizeof(struct MTL_SHADER_REFLECTION), 1);']
                if 'compile' in schema and 'compiled' not in schema:
                    field = next((f for f in ('vertex_args','geometry_args','hull_args','domain_args','args') if 'params->'+f in body), None)
                    if field: checks += [f'raven_shader_arguments(params->{field});']
                    checks += ['raven_unix_string(params->func_name);', 'raven_unix_checked(params->bitcode, 1, sizeof(*params->bitcode), 1);']
                if 'params->error' in body and schema not in ('sm50_free_error_params','sm50_get_error_message_params'):
                    checks += ['raven_unix_optional(params->error, 1, sizeof(*params->error), 1);']
                if schema == 'sm50_get_compiled_bitcode_params': checks += ['raven_unix_checked(params->data_out, 1, sizeof(*params->data_out), 1);']
                if schema == 'sm50_get_error_message_params': checks += ['raven_unix_checked(params->buffer, params->buffer_size, 1, 1);']
                if schema == 'sm50_get_ray_shader_params': checks += ['raven_unix_checked(params->name, params->name_size, 1, 1);', 'raven_unix_checked(params->info, 1, sizeof(*params->info), 1);']
            if name == '_MTLBuffer_updateContents':
                checks += ['uint64_t capacity = [(id<MTLBuffer>)params->buffer length];',
                           'if (params->offset > capacity || params->length > capacity - params->offset) return 0xc000000d;']
            if name == '_NSString_getCString': checks += ['raven_unix_checked((void *)params->buffer_ptr, params->max_length, 1, 1);']
            if name == '_MTLLibrary_newFunction': checks += ['raven_unix_string((void *)params->arg);']
            if 'dispatch_data_create((void *)params->handle' in body: checks += ['raven_unix_checked((void *)params->handle, params->arg, 1, 0);']
            if checks:
                body = re.sub(r'(struct \w+ \*params = (?:obj|args);)', lambda m:m[0]+'\n  '+'\n  '.join(checks), body, count=1)
            body = body.replace('constants[i].data.ptr', 'raven_function_constant(constants[i].data.ptr, constants[i].type)')
            edits.append((begin,end,body))
        for a,b,v in reversed(edits):text=text[:a]+v+text[b:]
        for name in created:
            text = text.replace(name+'(void *obj) {', 'CREATES('+name+')(void *obj) {')
        text='#include "raven_checked_memory.h"\n'+text.replace('memcpy(', 'raven_unix_copy(')
        file.write_text(text)
    missing=[(i,n,sizes.get(n)) for i,n in enumerate(entries) if n!='NULL' and sizes.get(n) is None]
    if missing:raise ValueError(f'unclassified argument blocks: {missing}')
    native.write_text(native.read_text().replace('#define STATUS_SUCCESS 0', '#define STATUS_SUCCESS 0\n'+shader_checks(root)) +'''\n#include "raven_macdrv_abi.h"
RAVEN_MACDRV_TABLE(struct macdrv_functions_t, macdrv_init_display_devices, get_win_data,
    release_win_data, macdrv_get_cocoa_window, macdrv_create_metal_device, macdrv_release_metal_device,
    macdrv_view_create_metal_view, macdrv_view_get_metal_layer, macdrv_view_release_metal_view, on_main_thread);
RAVEN_MACDRV_DATA(struct macdrv_win_data, hwnd, cocoa_window, cocoa_view, client_cocoa_view);
RAVEN_MACDRV_LAYER(struct WMTLayerProps, device, contents_scale, drawable_width, drawable_height,
    opaque, display_sync_enabled, framebuffer_only, pixel_format);
static const uint32_t raven_argument_sizes[] = {\n'''+',\n'.join(sizes.get(n,'0') for n in entries)+'''};
static int raven_validate_arguments(uint32_t code, void *args, raven_unix_check_range check) {
    return code < sizeof(__wine_unix_call_funcs)/sizeof(__wine_unix_call_funcs[0]) &&
        check((uintptr_t)args, raven_argument_sizes[code], 1);
}
const struct raven_unix_contract *raven_unix_get_contract(void) {
    static const struct raven_unix_contract contract = {RAVEN_UNIX_CONTRACT_VERSION,
        sizeof(__wine_unix_call_funcs)/sizeof(__wine_unix_call_funcs[0]),
        raven_argument_sizes, raven_validate_arguments, raven_unix_set_memory_check};
    return &contract;
}
''')
    converter = root / 'src/airconv/dxbc_converter.cpp'
    text = converter.read_text()
    # These output lengths come from the native shader object, not a wire count.
    # Check them at the final copy, where the exact extent is available.
    position = list(re.finditer(r'^#include .*$', text, re.M))[-1].end()
    text = text[:position] + '''
#include "../winemetal/unix/raven_checked_memory.h"
#include <cstring>
static void *raven_checked_copy(void *dst, const void *src, size_t size) {
    raven_unix_checked(dst, size, 1, 1);
    raven_unix_checked(src, size, 1, 0);
    return memcpy(dst, src, size);
}
#define memcpy raven_checked_copy
''' + text[position:]
    converter.write_text(text)
    return len(entries)

def shader_checks(root):
    public = (root/'src/airconv/airconv_public.h').read_text()
    types = re.findall(r'struct (SM50_SHADER_\w+_DATA) \{(.*?)\n\};', public, re.S)
    cases = []
    for name, fields in types:
        if name == 'SM50_SHADER_COMPILATION_ARGUMENT_DATA': continue
        tag = name[:-5]
        if tag not in public.split('enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {')[1].split('};')[0]: continue
        checks = []
        if '*elements;' in fields: checks.append('raven_unix_checked(p->elements, p->num_elements, sizeof(*p->elements), 0);')
        if '* bytecode;' in fields or '*bytecode;' in fields: checks.append('raven_unix_checked(p->bytecode, p->bytecode_length, 1, 0);')
        if '*libraries;' in fields: checks.append('raven_unix_checked(p->libraries, p->count, sizeof(*p->libraries), 0);')
        if '*name;' in fields: checks.append('raven_unix_string(p->name);')
        if '*pixel_shader_bytecode;' in fields: checks.append('if (p->pixel_shader_bytecode) { const uint32_t *h = raven_unix_checked(p->pixel_shader_bytecode, 32, 1, 0); raven_unix_checked(h, h[6], 1, 0); }')
        label = f'case {tag}:' + (' case SM50_SHADER_ROOT_SIGNATURE2:' if tag == 'SM50_SHADER_ROOT_SIGNATURE' else '')
        cases.append(label+f' {{ const struct {name} *p = raven_unix_checked(node, 1, sizeof(*p), 0); '+ ' '.join(checks) + ' break; }')
    return '''
static void raven_shader_arguments(void *args) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *node = args;
    unsigned limit = 4096;
    while (node) {
        if (!limit--) { raven_unix_checked(NULL, 1, 1, 0); return; }
        raven_unix_checked(node, 1, sizeof(*node), 0);
        switch (node->type) {
'''+ '\n'.join(cases)+'''
        default: raven_unix_checked(NULL, 1, 1, 0); return;
        }
        node = node->next;
    }
}
static void *raven_function_constant(const void *pointer, MTLDataType type) {
    // Metal scalar/vector function constants: each lane uses its scalar width.
    unsigned size;
    if (type >= MTLDataTypeFloat && type <= MTLDataTypeFloat4) size = 4 * (type - MTLDataTypeFloat + 1);
    else if (type >= MTLDataTypeHalf && type <= MTLDataTypeHalf4) size = 2 * (type - MTLDataTypeHalf + 1);
    else if (type >= MTLDataTypeInt && type <= MTLDataTypeInt4) size = 4 * (type - MTLDataTypeInt + 1);
    else if (type >= MTLDataTypeUInt && type <= MTLDataTypeUInt4) size = 4 * (type - MTLDataTypeUInt + 1);
    else if (type >= MTLDataTypeShort && type <= MTLDataTypeShort4) size = 2 * (type - MTLDataTypeShort + 1);
    else if (type >= MTLDataTypeUShort && type <= MTLDataTypeUShort4) size = 2 * (type - MTLDataTypeUShort + 1);
    else if (type >= MTLDataTypeChar && type <= MTLDataTypeChar4) size = type - MTLDataTypeChar + 1;
    else if (type >= MTLDataTypeUChar && type <= MTLDataTypeUChar4) size = type - MTLDataTypeUChar + 1;
    else if (type >= MTLDataTypeBool && type <= MTLDataTypeBool4) size = type - MTLDataTypeBool + 1;
    else { raven_unix_checked(NULL, 1, 1, 0); return NULL; }
    return raven_unix_checked(pointer, size, 1, 0);
}
'''

if __name__=='__main__':
    import sys
    print(harden(Path(sys.argv[1])))
