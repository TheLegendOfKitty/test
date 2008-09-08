/*
 * Copyright 2008 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "jscript.h"

#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(jscript);

/*
 * This IID is used to get DispatchEx objecto from interface.
 * We might consider using private insteface instead.
 */
static const IID IID_IDispatchJS =
        {0x719c3050,0xf9d3,0x11cf,{0xa4,0x93,0x00,0x40,0x05,0x23,0xa8,0xa6}};

typedef enum {
    PROP_VARIANT,
    PROP_BUILTIN,
    PROP_PROTREF,
    PROP_DELETED
} prop_type_t;

struct _dispex_prop_t {
    WCHAR *name;
    prop_type_t type;
    DWORD flags;

    union {
        VARIANT var;
        const builtin_prop_t *p;
        DWORD ref;
    } u;
};

static inline DISPID prop_to_id(DispatchEx *This, dispex_prop_t *prop)
{
    return prop - This->props;
}

static inline dispex_prop_t *get_prop(DispatchEx *This, DISPID id)
{
    if(id < 0 || id >= This->prop_cnt || This->props[id].type == PROP_DELETED)
        return NULL;

    return This->props+id;
}

static DWORD get_flags(DispatchEx *This, dispex_prop_t *prop)
{
    if(prop->type == PROP_PROTREF) {
        dispex_prop_t *parent = get_prop(This->prototype, prop->u.ref);
        if(!parent) {
            prop->type = PROP_DELETED;
            return 0;
        }

        return get_flags(This->prototype, parent);
    }

    return prop->flags;
}

static const builtin_prop_t *find_builtin_prop(DispatchEx *This, const WCHAR *name)
{
    int min = 0, max, i, r;

    max = This->builtin_info->props_cnt-1;
    while(min <= max) {
        i = (min+max)/2;

        r = strcmpW(name, This->builtin_info->props[i].name);
        if(!r)
            return This->builtin_info->props + i;

        if(r < 0)
            max = i-1;
        else
            min = i+1;
    }

    return NULL;
}

static dispex_prop_t *alloc_prop(DispatchEx *This, const WCHAR *name, prop_type_t type, DWORD flags)
{
    dispex_prop_t *ret;

    if(This->buf_size == This->prop_cnt) {
        dispex_prop_t *tmp = heap_realloc(This->props, (This->buf_size<<=1)*sizeof(*This->props));
        if(!tmp)
            return NULL;
        This->props = tmp;
    }

    ret = This->props + This->prop_cnt++;
    ret->type = type;
    ret->flags = flags;
    ret->name = heap_strdupW(name);
    if(!ret->name)
        return NULL;

    return ret;
}

static dispex_prop_t *alloc_protref(DispatchEx *This, const WCHAR *name, DWORD ref)
{
    dispex_prop_t *ret;

    ret = alloc_prop(This, name, PROP_PROTREF, 0);
    if(!ret)
        return NULL;

    ret->u.ref = ref;
    return ret;
}

static HRESULT find_prop_name(DispatchEx *This, const WCHAR *name, dispex_prop_t **ret)
{
    const builtin_prop_t *builtin;
    dispex_prop_t *prop;

    for(prop = This->props; prop < This->props+This->prop_cnt; prop++) {
        if(prop->name && !strcmpW(prop->name, name)) {
            *ret = prop;
            return S_OK;
        }
    }

    builtin = find_builtin_prop(This, name);
    if(builtin) {
        prop = alloc_prop(This, name, PROP_BUILTIN, builtin->flags);
        if(!prop)
            return E_OUTOFMEMORY;

        prop->u.p = builtin;
        *ret = prop;
        return S_OK;
    }

    *ret = NULL;
    return S_OK;
}

