/*
 * Copyright (c) 2008-2009 Martin Mitas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* CREDITS:
 *  - Big thanks belong to Jeff Glatt for his article and sample code on
 *    http://www.codeproject.com/KB/COM/cwebpage.aspx
 */

/* BUGS:
 *  - [tab] and [shift]+[tab] do not change focus to another link or
 *    form element, as IE or any normal Windows dialog does.
 */

/* TODO:
 *  - In-memory HTML documents?
 *  - Notifications for context menus so app could customize the menu
 *    or provide its own. (The meny may need to depend o the object clicked.
 *    e.g. if it is image or something diffferent).
 *  - Notification for status text.
 */

#include "html.h"
#include "theme.h"

#include <exdisp.h>    /* IWebBrowser2 */
#include <exdispid.h>  /* DISP_xxxx constants */
#include <mshtmhst.h>  /* IDocHostUIHandler */


/* Uncomment this to have more verbous traces about MC_HTML control. */
/*#define HTML_DEBUG     1*/

#ifdef HTML_DEBUG
    #define HTML_TRACE        MC_TRACE
    #define HTML_TRACE_GUID   MC_TRACE_GUID
#else
    #define HTML_TRACE(...)        do {} while(0)
    #define HTML_TRACE_GUID(...)   do {} while(0)
#endif


/* Window class name */
static const TCHAR html_wc[] = MC_WC_HTML;


static BSTR url_blank = NULL;


/* Main control structure */
typedef struct html_tag html_t;
struct html_tag {
    HWND win;
    DWORD style                 : 31;
    DWORD unicode_notifications :  1;

    /* Pointer to the COM-object representing the embedded Internet Explorer */
    IOleObject* browser_obj;

    /* This structure is also COM-object with these interfaces, for wiring
     * MC_HTML control to the embedded Internet Explorer COM-object */
    IDispatch dispatch;
    IOleClientSite client_site;
    IOleInPlaceSiteEx inplace_site_ex;
    IOleInPlaceFrame inplace_frame;
    IDocHostUIHandler ui_handler;
};


/* Helper macros for retrieving the complete structure inside methods
 * of particular interfaces */
#define MC_HTML_FROM_DISPTACH(ptr_dispatch)                                   \
    MC_CONTAINEROF(ptr_dispatch, html_t, dispatch)
#define MC_HTML_FROM_CLIENT_SITE(ptr_client_site)                             \
    MC_CONTAINEROF(ptr_client_site, html_t, client_site)
#define MC_HTML_FROM_INPLACE_SITE_EX(ptr_inplace_site_ex)                     \
    MC_CONTAINEROF(ptr_inplace_site_ex, html_t, inplace_site_ex)
#define MC_HTML_FROM_INPLACE_FRAME(ptr_inplace_frame)                         \
    MC_CONTAINEROF(ptr_inplace_frame, html_t, inplace_frame)
#define MC_HTML_FROM_UI_HANDLER(ptr_ui_handler)                               \
    MC_CONTAINEROF(ptr_ui_handler, html_t, ui_handler)


static HRESULT
html_QueryInterface(html_t* html, REFIID riid, void** obj)
{
    if(IsEqualIID(riid, &IID_IDispatch)) {
        *obj = (void*)&html->dispatch;
        return S_OK;
    }

    if(IsEqualIID(riid, &IID_IOleClientSite) ||
       IsEqualIID(riid, &IID_IUnknown)) {
        *obj = (void*)&html->client_site;
        return S_OK;
    }

    if(IsEqualIID(riid, &IID_IOleWindow) ||
       IsEqualIID(riid, &IID_IOleInPlaceSite) ||
       IsEqualIID(riid, &IID_IOleInPlaceSiteEx)) {
        *obj = (void*)&html->inplace_site_ex;
        return S_OK;
    }

    if(IsEqualIID(riid, &IID_IOleInPlaceFrame)) {
        *obj = (void*)&html->inplace_frame;
        return S_OK;
    }

    if(IsEqualIID(riid, &IID_IDocHostUIHandler)) {
        *obj = (void*)&html->ui_handler;
        return S_OK;
    }

    HTML_TRACE_GUID("html_QueryInterface: unsupported GUID", riid);
    *obj = NULL;
    return E_NOINTERFACE;
}



/********************************
 *** Dummy reference counting ***
 ********************************/

/* Lifetime of html_t is determined by the lifetime of host window. When the
 * host window is being destroyed, the embedded browser is destroyed too.
 * Hence we don't need to reference count and delay html_t unallocation
 * when it drops back to zero. It simplifies the cleaning code quite a lot. */

