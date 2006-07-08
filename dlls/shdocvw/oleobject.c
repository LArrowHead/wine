/*
 * Implementation of IOleObject interfaces for WebBrowser control
 *
 * - IOleObject
 * - IOleInPlaceObject
 * - IOleControl
 *
 * Copyright 2001 John R. Sheets (for CodeWeavers)
 * Copyright 2005 Jacek Caban
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

#include <string.h>
#include "wine/debug.h"
#include "shdocvw.h"
#include "htiframe.h"

WINE_DEFAULT_DEBUG_CHANNEL(shdocvw);

static ATOM shell_embedding_atom = 0;

static LRESULT resize_window(WebBrowser *This, LONG width, LONG height)
{
    if(This->doc_host.hwnd)
        SetWindowPos(This->doc_host.hwnd, NULL, 0, 0, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    return 0;
}

static LRESULT WINAPI shell_embedding_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WebBrowser *This;

    static const WCHAR wszTHIS[] = {'T','H','I','S',0};

    if(msg == WM_CREATE) {
        This = *(WebBrowser**)lParam;
        SetPropW(hwnd, wszTHIS, This);
    }else {
        This = GetPropW(hwnd, wszTHIS);
    }

    switch(msg) {
    case WM_SIZE:
        return resize_window(This, LOWORD(lParam), HIWORD(lParam));
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void create_shell_embedding_hwnd(WebBrowser *This)
{
    IOleInPlaceSite *inplace;
    HWND parent = NULL;
    HRESULT hres;

    static const WCHAR wszShellEmbedding[] =
        {'S','h','e','l','l',' ','E','m','b','e','d','d','i','n','g',0};

    if(!shell_embedding_atom) {
        static WNDCLASSEXW wndclass = {
            sizeof(wndclass),
            CS_DBLCLKS,
            shell_embedding_proc,
            0, 0 /* native uses 8 */, NULL, NULL, NULL,
            (HBRUSH)COLOR_WINDOWFRAME, NULL,
            wszShellEmbedding,
            NULL
        };
        wndclass.hInstance = shdocvw_hinstance;

        RegisterClassExW(&wndclass);
    }

    hres = IOleClientSite_QueryInterface(This->client, &IID_IOleInPlaceSite, (void**)&inplace);
    if(SUCCEEDED(hres)) {
        IOleInPlaceSite_GetWindow(inplace, &parent);
        IOleInPlaceSite_Release(inplace);
    }

    This->doc_host.frame_hwnd = This->shell_embedding_hwnd = CreateWindowExW(
            WS_EX_WINDOWEDGE,
            wszShellEmbedding, wszShellEmbedding,
            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
            0, 0, 0, 0, parent,
            NULL, shdocvw_hinstance, This);
}

static HRESULT activate_inplace(WebBrowser *This, IOleClientSite *active_site, HWND parent_hwnd)
{
    HRESULT hres;

    if(This->inplace)
        return S_OK;

    if(!active_site)
        return E_INVALIDARG;

    hres = IOleClientSite_QueryInterface(active_site, &IID_IOleInPlaceSite,
                                         (void**)&This->inplace);
    if(FAILED(hres)) {
        WARN("Could not get IOleInPlaceSite\n");
        return hres;
    }

    hres = IOleInPlaceSite_CanInPlaceActivate(This->inplace);
    if(hres != S_OK) {
        WARN("CanInPlaceActivate returned: %08lx\n", hres);
        IOleInPlaceSite_Release(This->inplace);
        return E_FAIL;
    }

    hres = IOleInPlaceSite_GetWindow(This->inplace, &This->iphwnd);
    if(FAILED(hres))
        This->iphwnd = parent_hwnd;

    IOleInPlaceSite_OnInPlaceActivate(This->inplace);

    IOleInPlaceSite_GetWindowContext(This->inplace, &This->frame, &This->uiwindow,
                                     &This->pos_rect, &This->clip_rect,
                                     &This->frameinfo);

    SetWindowPos(This->shell_embedding_hwnd, NULL,
                 This->pos_rect.left, This->pos_rect.top,
                 This->pos_rect.right-This->pos_rect.left,
                 This->pos_rect.bottom-This->pos_rect.top,
                 SWP_NOZORDER | SWP_SHOWWINDOW);

    if(This->client) {
        IOleClientSite_ShowObject(This->client);
        IOleClientSite_GetContainer(This->client, &This->container);
    }

    if(This->frame)
        IOleInPlaceFrame_GetWindow(This->frame, &This->frame_hwnd);

    return S_OK;
}