static HRESULT find_prop_name_prot(DispatchEx *This, const WCHAR *name, BOOL alloc, dispex_prop_t **ret)
{
    dispex_prop_t *prop;
    HRESULT hres;

    hres = find_prop_name(This, name, &prop);
    if(FAILED(hres))
        return hres;
    if(prop) {
        *ret = prop;
        return S_OK;
    }

    if(This->prototype) {
        hres = find_prop_name_prot(This->prototype, name, FALSE, &prop);
        if(FAILED(hres))
            return hres;
        if(prop) {
            prop = alloc_protref(This, prop->name, prop - This->prototype->props);
            if(!prop)
                return E_OUTOFMEMORY;
            *ret = prop;
            return S_OK;
        }
    }

    if(alloc) {
        TRACE("creating prop %s\n", debugstr_w(name));

        prop = alloc_prop(This, name, PROP_VARIANT, PROPF_ENUM);
        if(!prop)
            return E_OUTOFMEMORY;
        VariantInit(&prop->u.var);
    }

    *ret = prop;
    return S_OK;
}

static HRESULT set_this(DISPPARAMS *dp, DISPPARAMS *olddp, IDispatch *jsthis)
{
    VARIANTARG *oldargs;
    int i;

    static DISPID this_id = DISPID_THIS;

    *dp = *olddp;

    for(i = 0; i < dp->cNamedArgs; i++) {
        if(dp->rgdispidNamedArgs[i] == DISPID_THIS)
            return S_OK;
    }

    oldargs = dp->rgvarg;
    dp->rgvarg = heap_alloc((dp->cArgs+1) * sizeof(VARIANTARG));
    if(!dp->rgvarg)
        return E_OUTOFMEMORY;
    memcpy(dp->rgvarg+1, oldargs, dp->cArgs*sizeof(VARIANTARG));
    V_VT(dp->rgvarg) = VT_DISPATCH;
    V_DISPATCH(dp->rgvarg) = jsthis;
    dp->cArgs++;

    if(dp->cNamedArgs) {
        DISPID *old = dp->rgdispidNamedArgs;
        dp->rgdispidNamedArgs = heap_alloc((dp->cNamedArgs+1)*sizeof(DISPID));
        if(!dp->rgdispidNamedArgs) {
            heap_free(dp->rgvarg);
            return E_OUTOFMEMORY;
        }

        memcpy(dp->rgdispidNamedArgs+1, old, dp->cNamedArgs*sizeof(DISPID));
        dp->rgdispidNamedArgs[0] = DISPID_THIS;
        dp->cNamedArgs++;
    }else {
        dp->rgdispidNamedArgs = &this_id;
        dp->cNamedArgs = 1;
    }

    return S_OK;
}

static HRESULT invoke_prop_func(DispatchEx *This, DispatchEx *jsthis, dispex_prop_t *prop, LCID lcid, WORD flags,
        DISPPARAMS *dp, VARIANT *retv, jsexcept_t *ei, IServiceProvider *caller)
{
    HRESULT hres;

    switch(prop->type) {
    case PROP_BUILTIN:
        if(flags == DISPATCH_CONSTRUCT && (prop->flags & DISPATCH_METHOD)) {
            WARN("%s is not a constructor\n", debugstr_w(prop->name));
            return E_INVALIDARG;
        }
        return prop->u.p->invoke(jsthis, lcid, flags, dp, retv, ei, caller);
    case PROP_PROTREF:
        return invoke_prop_func(This->prototype, jsthis, This->prototype->props+prop->u.ref, lcid, flags, dp, retv, ei, caller);
    case PROP_VARIANT: {
        DISPPARAMS new_dp;

        if(V_VT(&prop->u.var) != VT_DISPATCH) {
            FIXME("invoke vt %d\n", V_VT(&prop->u.var));
            return E_FAIL;
        }

        TRACE("call %s %p\n", debugstr_w(prop->name), V_DISPATCH(&prop->u.var));

        hres = set_this(&new_dp, dp, (IDispatch*)_IDispatchEx_(jsthis));
        if(FAILED(hres))
            return hres;

        hres = disp_call(V_DISPATCH(&prop->u.var), DISPID_VALUE, lcid, flags, &new_dp, retv, ei, caller);

        if(new_dp.rgvarg != dp->rgvarg) {
            heap_free(new_dp.rgvarg);
            if(new_dp.cNamedArgs > 1)
                heap_free(new_dp.rgdispidNamedArgs);
        }

        return hres;
    }
    default:
        ERR("type %d\n", prop->type);
    }

    return E_FAIL;
}