static ULONG STDMETHODCALLTYPE
dummy_AddRef_or_Release(void* self)
{
    /* Always return value suggesting the caller that there is still some
     * remaning reference to the html_t object. */
    return 1;
}

#define DUMMY_ADDREF(type)                                                    \
    ((ULONG (STDMETHODCALLTYPE*)(type*)) dummy_AddRef_or_Release)

#define DUMMY_RELEASE(type)                                                   \
    ((ULONG (STDMETHODCALLTYPE*)(type*)) dummy_AddRef_or_Release)



/********************************
 *** IDispatch implementation ***
 ********************************/

static HRESULT STDMETHODCALLTYPE
dispatch_QueryInterface(IDispatch* self, REFIID riid, void** obj)
{
    return html_QueryInterface(MC_HTML_FROM_DISPTACH(self), riid, obj);
}

static HRESULT STDMETHODCALLTYPE
dispatch_GetTypeInfoCount(IDispatch* self, UINT* count)
{
    HTML_TRACE("dispatch_GetTypeInfoCount: [S_OK]");
    *count = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
dispatch_GetTypeInfo(IDispatch* self, UINT index, LCID lcid, ITypeInfo** type_info)
{
    HTML_TRACE("dispatch_GetTypeInfo: Stub [E_NOTIMPL]");
    *type_info = NULL;
    return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT STDMETHODCALLTYPE
dispatch_GetIDsOfNames(IDispatch* self, REFIID riid, LPOLESTR* names,
                       UINT names_count, LCID lcid, DISPID* disp_id)
{
    HTML_TRACE("dispatch_GetIDsOfNames('%S'): Stub [S_OK]", names_count > 0 ? names[0] : L"");
    *disp_id = DISPID_UNKNOWN;
    return E_NOTIMPL;
}

/* Forward declaration */
static void html_notify_url(html_t* html, UINT code, BSTR url);

static HRESULT STDMETHODCALLTYPE
dispatch_Invoke(IDispatch* self, DISPID disp_id, REFIID riid, LCID lcid,
                WORD flags, DISPPARAMS* disp_params, VARIANT* var_res,
                EXCEPINFO* except, UINT* arg_err)
{
    html_t* html = MC_HTML_FROM_DISPTACH(self);

    switch(disp_id) {
        case DISPID_BEFORENAVIGATE2:
        {
            BSTR url = disp_params->rgvarg[5].pvarVal->bstrVal;
            VARIANT_BOOL* done = disp_params->rgvarg[0].pboolVal;
            if(wcsncmp(url, L"app:", 4) == 0) {
                html_notify_url(html, MC_HN_APPLINK, url);
                *done = VARIANT_TRUE;
            } else {
                *done = VARIANT_FALSE;
            }
            break;
        }

        case DISPID_DOCUMENTCOMPLETE:
        {
            BSTR url = disp_params->rgvarg[0].pvarVal->bstrVal;
            html_notify_url(html, MC_HN_DOCUMENTCOMPLETE, url);
            break;
        }

        default:
            return DISP_E_MEMBERNOTFOUND;
    }

    return S_OK;
}

static IDispatchVtbl dispatch_vtable = {
    dispatch_QueryInterface,
    DUMMY_ADDREF(IDispatch),
    DUMMY_RELEASE(IDispatch),
    dispatch_GetTypeInfoCount,
    dispatch_GetTypeInfo,
    dispatch_GetIDsOfNames,
    dispatch_Invoke
};



/*************************************
 *** IOleClientSite implementation ***
 *************************************/

static HRESULT STDMETHODCALLTYPE
client_site_QueryInterface(IOleClientSite* self, REFIID riid, void** obj)
{
    return html_QueryInterface(MC_HTML_FROM_CLIENT_SITE(self), riid, obj);
}

static HRESULT STDMETHODCALLTYPE
client_site_SaveObject(IOleClientSite* self)
{
    HTML_TRACE("client_site_SaveObject: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
client_site_GetMoniker(IOleClientSite* self, DWORD assign, DWORD moniker_id, IMoniker** moniker)
{
    HTML_TRACE("client_site_GetMoniker: Stub [E_NOTIMPL]");
    *moniker = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
client_site_GetContainer(IOleClientSite* self, LPOLECONTAINER* container)
{
    HTML_TRACE("client_site_GetContainer: Stub [E_NOINTERFACE]");
    *container = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE
client_site_ShowObject(IOleClientSite* self)
{
    HTML_TRACE("client_site_ShowObject: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
client_site_OnShowWindow(IOleClientSite* self, BOOL show)
{
    HTML_TRACE("client_site_OnShowWindow(%d): Stub [E_NOTIMPL]", show);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
client_site_RequestNewObjectLayout(IOleClientSite* self)
{
    HTML_TRACE("client_site_RequestNewObjectLayout: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}


static IOleClientSiteVtbl client_site_vtable = {
    client_site_QueryInterface,
    DUMMY_ADDREF(IOleClientSite),
    DUMMY_RELEASE(IOleClientSite),
    client_site_SaveObject,
    client_site_GetMoniker,
    client_site_GetContainer,
    client_site_ShowObject,
    client_site_OnShowWindow,
    client_site_RequestNewObjectLayout
};



/******************************************
 *** IOleInPlaceSiteEx(Ex) implementation ***
 ******************************************/

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_QueryInterface(IOleInPlaceSiteEx* self, REFIID riid, void** obj)
{
    return html_QueryInterface(MC_HTML_FROM_INPLACE_SITE_EX(self), riid, obj);
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_GetWindow(IOleInPlaceSiteEx* self, HWND* win)
{
    HTML_TRACE("inplace_site_GetWindow");
    *win = MC_HTML_FROM_INPLACE_SITE_EX(self)->win;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_ContextSensitiveHelp(IOleInPlaceSiteEx* self, BOOL mode)
{
    HTML_TRACE("inplace_site_context_sensitive_help: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_CanInPlaceActivate(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_CanInPlaceActivate: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnInPlaceActivate(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_OnInPlaceActivate: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnUIActivate(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_OnUIActivate: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_GetWindowContext(IOleInPlaceSiteEx* self, LPOLEINPLACEFRAME* frame,
                LPOLEINPLACEUIWINDOW* doc, RECT* rect, RECT* clip_rect, LPOLEINPLACEFRAMEINFO frame_info)
{
	html_t* html;
    HTML_TRACE("inplace_site_GetWindowContext");

    html = MC_HTML_FROM_INPLACE_SITE_EX(self);
    *frame = &html->inplace_frame;
    *doc = NULL;
    frame_info->fMDIApp = FALSE;
    frame_info->hwndFrame = GetAncestor(html->win, GA_ROOT);

    frame_info->haccel = NULL;
    frame_info->cAccelEntries = 0;
    GetClientRect(html->win, rect);
    GetClientRect(html->win, clip_rect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_Scroll(IOleInPlaceSiteEx* self, SIZE extent)
{
    HTML_TRACE("inplace_site_Scroll: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnUIDeactivate(IOleInPlaceSiteEx* self, BOOL undoable)
{
    HTML_TRACE("inplace_site_OnUIDeactivate: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnInPlaceDeactivate(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_OnInPlaceDeactivate: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_DiscardUndoState(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_DiscardUndoState: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_DeactivateAndUndo(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_DeactivateAndUndo: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnPosRectChange(IOleInPlaceSiteEx* self, const RECT* rect)
{
    IOleInPlaceObject* inplace;
	html_t* html;
    HTML_TRACE("inplace_site_OnPosRectChange");

    html = MC_HTML_FROM_INPLACE_SITE_EX(self);
    html->browser_obj->lpVtbl->QueryInterface(html->browser_obj,
                        &IID_IOleInPlaceObject, (void**)(void*)&inplace);
    if(inplace != NULL) {
        inplace->lpVtbl->SetObjectRects(inplace, rect, rect);
        inplace->lpVtbl->Release(inplace);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnInPlaceActivateEx(IOleInPlaceSiteEx* self, WINBOOL* no_redraw, DWORD flags)
{
    HTML_TRACE("inplace_site_OnInPlaceActivateEx(): Stub [S_OK]");
    *no_redraw = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_OnInPlaceDeactivateEx(IOleInPlaceSiteEx* self, WINBOOL no_redraw)
{
    HTML_TRACE("inplace_site_OnInPlaceDeactivateEx(%d): Stub [S_OK]", no_redraw);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_site_ex_RequestUIActivate(IOleInPlaceSiteEx* self)
{
    HTML_TRACE("inplace_site_RequestUIActivate: Stub [S_OK]");
    return S_OK;
}

static IOleInPlaceSiteExVtbl inplace_site_ex_vtable = {
    inplace_site_ex_QueryInterface,
    DUMMY_ADDREF(IOleInPlaceSiteEx),
    DUMMY_RELEASE(IOleInPlaceSiteEx),
    inplace_site_ex_GetWindow,
    inplace_site_ex_ContextSensitiveHelp,
    inplace_site_ex_CanInPlaceActivate,
    inplace_site_ex_OnInPlaceActivate,
    inplace_site_ex_OnUIActivate,
    inplace_site_ex_GetWindowContext,
    inplace_site_ex_Scroll,
    inplace_site_ex_OnUIDeactivate,
    inplace_site_ex_OnInPlaceDeactivate,
    inplace_site_ex_DiscardUndoState,
    inplace_site_ex_DeactivateAndUndo,
    inplace_site_ex_OnPosRectChange,
    inplace_site_ex_OnInPlaceActivateEx,
    inplace_site_ex_OnInPlaceDeactivateEx,
    inplace_site_ex_RequestUIActivate
};


/***************************************
 *** IOleInPlaceFrame implementation ***
 ***************************************/

static HRESULT STDMETHODCALLTYPE
inplace_frame_QueryInterface(IOleInPlaceFrame* self, REFIID riid, void** obj)
{
    return html_QueryInterface(MC_HTML_FROM_INPLACE_FRAME(self), riid, obj);
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_GetWindow(IOleInPlaceFrame* self, HWND* win)
{
    HTML_TRACE("inplace_frame_GetWindow");
    *win = GetAncestor(MC_HTML_FROM_INPLACE_FRAME(self)->win, GA_ROOT);
    return(S_OK);
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_ContextSensitiveHelp(IOleInPlaceFrame* self, BOOL mode)
{
    HTML_TRACE("inplace_frame_ContextSensitiveHelp: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_GetBorder(IOleInPlaceFrame* self, RECT* rect)
{
    HTML_TRACE("inplace_frame_GetBorder: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_RequestBorderSpace(IOleInPlaceFrame* self, LPCBORDERWIDTHS widths)
{
    HTML_TRACE("inplace_frame_RequestBorderSpace: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_SetBorderSpace(IOleInPlaceFrame* self, LPCBORDERWIDTHS space)
{
    HTML_TRACE("inplace_frame_SetBorderSpace: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_SetActiveObject(IOleInPlaceFrame* self, IOleInPlaceActiveObject *active_obj, LPCOLESTR obj_name)
{
    HTML_TRACE("inplace_frame_SetActiveObject: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_InsertMenus(IOleInPlaceFrame* self, HMENU menu_shared, LPOLEMENUGROUPWIDTHS menu_widths)
{
    HTML_TRACE("inplace_frame_InsertMenus: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_SetMenu(IOleInPlaceFrame* self, HMENU menu_shared, HOLEMENU old_menu, HWND active_obj)
{
    HTML_TRACE("inplace_frame_SetMenu: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_RemoveMenus(IOleInPlaceFrame* self, HMENU menu_shared)
{
    HTML_TRACE("inplace_frame_RemoveMenus: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_SetStatusText(IOleInPlaceFrame* self, LPCOLESTR status_text)
{
    /*HTML_TRACE("inplace_frame_SetStatusText: Stub [S_OK]: '%ls'", status_text);*/
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_EnableModeless(IOleInPlaceFrame* self, BOOL enable)
{
    HTML_TRACE("inplace_frame_EnableModeless(%d): Stub [S_OK]", enable);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
inplace_frame_TranslateAccelerator(IOleInPlaceFrame* self, MSG* msg, WORD id)
{
    HTML_TRACE("inplace_frame_TranslateAccelerator: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}


static IOleInPlaceFrameVtbl inplace_frame_vtable = {
    inplace_frame_QueryInterface,
    DUMMY_ADDREF(IOleInPlaceFrame),
    DUMMY_RELEASE(IOleInPlaceFrame),
    inplace_frame_GetWindow,
    inplace_frame_ContextSensitiveHelp,
    inplace_frame_GetBorder,
    inplace_frame_RequestBorderSpace,
    inplace_frame_SetBorderSpace,
    inplace_frame_SetActiveObject,
    inplace_frame_InsertMenus,
    inplace_frame_SetMenu,
    inplace_frame_RemoveMenus,
    inplace_frame_SetStatusText,
    inplace_frame_EnableModeless,
    inplace_frame_TranslateAccelerator
};



/****************************************
 *** IDocHostUIHandler implementation ***
 ****************************************/

static HRESULT STDMETHODCALLTYPE
ui_handler_QueryInterface(IDocHostUIHandler* self, REFIID riid, void** obj)
{
    return html_QueryInterface(MC_HTML_FROM_UI_HANDLER(self), riid, obj);
}

static HRESULT STDMETHODCALLTYPE
ui_handler_ShowContextMenu(IDocHostUIHandler* self, DWORD menu_id, POINT* pos,
                IUnknown* reserved1, IDispatch* reserved2)
{
    html_t* html = MC_HTML_FROM_UI_HANDLER(self);
    if(html->style & MC_HS_NOCONTEXTMENU)
        return S_OK;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_GetHostInfo(IDocHostUIHandler* self, DOCHOSTUIINFO* info)
{
    if(info == NULL || info->cbSize < sizeof(DOCHOSTUIINFO)) {
        HTML_TRACE("ui_handler_GetHostInfo: info->cbSize == %lu "
                   "[E_UNEXPECTED]", info->cbSize);
        return E_UNEXPECTED;
    }

    /* Host window is responsible for outer border (the host window may use
     * WS_BORDER and/or WS_EX_CLIENTEDGE) */
    info->dwFlags |= DOCHOSTUIFLAG_NO3DOUTERBORDER;

    /* Check whether the controls on HTML page should use XP theming. */
    if(theme_IsThemeActive()) {
        info->dwFlags &= ~DOCHOSTUIFLAG_NOTHEME;
        info->dwFlags |= DOCHOSTUIFLAG_THEME;
    } else {
        info->dwFlags |= DOCHOSTUIFLAG_NOTHEME;
        info->dwFlags &= ~DOCHOSTUIFLAG_THEME;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_ShowUI(IDocHostUIHandler* self, DWORD id, IOleInPlaceActiveObject* active_obj,
                IOleCommandTarget* target, IOleInPlaceFrame* inplace_frame, IOleInPlaceUIWindow* doc)
{
    HTML_TRACE("ui_handler_ShowUI: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_HideUI(IDocHostUIHandler* self)
{
    HTML_TRACE("ui_handler_HideUI: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_UpdateUI(IDocHostUIHandler* self)
{
    HTML_TRACE("ui_handler_UpdateUI: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_EnableModeless(IDocHostUIHandler* self, BOOL enable)
{
    HTML_TRACE("ui_handler_EnableModeless(%d): Stub [S_OK]", enable);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_OnDocWindowActivate(IDocHostUIHandler* self, BOOL active)
{
    HTML_TRACE("ui_handler_OnDocWindowActivate(%d): Stub [S_OK]", active);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_OnFrameWindowActivate(IDocHostUIHandler* self, BOOL active)
{
    HTML_TRACE("ui_handler_OnFrameWindowActivate(%d): Stub [S_OK]", active);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_ResizeBorder(IDocHostUIHandler* self, const RECT* rect,
                IOleInPlaceUIWindow* inplace_win, BOOL is_frame_win)
{
    HTML_TRACE("ui_handler_ResizeBorder: Stub [S_OK]");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_TranslateAccelerator(IDocHostUIHandler* self, MSG* msg, const GUID* guid, DWORD cmd_id)
{
    HTML_TRACE("ui_handler_TranslateAccelerator: Stub [E_NOTIMPL]");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_GetOptionKeyPath(IDocHostUIHandler* self, LPOLESTR* key, DWORD reserved)
{
    HTML_TRACE("ui_handler_GetOptionKeyPath: Stub [E_NOTIMPL]");
    *key = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_GetDropTarget(IDocHostUIHandler* self, IDropTarget* drop_target, IDropTarget** p_drop_target)
{
    HTML_TRACE("ui_handler_GetDropTarget: Stub [E_NOTIMPL]");
    *p_drop_target = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_GetExternal(IDocHostUIHandler* self, IDispatch** p_dispatch)
{
    html_t* html = MC_HTML_FROM_UI_HANDLER(self);
    *p_dispatch = &html->dispatch;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_TranslateUrl(IDocHostUIHandler* self, DWORD reserved,
                        OLECHAR* url, OLECHAR** p_url)
{
    HTML_TRACE("ui_handler_TranslateUrl('%S'): Stub [S_FALSE]", url);
    *p_url = NULL;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE
ui_handler_FilterDataObject(IDocHostUIHandler* self, IDataObject* obj, IDataObject** p_obj)
{
    HTML_TRACE("ui_handler_FilterDataObject: Stub [S_FALSE]");
    *p_obj = NULL;
    return S_FALSE;
}


static IDocHostUIHandlerVtbl ui_handler_vtable = {
    ui_handler_QueryInterface,
    DUMMY_ADDREF(IDocHostUIHandler),
    DUMMY_RELEASE(IDocHostUIHandler),
    ui_handler_ShowContextMenu,
    ui_handler_GetHostInfo,
    ui_handler_ShowUI,
    ui_handler_HideUI,
    ui_handler_UpdateUI,
    ui_handler_EnableModeless,
    ui_handler_OnDocWindowActivate,
    ui_handler_OnFrameWindowActivate,
    ui_handler_ResizeBorder,
    ui_handler_TranslateAccelerator,
    ui_handler_GetOptionKeyPath,
    ui_handler_GetDropTarget,
    ui_handler_GetExternal,
    ui_handler_TranslateUrl,
    ui_handler_FilterDataObject
};



/**********************************
 *** Host window implementation ***
 **********************************/

static BSTR
html_bstr(const void* from_str, int from_type)
{
    WCHAR* str_w;
    BSTR str_b;

    if(from_str == NULL)
        return NULL;

    if(from_type == MC_STRW) {
        str_w = (WCHAR*) from_str;
        if(str_w[0] == L'\0')
            return NULL;
    } else {
        char* str_a;
        MC_ASSERT(from_type == MC_STRA);
        str_a = (char*) from_str;
        if(str_a[0] == '\0')
            return NULL;
        str_w = (WCHAR*) mc_str(str_a, from_type, MC_STRW);
        if(MC_ERR(str_w == NULL)) {
            MC_TRACE("html_bstr: mc_str() failed.");
            return NULL;
        }
    }

    str_b = SysAllocString(str_w);
    if(MC_ERR(str_b == NULL))
        MC_TRACE("html_bstr: SysAllocString() failed.");

    if(from_type == MC_STRA)
        free(str_w);

    return str_b;
}

static void
html_notify_url(html_t* html, UINT code, BSTR url)
{
    MC_NMHTMLURLW notify;

    HTML_TRACE("html_notify_url: code=%d url='%S'", code, url ? url : L"[null]");

    notify.hdr.hwndFrom = html->win;
    notify.hdr.idFrom = GetDlgCtrlID(html->win);
    notify.hdr.code = code;
    if(html->unicode_notifications)
        notify.pszUrl = url;
    else
        notify.pszUrl = (WCHAR*) mc_str(url, MC_STRW, MC_STRA);

    SendMessage(GetParent(html->win), WM_NOTIFY,
                (WPARAM)notify.hdr.idFrom, (LPARAM)&notify);

    if(!html->unicode_notifications && notify.pszUrl != NULL)
        free((char*)notify.pszUrl);
}

static void
html_notify_format(html_t* html)
{
    LRESULT lres;
    lres = SendMessage(GetParent(html->win), WM_NOTIFYFORMAT,
                       (WPARAM) html->win, NF_QUERY);
    html->unicode_notifications = (lres == NFR_UNICODE ? 1 : 0);
    MC_TRACE("html_notify_format: Will use %s notifications.",
             html->unicode_notifications ? "Unicode" : "ANSI");
}

static int
html_goto_url(html_t* html, const void* url, BOOL unicode)
{
    IWebBrowser2* browser_iface;
    BSTR bstr_url;
    VARIANT var_url;
    HRESULT hr;

    if((unicode && url != NULL && ((WCHAR*)url)[0] != L'\0') ||
       (!unicode && url != NULL && ((char*)url)[0] != '\0')) {
        bstr_url = html_bstr(url, (unicode ? MC_STRW : MC_STRA));
        if(MC_ERR(bstr_url == NULL)) {
            MC_TRACE("html_goto_url: html_bstr() failed.");
            goto err_bstr;
        }
    } else {
        bstr_url = url_blank;
    }

    hr = html->browser_obj->lpVtbl->QueryInterface(html->browser_obj,
                    &IID_IWebBrowser2, (void**)(void*)&browser_iface);
    if(MC_ERR(browser_iface == NULL)) {
        MC_TRACE("html_goto_url: IID_IWebBrowser2::QueryInterface() failed "
                 "[%lu]", (ULONG) hr);
        goto err_iface;
    }

    VariantInit(&var_url);
    var_url.vt = VT_BSTR;
    var_url.bstrVal = bstr_url;
    browser_iface->lpVtbl->Navigate2(browser_iface, &var_url, NULL, NULL, NULL, NULL);
    browser_iface->lpVtbl->Release(browser_iface);
    VariantClear(&var_url);

    if(bstr_url != url_blank)
        SysFreeString(bstr_url);
    return 0;

err_iface:
    if(bstr_url != url_blank)
        SysFreeString(bstr_url);
err_bstr:
    return -1;
}

static html_t*
html_create(HWND win, CREATESTRUCT* cs)
{
    html_t* html = NULL;
    IWebBrowser2* browser_iface = NULL;
    IConnectionPointContainer* conn_point_container;
    IConnectionPoint* conn_point;
    DWORD cookie;
    RECT rect;
    HRESULT hr;

    /* Initialize OLE. It would be more logical to perform this in html_init()
     * but accroding to MSDN, OleInitialize() must not be called from context
     * of DllMain(). Also, calling it here can avoid the OLE machinery
     * initialization for applications not using MC_HTML control. */
    hr = OleInitialize(NULL);
    if(MC_ERR(hr != S_OK && hr != S_FALSE)) {
        MC_TRACE("html_create: OleInitialize() failed [%lu]", (ULONG)hr);
        goto err_OleInitialize;
    }

    /* Allocate and setup the html_t structure */
    html = (html_t*) malloc(sizeof(html_t));
    if(MC_ERR(html == NULL)) {
        MC_TRACE("html_create: malloc() failed.");
        goto err_malloc;
    }
    html->win = win;
    html->style = GetWindowLong(win, GWL_STYLE);
    html->browser_obj = NULL;
    html->dispatch.lpVtbl = &dispatch_vtable;
    html->client_site.lpVtbl = &client_site_vtable;
    html->inplace_site_ex.lpVtbl = &inplace_site_ex_vtable;
    html->inplace_frame.lpVtbl = &inplace_frame_vtable;
    html->ui_handler.lpVtbl = &ui_handler_vtable;
    html_notify_format(html);

    /* Create browser object */
    hr = CoCreateInstance(&CLSID_WebBrowser, NULL, CLSCTX_INPROC,
            &IID_IOleObject, (void**)&html->browser_obj);
    if(MC_ERR(html->browser_obj == NULL)) {
        MC_TRACE("html_create: CoCreateInstance(CLSID_WebBrowser) failed "
                 "[%lu]", (ULONG)hr);
        goto err_CoCreateInstance;
    }

    /* Embed the browser object into our host window */
    hr = html->browser_obj->lpVtbl->SetClientSite(html->browser_obj,
                &html->client_site);
    if(MC_ERR(hr != S_OK)) {
        MC_TRACE("html_create: IOleObject::SetClientSite() failed [%lu]",
                 (ULONG)hr);
        goto err_SetClientSite;
    }
    GetClientRect(win, &rect);
    hr = html->browser_obj->lpVtbl->DoVerb(html->browser_obj, OLEIVERB_INPLACEACTIVATE,
                            NULL, &html->client_site, 0, win, &rect);
    if(MC_ERR(hr != S_OK)) {
        MC_TRACE("html_create: IOleObject::DoVerb(OLEIVERB_INPLACEACTIVATE) "
                 "failed [%lu]", (ULONG)hr);
        goto err_DoVerb;
    }

    /* Send DIID_DWebBrowserEvents2 to our IDispatch */
    hr = html->browser_obj->lpVtbl->QueryInterface(html->browser_obj,
                &IID_IConnectionPointContainer, (void**)&conn_point_container);
    if(MC_ERR(hr != S_OK)) {
        MC_TRACE("html_create: QueryInterface(IID_IConnectionPointContainer) failed "
                 "[%lu]", (ULONG)hr);
        goto err_QueryCPCInterface;
    }
    hr = conn_point_container->lpVtbl->FindConnectionPoint(conn_point_container,
                &DIID_DWebBrowserEvents2, &conn_point);
    conn_point_container->lpVtbl->Release(conn_point_container);
    if(MC_ERR(hr != S_OK)) {
        MC_TRACE("html_create: FindConnectionPoint(DIID_DWebBrowserEvents2) failed "
                 "[%lu]", (ULONG)hr);
        goto err_FindConnPoint;
    }
    conn_point->lpVtbl->Advise(conn_point, (IUnknown*)&html->client_site, &cookie);
    conn_point->lpVtbl->Release(conn_point);

    /* Set browser position and size according to the host window */
    hr = html->browser_obj->lpVtbl->QueryInterface(html->browser_obj,
                &IID_IWebBrowser2, (void**)&browser_iface);
    if(MC_ERR(browser_iface == NULL)) {
        MC_TRACE("html_create: QueryInterface(IID_IWebBrowser2) failed "
                 "[%lu]", (ULONG)hr);
        goto err_QueryBrowserInterface;
    }
    browser_iface->lpVtbl->put_Left(browser_iface, 0);
    browser_iface->lpVtbl->put_Top(browser_iface, 0);
#if 0  /* these are set in WM_SIZE handler */
    browser_iface->lpVtbl->put_Width(browser_iface, rect.right - rect.left);
    browser_iface->lpVtbl->put_Height(browser_iface, rect.bottom - rect.top);
#endif
    browser_iface->lpVtbl->Release(browser_iface);

    /* Goto specified URL if any */
    if(cs->lpszName != NULL && cs->lpszName[0] != _T('\0')) {
#ifdef UNICODE
        html_goto_url(html, cs->lpszName, TRUE);
#else
        html_goto_url(html, cs->lpszName, FALSE);
#endif
    }

    /* Success */
    return html;

    /* Error unwinding */
err_QueryBrowserInterface:
err_FindConnPoint:
err_QueryCPCInterface:
err_DoVerb:
err_SetClientSite:
    html->browser_obj->lpVtbl->Close(html->browser_obj, OLECLOSE_NOSAVE);
    html->browser_obj->lpVtbl->Release(html->browser_obj);
err_CoCreateInstance:
    free(html);
err_malloc:
    OleUninitialize();
err_OleInitialize:
    return NULL;
}

static void
html_destroy(html_t* html)
{
    html->browser_obj->lpVtbl->Close(html->browser_obj, OLECLOSE_NOSAVE);
    html->browser_obj->lpVtbl->Release(html->browser_obj);
    free(html);
    OleUninitialize();
}

static LRESULT CALLBACK
html_proc(HWND win, UINT msg, WPARAM wp, LPARAM lp)
{
    html_t* html = (html_t*) GetWindowLongPtr(win, 0);

    switch(msg) {
        case MC_HM_GOTOURLW:
        case MC_HM_GOTOURLA:
            return (html_goto_url(html, (const void*)lp, (msg == MC_HM_GOTOURLW)) == 0);

        case WM_SIZE:
        {
            IWebBrowser2* browser_iface;

            html->browser_obj->lpVtbl->QueryInterface(html->browser_obj,
                        &IID_IWebBrowser2, (void**)(void*)&browser_iface);
            if(browser_iface != NULL) {
                browser_iface->lpVtbl->put_Width(browser_iface, LOWORD(lp));
                browser_iface->lpVtbl->put_Height(browser_iface, HIWORD(lp));
                browser_iface->lpVtbl->Release(browser_iface);
            }
            return 0;
        }

        case WM_STYLECHANGED:
            if(wp == GWL_STYLE) {
                STYLESTRUCT* ss = (STYLESTRUCT*) lp;
                html->style = ss->styleNew;
            }
            return 0;

        case WM_NOTIFYFORMAT:
            if(lp == NF_REQUERY)
                html_notify_format(html);
            return (html->unicode_notifications ? NFR_UNICODE : NFR_ANSI);

        case WM_SETTEXT:
            return FALSE;

        case WM_GETTEXT:
            if(wp > 0)
                ((TCHAR*)lp)[0] = _T('\0');
            return 0;

        case WM_CREATE:
            html = html_create(win, (CREATESTRUCT*)lp);
            if(MC_ERR(html == NULL)) {
                MC_TRACE("html_proc(WM_CREATE): html_create() failed.");
                return -1;
            }
            SetWindowLongPtr(win, 0, (LONG_PTR)html);
            return 0;

        case WM_DESTROY:
            html_destroy(html);
            return 0;
    }

    return DefWindowProc(win, msg, wp, lp);
}


int
html_init(void)
{
    WNDCLASS wc = { 0 };

    mc_init_common_controls(ICC_STANDARD_CLASSES);

    url_blank = SysAllocString(L"about:blank");
    if(MC_ERR(url_blank == NULL)) {
        MC_TRACE("html_init: SysAllocString() failed [%lu]", GetLastError());
        return -1;
    }

    wc.lpfnWndProc = html_proc;
    wc.cbWndExtra = sizeof(html_t*);
    wc.hInstance = mc_instance_exe;
    wc.lpszClassName = html_wc;
    if(MC_ERR(!RegisterClass(&wc))) {
        MC_TRACE("html_init: RegisterClass() failed [%lu]", GetLastError());
        SysFreeString(url_blank);
        return -1;
    }

    return 0;
}

void
html_fini(void)
{
    UnregisterClass(html_wc, mc_instance_exe);
    SysFreeString(url_blank);
}