static HRESULT activate_ui(WebBrowser *This, IOleClientSite *active_site, HWND parent_hwnd)
{
    HRESULT hres;

    static const WCHAR wszitem[] = {'i','t','e','m',0};

    if(This->inplace)
        return S_OK;

    hres = activate_inplace(This, active_site, parent_hwnd);
    if(FAILED(hres))
        return hres;

    IOleInPlaceSite_OnUIActivate(This->inplace);

    if(This->frame)
        IOleInPlaceFrame_SetActiveObject(This->frame, ACTIVEOBJ(This), wszitem);
    if(This->uiwindow)
        IOleInPlaceUIWindow_SetActiveObject(This->uiwindow, ACTIVEOBJ(This), wszitem);

    if(This->frame)
        IOleInPlaceFrame_SetMenu(This->frame, NULL, NULL, This->shell_embedding_hwnd);

    SetFocus(This->shell_embedding_hwnd);

    return S_OK;
}

/**********************************************************************
 * Implement the IOleObject interface for the WebBrowser control
 */

#define OLEOBJ_THIS(iface) DEFINE_THIS(WebBrowser, OleObject, iface)

static HRESULT WINAPI OleObject_QueryInterface(IOleObject *iface, REFIID riid, void **ppv)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    return IWebBrowser_QueryInterface(WEBBROWSER(This), riid, ppv);
}

static ULONG WINAPI OleObject_AddRef(IOleObject *iface)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    return IWebBrowser_AddRef(WEBBROWSER(This));
}

static ULONG WINAPI OleObject_Release(IOleObject *iface)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    return IWebBrowser_Release(WEBBROWSER(This));
}

static HRESULT WINAPI OleObject_SetClientSite(IOleObject *iface, LPOLECLIENTSITE pClientSite)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    IOleContainer *container;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, pClientSite);

    if(This->client == pClientSite)
        return S_OK;

    if(This->doc_host.hwnd) {
        DestroyWindow(This->doc_host.hwnd);
        This->doc_host.hwnd = NULL;
    }
    if(This->shell_embedding_hwnd) {
        DestroyWindow(This->shell_embedding_hwnd);
        This->shell_embedding_hwnd = NULL;
    }

    if(This->inplace) {
        IOleInPlaceSite_Release(This->inplace);
        This->inplace = NULL;
    }

    if(This->doc_host.hostui)
        IDocHostUIHandler_Release(This->doc_host.hostui);
    if(This->client)
        IOleClientSite_Release(This->client);

    if(!pClientSite) {
        if(This->doc_host.document)
            deactivate_document(&This->doc_host);
        This->client = NULL;
        return S_OK;
    }

    This->client = pClientSite;
    IOleClientSite_AddRef(pClientSite);

    hres = IOleClientSite_QueryInterface(This->client, &IID_IDocHostUIHandler,
                                         (void**)&This->doc_host.hostui);
    if(FAILED(hres))
        This->doc_host.hostui = NULL;

    hres = IOleClientSite_GetContainer(This->client, &container);
    if(SUCCEEDED(hres)) {
        ITargetContainer *target_container;

        hres = IOleContainer_QueryInterface(container, &IID_ITargetContainer,
                                            (void**)&target_container);
        if(SUCCEEDED(hres)) {
            FIXME("Unsupported ITargetContainer\n");
            ITargetContainer_Release(target_container);
        }

        IOleContainer_Release(container);
    }

    create_shell_embedding_hwnd(This);

    return S_OK;
}

static HRESULT WINAPI OleObject_GetClientSite(IOleObject *iface, LPOLECLIENTSITE *ppClientSite)
{
    WebBrowser *This = OLEOBJ_THIS(iface);

    TRACE("(%p)->(%p)\n", This, ppClientSite);

    if(!ppClientSite)
        return E_INVALIDARG;

    if(This->client)
        IOleClientSite_AddRef(This->client);
    *ppClientSite = This->client;

    return S_OK;
}