static HRESULT prop_get(DispatchEx *This, dispex_prop_t *prop, LCID lcid, DISPPARAMS *dp,
        VARIANT *retv, jsexcept_t *ei, IServiceProvider *caller)
{
    HRESULT hres;

    switch(prop->type) {
    case PROP_BUILTIN:
        if(prop->u.p->flags & PROPF_METHOD) {
            FIXME("function objects not supported\n");
            return E_NOTIMPL;
        }else {
            hres = prop->u.p->invoke(This, lcid, DISPATCH_PROPERTYGET, dp, retv, ei, caller);
        }
        break;
    case PROP_PROTREF:
        hres = prop_get(This->prototype, This->prototype->props+prop->u.ref, lcid, dp, retv, ei, caller);
        break;
    case PROP_VARIANT:
        hres = VariantCopy(retv, &prop->u.var);
        break;
    default:
        ERR("type %d\n", prop->type);
        return E_FAIL;
    }

    if(FAILED(hres)) {
        TRACE("fail %08x\n", hres);
        return hres;
    }

    TRACE("%s ret %s\n", debugstr_w(prop->name), debugstr_variant(retv));
    return hres;
}

static HRESULT prop_put(DispatchEx *This, dispex_prop_t *prop, LCID lcid, DISPPARAMS *dp,
        jsexcept_t *ei, IServiceProvider *caller)
{
    DWORD i;
    HRESULT hres;

    switch(prop->type) {
    case PROP_BUILTIN:
        if(!(prop->flags & PROPF_METHOD))
            return prop->u.p->invoke(This, lcid, DISPATCH_PROPERTYPUT, dp, NULL, ei, caller);
    case PROP_PROTREF:
        prop->type = PROP_VARIANT;
        prop->flags = PROPF_ENUM;
        V_VT(&prop->u.var) = VT_EMPTY;
        break;
    case PROP_VARIANT:
        VariantClear(&prop->u.var);
        break;
    default:
        ERR("type %d\n", prop->type);
        return E_FAIL;
    }

    for(i=0; i < dp->cNamedArgs; i++) {
        if(dp->rgdispidNamedArgs[i] == DISPID_PROPERTYPUT)
            break;
    }

    if(i == dp->cNamedArgs) {
        TRACE("no value to set\n");
        return DISP_E_PARAMNOTOPTIONAL;
    }

    hres = VariantCopy(&prop->u.var, dp->rgvarg+i);
    if(FAILED(hres))
        return hres;

    if(This->builtin_info->on_put)
        This->builtin_info->on_put(This, prop->name);

    TRACE("%s = %s\n", debugstr_w(prop->name), debugstr_variant(dp->rgvarg+i));
    return S_OK;
}

static HRESULT fill_protrefs(DispatchEx *This)
{
    dispex_prop_t *iter, *prop;
    HRESULT hres;

    if(!This->prototype)
        return S_OK;

    fill_protrefs(This->prototype);

    for(iter = This->prototype->props; iter < This->prototype->props+This->prototype->prop_cnt; iter++) {
        hres = find_prop_name(This, iter->name, &prop);
        if(FAILED(hres))
            return hres;
        if(!prop) {
            prop = alloc_protref(This, iter->name, iter - This->prototype->props);
            if(!prop)
                return E_OUTOFMEMORY;
        }
    }

    return S_OK;
}

#define DISPATCHEX_THIS(iface) DEFINE_THIS(DispatchEx, IDispatchEx, iface)