static HRESULT WINAPI OleObject_SetHostNames(IOleObject *iface, LPCOLESTR szContainerApp,
        LPCOLESTR szContainerObj)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%s, %s)\n", This, debugstr_w(szContainerApp), debugstr_w(szContainerObj));
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_Close(IOleObject *iface, DWORD dwSaveOption)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%ld)\n", This, dwSaveOption);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_SetMoniker(IOleObject *iface, DWORD dwWhichMoniker, IMoniker* pmk)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%ld, %p)\n", This, dwWhichMoniker, pmk);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_GetMoniker(IOleObject *iface, DWORD dwAssign,
        DWORD dwWhichMoniker, LPMONIKER *ppmk)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%ld, %ld, %p)\n", This, dwAssign, dwWhichMoniker, ppmk);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_InitFromData(IOleObject *iface, LPDATAOBJECT pDataObject,
        BOOL fCreation, DWORD dwReserved)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%p, %d, %ld)\n", This, pDataObject, fCreation, dwReserved);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_GetClipboardData(IOleObject *iface, DWORD dwReserved,
        LPDATAOBJECT *ppDataObject)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%ld, %p)\n", This, dwReserved, ppDataObject);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_DoVerb(IOleObject *iface, LONG iVerb, struct tagMSG* lpmsg,
        LPOLECLIENTSITE pActiveSite, LONG lindex, HWND hwndParent, LPCRECT lprcPosRect)
{
    WebBrowser *This = OLEOBJ_THIS(iface);

    TRACE("(%p)->(%ld %p %p %ld %p %p)\n", This, iVerb, lpmsg, pActiveSite, lindex, hwndParent,
            lprcPosRect);

    switch (iVerb)
    {
    case OLEIVERB_SHOW:
        TRACE("OLEIVERB_SHOW\n");
        return activate_ui(This, pActiveSite, hwndParent);
    case OLEIVERB_UIACTIVATE:
        TRACE("OLEIVERB_UIACTIVATE\n");
        return activate_ui(This, pActiveSite, hwndParent);
    case OLEIVERB_INPLACEACTIVATE:
        TRACE("OLEIVERB_INPLACEACTIVATE\n");
        return activate_inplace(This, pActiveSite, hwndParent);
    default:
        FIXME("stub for %ld\n", iVerb);
        break;
    }

    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_EnumVerbs(IOleObject *iface, IEnumOLEVERB **ppEnumOleVerb)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    TRACE("(%p)->(%p)\n", This, ppEnumOleVerb);
    return OleRegEnumVerbs(&CLSID_WebBrowser, ppEnumOleVerb);
}

static HRESULT WINAPI OleObject_Update(IOleObject *iface)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_IsUpToDate(IOleObject *iface)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_GetUserClassID(IOleObject *iface, CLSID* pClsid)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%p)\n", This, pClsid);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_GetUserType(IOleObject *iface, DWORD dwFormOfType,
        LPOLESTR* pszUserType)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    TRACE("(%p, %ld, %p)\n", This, dwFormOfType, pszUserType);
    return OleRegGetUserType(&CLSID_WebBrowser, dwFormOfType, pszUserType);
}

static HRESULT WINAPI OleObject_SetExtent(IOleObject *iface, DWORD dwDrawAspect, SIZEL *psizel)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%lx %p)\n", This, dwDrawAspect, psizel);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_GetExtent(IOleObject *iface, DWORD dwDrawAspect, SIZEL *psizel)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%lx, %p)\n", This, dwDrawAspect, psizel);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_Advise(IOleObject *iface, IAdviseSink *pAdvSink,
        DWORD* pdwConnection)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%p, %p)\n", This, pAdvSink, pdwConnection);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_Unadvise(IOleObject *iface, DWORD dwConnection)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%ld)\n", This, dwConnection);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleObject_EnumAdvise(IOleObject *iface, IEnumSTATDATA **ppenumAdvise)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%p)\n", This, ppenumAdvise);
    return S_OK;
}

static HRESULT WINAPI OleObject_GetMiscStatus(IOleObject *iface, DWORD dwAspect, DWORD *pdwStatus)
{
    WebBrowser *This = OLEOBJ_THIS(iface);

    TRACE("(%p)->(%lx, %p)\n", This, dwAspect, pdwStatus);

    *pdwStatus = OLEMISC_SETCLIENTSITEFIRST|OLEMISC_ACTIVATEWHENVISIBLE|OLEMISC_INSIDEOUT
        |OLEMISC_CANTLINKINSIDE|OLEMISC_RECOMPOSEONRESIZE;

    return S_OK;
}

static HRESULT WINAPI OleObject_SetColorScheme(IOleObject *iface, LOGPALETTE* pLogpal)
{
    WebBrowser *This = OLEOBJ_THIS(iface);
    FIXME("(%p)->(%p)\n", This, pLogpal);
    return E_NOTIMPL;
}

#undef OLEOBJ_THIS

static const IOleObjectVtbl OleObjectVtbl =
{
    OleObject_QueryInterface,
    OleObject_AddRef,
    OleObject_Release,
    OleObject_SetClientSite,
    OleObject_GetClientSite,
    OleObject_SetHostNames,
    OleObject_Close,
    OleObject_SetMoniker,
    OleObject_GetMoniker,
    OleObject_InitFromData,
    OleObject_GetClipboardData,
    OleObject_DoVerb,
    OleObject_EnumVerbs,
    OleObject_Update,
    OleObject_IsUpToDate,
    OleObject_GetUserClassID,
    OleObject_GetUserType,
    OleObject_SetExtent,
    OleObject_GetExtent,
    OleObject_Advise,
    OleObject_Unadvise,
    OleObject_EnumAdvise,
    OleObject_GetMiscStatus,
    OleObject_SetColorScheme
};

/**********************************************************************
 * Implement the IOleInPlaceObject interface
 */

#define INPLACEOBJ_THIS(iface) DEFINE_THIS(WebBrowser, OleInPlaceObject, iface)

static HRESULT WINAPI OleInPlaceObject_QueryInterface(IOleInPlaceObject *iface,
        REFIID riid, LPVOID *ppobj)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    return IWebBrowser_QueryInterface(WEBBROWSER(This), riid, ppobj);
}

static ULONG WINAPI OleInPlaceObject_AddRef(IOleInPlaceObject *iface)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    return IWebBrowser_AddRef(WEBBROWSER(This));
}

static ULONG WINAPI OleInPlaceObject_Release(IOleInPlaceObject *iface)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    return IWebBrowser_Release(WEBBROWSER(This));
}

static HRESULT WINAPI OleInPlaceObject_GetWindow(IOleInPlaceObject *iface, HWND* phwnd)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);

    TRACE("(%p)->(%p)\n", This, phwnd);

    *phwnd = This->shell_embedding_hwnd;
    return S_OK;
}