static HRESULT WINAPI DispatchEx_QueryInterface(IDispatchEx *iface, REFIID riid, void **ppv)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = _IDispatchEx_(This);
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        TRACE("(%p)->(IID_IDispatch %p)\n", This, ppv);
        *ppv = _IDispatchEx_(This);
    }else if(IsEqualGUID(&IID_IDispatchEx, riid)) {
        TRACE("(%p)->(IID_IDispatchEx %p)\n", This, ppv);
        *ppv = _IDispatchEx_(This);
    }else if(IsEqualGUID(&IID_IDispatchJS, riid)) {
        TRACE("(%p)->(IID_IDispatchJS %p)\n", This, ppv);
        IUnknown_AddRef(_IDispatchEx_(This));
        *ppv = This;
        return S_OK;
    }else {
        WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI DispatchEx_AddRef(IDispatchEx *iface)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    return ref;
}

static ULONG WINAPI DispatchEx_Release(IDispatchEx *iface)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    if(!ref) {
        dispex_prop_t *prop;

        for(prop = This->props; prop < This->props+This->prop_cnt; prop++) {
            if(prop->type == PROP_VARIANT)
                VariantClear(&prop->u.var);
            heap_free(prop->name);
        }
        heap_free(This->props);
        script_release(This->ctx);

        if(This->builtin_info->destructor)
            This->builtin_info->destructor(This);
        else
            heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI DispatchEx_GetTypeInfoCount(IDispatchEx *iface, UINT *pctinfo)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);

    TRACE("(%p)->(%p)\n", This, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetTypeInfo(IDispatchEx *iface, UINT iTInfo,
                                              LCID lcid, ITypeInfo **ppTInfo)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%u %u %p)\n", This, iTInfo, lcid, ppTInfo);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetIDsOfNames(IDispatchEx *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    UINT i;
    HRESULT hres;

    TRACE("(%p)->(%s %p %u %u %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    for(i=0; i < cNames; i++) {
        hres = IDispatchEx_GetDispID(_IDispatchEx_(This), rgszNames[i], 0, rgDispId+i);
        if(FAILED(hres))
            return hres;
    }

    return S_OK;
}

static HRESULT WINAPI DispatchEx_Invoke(IDispatchEx *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);

    TRACE("(%p)->(%d %s %d %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    return IDispatchEx_InvokeEx(_IDispatchEx_(This), dispIdMember, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, NULL);
}

static HRESULT WINAPI DispatchEx_GetDispID(IDispatchEx *iface, BSTR bstrName, DWORD grfdex, DISPID *pid)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    dispex_prop_t *prop;
    HRESULT hres;

    TRACE("(%p)->(%s %x %p)\n", This, debugstr_w(bstrName), grfdex, pid);

    if(grfdex & ~(fdexNameCaseSensitive|fdexNameEnsure|fdexNameImplicit)) {
        FIXME("Unsupported grfdex %x\n", grfdex);
        return E_NOTIMPL;
    }

    hres = find_prop_name_prot(This, bstrName, (grfdex&fdexNameEnsure) != 0, &prop);
    if(FAILED(hres))
        return hres;
    if(prop) {
        *pid = prop_to_id(This, prop);
        return S_OK;
    }

    TRACE("not found %s\n", debugstr_w(bstrName));
    return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI DispatchEx_InvokeEx(IDispatchEx *iface, DISPID id, LCID lcid, WORD wFlags, DISPPARAMS *pdp,
        VARIANT *pvarRes, EXCEPINFO *pei, IServiceProvider *pspCaller)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    dispex_prop_t *prop;
    jsexcept_t jsexcept;
    HRESULT hres;

    TRACE("(%p)->(%x %x %x %p %p %p %p)\n", This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);

    if(pvarRes)
        V_VT(pvarRes) = VT_EMPTY;

    prop = get_prop(This, id);
    if(!prop || prop->type == PROP_DELETED) {
        TRACE("invalid id\n");
        return DISP_E_MEMBERNOTFOUND;
    }

    memset(&jsexcept, 0, sizeof(jsexcept));

    switch(wFlags) {
    case DISPATCH_METHOD:
    case DISPATCH_CONSTRUCT:
        hres = invoke_prop_func(This, This, prop, lcid, wFlags, pdp, pvarRes, &jsexcept, pspCaller);
        break;
    case DISPATCH_PROPERTYGET:
        hres = prop_get(This, prop, lcid, pdp, pvarRes, &jsexcept, pspCaller);
        break;
    case DISPATCH_PROPERTYPUT:
        hres = prop_put(This, prop, lcid, pdp, &jsexcept, pspCaller);
        break;
    default:
        FIXME("Unimplemented flags %x\n", wFlags);
        return E_INVALIDARG;
    }

    if(pei)
        *pei = jsexcept.ei;

    return hres;
}