static HRESULT WINAPI OleInPlaceObject_ContextSensitiveHelp(IOleInPlaceObject *iface,
        BOOL fEnterMode)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    FIXME("(%p)->(%x)\n", This, fEnterMode);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleInPlaceObject_InPlaceDeactivate(IOleInPlaceObject *iface)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleInPlaceObject_UIDeactivate(IOleInPlaceObject *iface)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleInPlaceObject_SetObjectRects(IOleInPlaceObject *iface,
        LPCRECT lprcPosRect, LPCRECT lprcClipRect)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);

    TRACE("(%p)->(%p %p)\n", This, lprcPosRect, lprcClipRect);

    memcpy(&This->pos_rect, lprcPosRect, sizeof(RECT));

    if(lprcClipRect)
        memcpy(&This->clip_rect, lprcClipRect, sizeof(RECT));

    if(This->shell_embedding_hwnd) {
        SetWindowPos(This->shell_embedding_hwnd, NULL,
                     lprcPosRect->left, lprcPosRect->top,
                     lprcPosRect->right-lprcPosRect->left,
                     lprcPosRect->bottom-lprcPosRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return S_OK;
}

static HRESULT WINAPI OleInPlaceObject_ReactivateAndUndo(IOleInPlaceObject *iface)
{
    WebBrowser *This = INPLACEOBJ_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

#undef INPLACEOBJ_THIS

static const IOleInPlaceObjectVtbl OleInPlaceObjectVtbl =
{
    OleInPlaceObject_QueryInterface,
    OleInPlaceObject_AddRef,
    OleInPlaceObject_Release,
    OleInPlaceObject_GetWindow,
    OleInPlaceObject_ContextSensitiveHelp,
    OleInPlaceObject_InPlaceDeactivate,
    OleInPlaceObject_UIDeactivate,
    OleInPlaceObject_SetObjectRects,
    OleInPlaceObject_ReactivateAndUndo
};

/**********************************************************************
 * Implement the IOleControl interface
 */

#define CONTROL_THIS(iface) DEFINE_THIS(WebBrowser, OleControl, iface)

static HRESULT WINAPI OleControl_QueryInterface(IOleControl *iface,
        REFIID riid, LPVOID *ppobj)
{
    WebBrowser *This = CONTROL_THIS(iface);
    return IWebBrowser_QueryInterface(WEBBROWSER(This), riid, ppobj);
}

static ULONG WINAPI OleControl_AddRef(IOleControl *iface)
{
    WebBrowser *This = CONTROL_THIS(iface);
    return IWebBrowser_AddRef(WEBBROWSER(This));
}

static ULONG WINAPI OleControl_Release(IOleControl *iface)
{
    WebBrowser *This = CONTROL_THIS(iface);
    return IWebBrowser_Release(WEBBROWSER(This));
}

static HRESULT WINAPI OleControl_GetControlInfo(IOleControl *iface, LPCONTROLINFO pCI)
{
    WebBrowser *This = CONTROL_THIS(iface);
    FIXME("(%p)->(%p)\n", This, pCI);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleControl_OnMnemonic(IOleControl *iface, struct tagMSG *pMsg)
{
    WebBrowser *This = CONTROL_THIS(iface);
    FIXME("(%p)->(%p)\n", This, pMsg);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleControl_OnAmbientPropertyChange(IOleControl *iface, DISPID dispID)
{
    WebBrowser *This = CONTROL_THIS(iface);
    FIXME("(%p)->(%ld)\n", This, dispID);
    return E_NOTIMPL;
}

static HRESULT WINAPI OleControl_FreezeEvents(IOleControl *iface, BOOL bFreeze)
{
    WebBrowser *This = CONTROL_THIS(iface);
    FIXME("(%p)->(%x)\n", This, bFreeze);
    return E_NOTIMPL;
}

#undef CONTROL_THIS

static const IOleControlVtbl OleControlVtbl =
{
    OleControl_QueryInterface,
    OleControl_AddRef,
    OleControl_Release,
    OleControl_GetControlInfo,
    OleControl_OnMnemonic,
    OleControl_OnAmbientPropertyChange,
    OleControl_FreezeEvents
};

#define ACTIVEOBJ_THIS(iface) DEFINE_THIS(WebBrowser, OleInPlaceActiveObject, iface)

static HRESULT WINAPI InPlaceActiveObject_QueryInterface(IOleInPlaceActiveObject *iface,
                                                            REFIID riid, void **ppv)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    return IWebBrowser2_QueryInterface(WEBBROWSER2(This), riid, ppv);
}

static ULONG WINAPI InPlaceActiveObject_AddRef(IOleInPlaceActiveObject *iface)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    return IWebBrowser2_AddRef(WEBBROWSER2(This));
}

static ULONG WINAPI InPlaceActiveObject_Release(IOleInPlaceActiveObject *iface)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    return IWebBrowser2_Release(WEBBROWSER2(This));
}

static HRESULT WINAPI InPlaceActiveObject_GetWindow(IOleInPlaceActiveObject *iface,
                                                    HWND *phwnd)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    return IOleInPlaceObject_GetWindow(INPLACEOBJ(This), phwnd);
}

static HRESULT WINAPI InPlaceActiveObject_ContextSensitiveHelp(IOleInPlaceActiveObject *iface,
                                                               BOOL fEnterMode)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    return IOleInPlaceObject_ContextSensitiveHelp(INPLACEOBJ(This), fEnterMode);
}

static HRESULT WINAPI InPlaceActiveObject_TranslateAccelerator(IOleInPlaceActiveObject *iface,
                                                               LPMSG lpmsg)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    FIXME("(%p)->(%p)\n", This, lpmsg);
    return E_NOTIMPL;
}

static HRESULT WINAPI InPlaceActiveObject_OnFrameWindowActivate(IOleInPlaceActiveObject *iface,
                                                                BOOL fActivate)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    FIXME("(%p)->(%x)\n", This, fActivate);
    return E_NOTIMPL;
}

static HRESULT WINAPI InPlaceActiveObject_OnDocWindowActivate(IOleInPlaceActiveObject *iface,
                                                              BOOL fActivate)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    FIXME("(%p)->(%x)\n", This, fActivate);
    return E_NOTIMPL;
}

static HRESULT WINAPI InPlaceActiveObject_ResizeBorder(IOleInPlaceActiveObject *iface,
        LPCRECT lprcBorder, IOleInPlaceUIWindow *pUIWindow, BOOL fFrameWindow)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    FIXME("(%p)->(%p %p %x)\n", This, lprcBorder, pUIWindow, fFrameWindow);
    return E_NOTIMPL;
}

static HRESULT WINAPI InPlaceActiveObject_EnableModeless(IOleInPlaceActiveObject *iface,
                                                         BOOL fEnable)
{
    WebBrowser *This = ACTIVEOBJ_THIS(iface);
    FIXME("(%p)->(%x)\n", This, fEnable);
    return E_NOTIMPL;
}

#undef ACTIVEOBJ_THIS

static const IOleInPlaceActiveObjectVtbl OleInPlaceActiveObjectVtbl = {
    InPlaceActiveObject_QueryInterface,
    InPlaceActiveObject_AddRef,
    InPlaceActiveObject_Release,
    InPlaceActiveObject_GetWindow,
    InPlaceActiveObject_ContextSensitiveHelp,
    InPlaceActiveObject_TranslateAccelerator,
    InPlaceActiveObject_OnFrameWindowActivate,
    InPlaceActiveObject_OnDocWindowActivate,
    InPlaceActiveObject_ResizeBorder,
    InPlaceActiveObject_EnableModeless
};

#define OLECMD_THIS(iface) DEFINE_THIS(WebBrowser, OleCommandTarget, iface)

static HRESULT WINAPI WBOleCommandTarget_QueryInterface(IOleCommandTarget *iface,
        REFIID riid, void **ppv)
{
    WebBrowser *This = OLECMD_THIS(iface);
    return IWebBrowser2_QueryInterface(WEBBROWSER(This), riid, ppv);
}

static ULONG WINAPI WBOleCommandTarget_AddRef(IOleCommandTarget *iface)
{
    WebBrowser *This = OLECMD_THIS(iface);
    return IWebBrowser2_AddRef(WEBBROWSER(This));
}

static ULONG WINAPI WBOleCommandTarget_Release(IOleCommandTarget *iface)
{
    WebBrowser *This = OLECMD_THIS(iface);
    return IWebBrowser2_Release(WEBBROWSER(This));
}

static HRESULT WINAPI WBOleCommandTarget_QueryStatus(IOleCommandTarget *iface,
        const GUID *pguidCmdGroup, ULONG cCmds, OLECMD prgCmds[], OLECMDTEXT *pCmdText)
{
    WebBrowser *This = OLECMD_THIS(iface);
    FIXME("(%p)->(%s %lu %p %p)\n", This, debugstr_guid(pguidCmdGroup), cCmds, prgCmds,
          pCmdText);
    return E_NOTIMPL;
}

static HRESULT WINAPI WBOleCommandTarget_Exec(IOleCommandTarget *iface,
        const GUID *pguidCmdGroup, DWORD nCmdID, DWORD nCmdexecopt, VARIANT *pvaIn,
        VARIANT *pvaOut)
{
    WebBrowser *This = OLECMD_THIS(iface);
    FIXME("(%p)->(%s %ld %ld %p %p)\n", This, debugstr_guid(pguidCmdGroup), nCmdID,
          nCmdexecopt, pvaIn, pvaOut);
    return E_NOTIMPL;
}

#undef OLECMD_THIS

static const IOleCommandTargetVtbl OleCommandTargetVtbl = {
    WBOleCommandTarget_QueryInterface,
    WBOleCommandTarget_AddRef,
    WBOleCommandTarget_Release,
    WBOleCommandTarget_QueryStatus,
    WBOleCommandTarget_Exec
};

void WebBrowser_OleObject_Init(WebBrowser *This)
{
    This->lpOleObjectVtbl              = &OleObjectVtbl;
    This->lpOleInPlaceObjectVtbl       = &OleInPlaceObjectVtbl;
    This->lpOleControlVtbl             = &OleControlVtbl;
    This->lpOleInPlaceActiveObjectVtbl = &OleInPlaceActiveObjectVtbl;
    This->lpOleCommandTargetVtbl     = &OleCommandTargetVtbl;

    This->client = NULL;
    This->inplace = NULL;
    This->container = NULL;
    This->iphwnd = NULL;
    This->frame_hwnd = NULL;
    This->frame = NULL;
    This->uiwindow = NULL;
    This->shell_embedding_hwnd = NULL;

    memset(&This->pos_rect, 0, sizeof(RECT));
    memset(&This->clip_rect, 0, sizeof(RECT));
    memset(&This->frameinfo, 0, sizeof(OLEINPLACEFRAMEINFO));
}

void WebBrowser_OleObject_Destroy(WebBrowser *This)
{
    if(This->client)
        IOleObject_SetClientSite(OLEOBJ(This), NULL);
    if(This->container)
        IOleContainer_Release(This->container);
    if(This->frame)
        IOleInPlaceFrame_Release(This->frame);
    if(This->uiwindow)
        IOleInPlaceUIWindow_Release(This->uiwindow);
}