static HRESULT WINAPI DispatchEx_DeleteMemberByName(IDispatchEx *iface, BSTR bstrName, DWORD grfdex)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%s %x)\n", This, debugstr_w(bstrName), grfdex);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_DeleteMemberByDispID(IDispatchEx *iface, DISPID id)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%x)\n", This, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetMemberProperties(IDispatchEx *iface, DISPID id, DWORD grfdexFetch, DWORD *pgrfdex)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%x %x %p)\n", This, id, grfdexFetch, pgrfdex);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetMemberName(IDispatchEx *iface, DISPID id, BSTR *pbstrName)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%x %p)\n", This, id, pbstrName);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetNextDispID(IDispatchEx *iface, DWORD grfdex, DISPID id, DISPID *pid)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    dispex_prop_t *iter;
    HRESULT hres;

    TRACE("(%p)->(%x %x %p)\n", This, grfdex, id, pid);

    if(id == DISPID_STARTENUM) {
        hres = fill_protrefs(This);
        if(FAILED(hres))
            return hres;
    }

    iter = get_prop(This, id+1);
    if(!iter) {
        *pid = DISPID_STARTENUM;
        return S_FALSE;
    }

    while(iter < This->props + This->prop_cnt) {
        if(iter->name && (get_flags(This, iter) & PROPF_ENUM)) {
            *pid = prop_to_id(This, iter);
            return S_OK;
        }
        iter++;
    }

    *pid = DISPID_STARTENUM;
    return S_FALSE;
}

static HRESULT WINAPI DispatchEx_GetNameSpaceParent(IDispatchEx *iface, IUnknown **ppunk)
{
    DispatchEx *This = DISPATCHEX_THIS(iface);
    FIXME("(%p)->(%p)\n", This, ppunk);
    return E_NOTIMPL;
}

#undef DISPATCHEX_THIS

static IDispatchExVtbl DispatchExVtbl = {
    DispatchEx_QueryInterface,
    DispatchEx_AddRef,
    DispatchEx_Release,
    DispatchEx_GetTypeInfoCount,
    DispatchEx_GetTypeInfo,
    DispatchEx_GetIDsOfNames,
    DispatchEx_Invoke,
    DispatchEx_GetDispID,
    DispatchEx_InvokeEx,
    DispatchEx_DeleteMemberByName,
    DispatchEx_DeleteMemberByDispID,
    DispatchEx_GetMemberProperties,
    DispatchEx_GetMemberName,
    DispatchEx_GetNextDispID,
    DispatchEx_GetNameSpaceParent
};

static HRESULT jsdisp_set_prot_prop(DispatchEx *dispex, DispatchEx *prototype)
{
    VARIANT *var;

    if(!dispex->props[1].name)
        return E_OUTOFMEMORY;

    dispex->props[1].type = PROP_VARIANT;
    dispex->props[1].flags = 0;

    var = &dispex->props[1].u.var;
    V_VT(var) = VT_DISPATCH;
    V_DISPATCH(var) = (IDispatch*)_IDispatchEx_(prototype);

    return S_OK;
}

static HRESULT init_dispex(DispatchEx *dispex, script_ctx_t *ctx, const builtin_info_t *builtin_info, DispatchEx *prototype)
{
    static const WCHAR prototypeW[] = {'p','r','o','t','o','t','y','p','e',0};

    TRACE("%p (%p)\n", dispex, prototype);

    dispex->lpIDispatchExVtbl = &DispatchExVtbl;
    dispex->ref = 1;
    dispex->builtin_info = builtin_info;

    dispex->props = heap_alloc((dispex->buf_size=4) * sizeof(dispex_prop_t));
    if(!dispex->props)
        return E_OUTOFMEMORY;

    dispex->prototype = prototype;
    if(prototype)
        IDispatchEx_AddRef(_IDispatchEx_(prototype));

    dispex->prop_cnt = 2;
    dispex->props[0].name = NULL;
    dispex->props[0].flags = 0;
    if(builtin_info->value_prop.invoke) {
        dispex->props[0].type = PROP_BUILTIN;
        dispex->props[0].u.p = &builtin_info->value_prop;
    }else {
        dispex->props[0].type = PROP_DELETED;
    }

    dispex->props[1].type = PROP_DELETED;
    dispex->props[1].name = SysAllocString(prototypeW);
    dispex->props[1].flags = 0;

    if(prototype) {
        HRESULT hres;

        hres = jsdisp_set_prot_prop(dispex, prototype);
        if(FAILED(hres)) {
            IDispatchEx_Release(_IDispatchEx_(dispex));
            return hres;
        }
    }

    script_addref(ctx);
    dispex->ctx = ctx;

    return S_OK;
}

static const builtin_info_t dispex_info = {
    JSCLASS_NONE,
    {NULL, NULL, 0},
    0, NULL,
    NULL,
    NULL
};

HRESULT create_dispex(script_ctx_t *ctx, const builtin_info_t *builtin_info, DispatchEx *prototype, DispatchEx **dispex)
{
    DispatchEx *ret;
    HRESULT hres;

    ret = heap_alloc_zero(sizeof(DispatchEx));
    if(!ret)
        return E_OUTOFMEMORY;

    hres = init_dispex(ret, ctx, builtin_info ? builtin_info : &dispex_info, prototype);
    if(FAILED(hres))
        return hres;

    *dispex = ret;
    return S_OK;
}

DispatchEx *iface_to_jsdisp(IUnknown *iface)
{
    DispatchEx *ret;
    HRESULT hres;

    hres = IUnknown_QueryInterface(iface, &IID_IDispatchJS, (void**)&ret);
    if(FAILED(hres))
        return NULL;

    return ret;
}

HRESULT jsdisp_call(DispatchEx *disp, DISPID id, LCID lcid, WORD flags, DISPPARAMS *dp, VARIANT *retv,
        jsexcept_t *ei, IServiceProvider *caller)
{
    dispex_prop_t *prop;

    memset(ei, 0, sizeof(*ei));
    if(retv)
        V_VT(retv) = VT_EMPTY;

    prop = get_prop(disp, id);
    if(!prop)
        return DISP_E_MEMBERNOTFOUND;

    return invoke_prop_func(disp, disp, prop, lcid, flags, dp, retv, ei, caller);
}

HRESULT disp_call(IDispatch *disp, DISPID id, LCID lcid, WORD flags, DISPPARAMS *dp, VARIANT *retv,
        jsexcept_t *ei, IServiceProvider *caller)
{
    DispatchEx *jsdisp;
    IDispatchEx *dispex;
    HRESULT hres;

    jsdisp = iface_to_jsdisp((IUnknown*)disp);
    if(jsdisp) {
        hres = jsdisp_call(jsdisp, id, lcid, flags, dp, retv, ei, caller);
        IDispatchEx_Release(_IDispatchEx_(jsdisp));
        return hres;
    }

    memset(ei, 0, sizeof(*ei));

    if(retv)
        V_VT(retv) = VT_EMPTY;
    hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    if(FAILED(hres)) {
        UINT err = 0;

        TRACE("using IDispatch\n");
        return IDispatch_Invoke(disp, id, &IID_NULL, lcid, flags, dp, retv, &ei->ei, &err);
    }

    hres = IDispatchEx_InvokeEx(dispex, id, lcid, flags, dp, retv, &ei->ei, caller);
    IDispatchEx_Release(dispex);

    return hres;
}
