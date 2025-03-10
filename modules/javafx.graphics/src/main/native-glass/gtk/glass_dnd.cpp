/*
 * Copyright (c) 2011, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
#include "glass_dnd.h"
#include "glass_general.h"
#include "glass_evloop.h"

#include "com_sun_glass_events_DndEvent.h"
#include "com_sun_glass_ui_gtk_GtkDnDClipboard.h"

#include <jni.h>
#include <cstring>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

/************************* COMMON *********************************************/
static jint translate_gdk_action_to_glass(GdkDragAction action)
{
    jint result = 0;
    result |= (action & GDK_ACTION_COPY)? com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_COPY : 0;
    result |= (action & GDK_ACTION_MOVE)? com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_MOVE : 0;
    result |= (action & GDK_ACTION_LINK)? com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_REFERENCE : 0;
    return result;
}

static GdkDragAction translate_glass_action_to_gdk(jint action)
{
    int result = 0;
    result |= (action & com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_COPY)? GDK_ACTION_COPY : 0;
    result |= (action & com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_MOVE)? GDK_ACTION_MOVE : 0;
    result |= (action & com_sun_glass_ui_gtk_GtkDnDClipboard_ACTION_REFERENCE)? GDK_ACTION_LINK : 0;
    return static_cast<GdkDragAction>(result);
}

static gboolean target_atoms_initialized = FALSE;
static GdkAtom TARGET_UTF8_STRING_ATOM;
static GdkAtom TARGET_MIME_TEXT_PLAIN_ATOM;
static GdkAtom TARGET_COMPOUND_TEXT_ATOM;
static GdkAtom TARGET_STRING_ATOM;

static GdkAtom TARGET_MIME_URI_LIST_ATOM;

static GdkAtom TARGET_MIME_PNG_ATOM;
static GdkAtom TARGET_MIME_JPEG_ATOM;
static GdkAtom TARGET_MIME_TIFF_ATOM;
static GdkAtom TARGET_MIME_BMP_ATOM;

static void init_target_atoms()
{
    if (target_atoms_initialized) {
        return;
    }
    TARGET_UTF8_STRING_ATOM = gdk_atom_intern_static_string("UTF8_STRING");
    TARGET_MIME_TEXT_PLAIN_ATOM = gdk_atom_intern_static_string("text/plain");
    TARGET_COMPOUND_TEXT_ATOM = gdk_atom_intern_static_string("COMPOUND_TEXT");
    TARGET_STRING_ATOM = gdk_atom_intern_static_string("STRING");

    TARGET_MIME_URI_LIST_ATOM = gdk_atom_intern_static_string("text/uri-list");

    TARGET_MIME_PNG_ATOM = gdk_atom_intern_static_string("image/png");
    TARGET_MIME_JPEG_ATOM = gdk_atom_intern_static_string("image/jpeg");
    TARGET_MIME_TIFF_ATOM = gdk_atom_intern_static_string("image/tiff");
    TARGET_MIME_BMP_ATOM = gdk_atom_intern_static_string("image/bmp");

    target_atoms_initialized = TRUE;
}

static gboolean target_is_text(GdkAtom target)
{
    init_target_atoms();

    return (target == TARGET_UTF8_STRING_ATOM ||
            target == TARGET_STRING_ATOM ||
            target == TARGET_MIME_TEXT_PLAIN_ATOM/* ||
            target == TARGET_COMPOUND_TEXT_ATOM*/);
}

static gboolean target_is_uri(GdkAtom target)
{
    init_target_atoms();
    return target == TARGET_MIME_URI_LIST_ATOM;
}

static gboolean target_is_image(GdkAtom target)
{
    init_target_atoms();
    return (target == TARGET_MIME_PNG_ATOM ||
            target == TARGET_MIME_JPEG_ATOM ||
            target == TARGET_MIME_TIFF_ATOM ||
            target == TARGET_MIME_BMP_ATOM);
}

static void clear_global_ref(gpointer data)
{
    mainEnv->DeleteGlobalRef((jobject)data);
}

static void dnd_set_performed_action(jint performed_action);
static jint dnd_get_performed_action();

/************************* TARGET *********************************************/
struct selection_data_ctx {
    gboolean received;
    guchar *data;
    GdkAtom type;
    gint format;
    gint length;
};

static gboolean dnd_target_receive_data(JNIEnv *env, GdkAtom target, selection_data_ctx *selection_ctx);

static struct {
    GdkDragContext *ctx;
    gboolean just_entered;
    jobjectArray mimes;
    gint dx, dy;
} enter_ctx = {NULL, FALSE, NULL, 0, 0};

gboolean is_dnd_owner = FALSE;

static void reset_enter_ctx() {
    if (enter_ctx.mimes != NULL) {
        mainEnv->DeleteGlobalRef(enter_ctx.mimes);
    }

    memset(&enter_ctx, 0, sizeof(enter_ctx));
}

static void process_dnd_target_drag_enter(WindowContext *ctx, GdkEventDND *event)
{
    reset_enter_ctx();
    enter_ctx.ctx = event->context;
    enter_ctx.just_entered = TRUE;
    gdk_window_get_origin(ctx->get_gdk_window(), &enter_ctx.dx, &enter_ctx.dy);
    is_dnd_owner = is_in_drag();
}

static void process_dnd_target_drag_motion(WindowContext *ctx, GdkEventDND *event)
{
    if (!enter_ctx.ctx) {
        gdk_drag_status(event->context, static_cast<GdkDragAction>(0), GDK_CURRENT_TIME);
        return; // Do not process motion events if no enter event was received
    }
    jmethodID method = enter_ctx.just_entered ? jViewNotifyDragEnter : jViewNotifyDragOver;
    GdkDragAction suggested = gdk_drag_context_get_suggested_action(event->context);
    GdkDragAction result = translate_glass_action_to_gdk(mainEnv->CallIntMethod(ctx->get_jview(), method,
            (jint)event->x_root - enter_ctx.dx, (jint)event->y_root - enter_ctx.dy,
            (jint)event->x_root, (jint)event->y_root,
            translate_gdk_action_to_glass(suggested)));
    CHECK_JNI_EXCEPTION(mainEnv)

    if (enter_ctx.just_entered) {
        enter_ctx.just_entered = FALSE;
    }
    gdk_drag_status(event->context, result, GDK_CURRENT_TIME);
}

static void process_dnd_target_drag_leave(WindowContext *ctx, GdkEventDND *event)
{
    (void)event;

    mainEnv->CallVoidMethod(ctx->get_jview(), jViewNotifyDragLeave, NULL);
    CHECK_JNI_EXCEPTION(mainEnv)
}

static void process_dnd_target_drop_start(WindowContext *ctx, GdkEventDND *event)
{
    if (!enter_ctx.ctx || enter_ctx.just_entered) {
        gdk_drop_finish(event->context, FALSE, GDK_CURRENT_TIME);
        gdk_drop_reply(event->context, FALSE, GDK_CURRENT_TIME);
        return; // Do not process drop events if no enter event and subsequent motion event were received
    }
    GdkDragAction selected = gdk_drag_context_get_selected_action(event->context);

    mainEnv->CallIntMethod(ctx->get_jview(), jViewNotifyDragDrop,
            (jint)event->x_root - enter_ctx.dx, (jint)event->y_root - enter_ctx.dy,
            (jint)event->x_root, (jint)event->y_root,
            translate_gdk_action_to_glass(selected));
    LOG_EXCEPTION(mainEnv)

    gdk_drop_finish(event->context, TRUE, GDK_CURRENT_TIME);
    gdk_drop_reply(event->context, TRUE, GDK_CURRENT_TIME);
}

static gboolean check_state_in_drag(JNIEnv *env)
{
    if (!enter_ctx.ctx) {
        jclass jc = env->FindClass("java/lang/IllegalStateException");
        if (!env->ExceptionCheck()) {
            env->ThrowNew(jc,
                    "Cannot get supported actions. Drag pointer haven't entered the application window");
        }
        return TRUE;
    }
    return FALSE;
}

// Events coming from application that are related to us being a DnD target
void process_dnd_target(WindowContext *ctx, GdkEventDND *event)
{
    switch (event->type) {
        case GDK_DRAG_ENTER:
            process_dnd_target_drag_enter(ctx, event);
            break;
        case GDK_DRAG_MOTION:
            process_dnd_target_drag_motion(ctx, event);
            break;
        case GDK_DRAG_LEAVE:
            process_dnd_target_drag_leave(ctx, event);
            break;
        case GDK_DROP_START:
            process_dnd_target_drop_start(ctx, event);
            break;
        default:
            break;
    }
}

jobjectArray dnd_target_get_mimes(JNIEnv *env)
{
    if (check_state_in_drag(env)) {
        return NULL;
    }
    if (!enter_ctx.mimes) {
        GList* targets = gdk_drag_context_list_targets(enter_ctx.ctx);
        jobject set = env->NewObject(jHashSetCls, jHashSetInit, NULL);
        EXCEPTION_OCCURED(env);

        while (targets) {
            GdkAtom target = GDK_POINTER_TO_ATOM(targets->data);
            gchar *name = gdk_atom_name(target);

            if (target_is_text(target)) {
                jstring jStr = env->NewStringUTF("text/plain");
                EXCEPTION_OCCURED(env);
                env->CallBooleanMethod(set, jSetAdd, jStr, NULL);
                EXCEPTION_OCCURED(env);
            }

            if (target_is_image(target)) {
                jstring jStr = env->NewStringUTF("application/x-java-rawimage");
                EXCEPTION_OCCURED(env);
                env->CallBooleanMethod(set, jSetAdd, jStr, NULL);
                EXCEPTION_OCCURED(env);
            }

            if (target_is_uri(target)) {
                selection_data_ctx ctx;
                if (dnd_target_receive_data(env, TARGET_MIME_URI_LIST_ATOM, &ctx)) {
                    gchar** uris = g_uri_list_extract_uris((gchar *) ctx.data);
                    guint size = g_strv_length(uris);
                    guint files_cnt = get_files_count(uris);
                    if (files_cnt) {
                        jstring jStr = env->NewStringUTF("application/x-java-file-list");
                        EXCEPTION_OCCURED(env);
                        env->CallBooleanMethod(set, jSetAdd, jStr, NULL);
                        EXCEPTION_OCCURED(env);
                    }
                    if (size - files_cnt) {
                        jstring jStr = env->NewStringUTF("text/uri-list");
                        EXCEPTION_OCCURED(env);
                        env->CallBooleanMethod(set, jSetAdd, jStr, NULL);
                        EXCEPTION_OCCURED(env);
                    }
                    g_strfreev(uris);
                }
                g_free(ctx.data);
            } else {
                jstring jStr = env->NewStringUTF(name);
                EXCEPTION_OCCURED(env);
                env->CallBooleanMethod(set, jSetAdd, jStr, NULL);
                EXCEPTION_OCCURED(env);
            }

            g_free(name);
            targets = targets->next;
        }
        enter_ctx.mimes = env->NewObjectArray(env->CallIntMethod(set, jSetSize, NULL),
                jStringCls, NULL);
        EXCEPTION_OCCURED(env);
        enter_ctx.mimes = (jobjectArray)env->CallObjectMethod(set, jSetToArray, enter_ctx.mimes, NULL);
        enter_ctx.mimes = (jobjectArray)env->NewGlobalRef(enter_ctx.mimes);
    }
    return enter_ctx.mimes;
}

jint dnd_target_get_supported_actions(JNIEnv *env)
{
    if (check_state_in_drag(env)) {
        return 0;
    }
    return translate_gdk_action_to_glass(gdk_drag_context_get_actions(enter_ctx.ctx));
}

static void wait_for_selection_data_hook(GdkEvent * event, void * data)
{
    selection_data_ctx *ctx = (selection_data_ctx*)data;
    GdkWindow *dest = glass_gdk_drag_context_get_dest_window(enter_ctx.ctx);
    if (event->type == GDK_SELECTION_NOTIFY &&
            event->selection.window == dest) {
        if (event->selection.property) { // if 0, that we received negative response
            ctx->length = gdk_selection_property_get(dest, &(ctx->data), &(ctx->type), &(ctx->format));
        }
        ctx->received = TRUE;
    }
}

static gboolean dnd_target_receive_data(JNIEnv *env, GdkAtom target, selection_data_ctx *selection_ctx)
{
    GevlHookRegistration hookReg;

    memset(selection_ctx, 0, sizeof(selection_data_ctx));

    gdk_selection_convert(glass_gdk_drag_context_get_dest_window(enter_ctx.ctx), gdk_drag_get_selection(enter_ctx.ctx), target,
                          GDK_CURRENT_TIME);

    hookReg =
            glass_evloop_hook_add(
                    (GevlHookFunction) wait_for_selection_data_hook,
                    selection_ctx);
    if (HANDLE_MEM_ALLOC_ERROR(env, hookReg,
                               "Failed to allocate event hook")) {
        return TRUE;
    }

    do {
        gtk_main_iteration();
    } while (!(selection_ctx->received));


    glass_evloop_hook_remove(hookReg);
    return selection_ctx->data != NULL;
}

static jobject dnd_target_get_string(JNIEnv *env)
{
    jobject result = NULL;
    selection_data_ctx ctx;

    if (dnd_target_receive_data(env, TARGET_UTF8_STRING_ATOM, &ctx)) {
        result = env->NewStringUTF((char *)ctx.data);
        EXCEPTION_OCCURED(env);
        g_free(ctx.data);
    }
    if (!result && dnd_target_receive_data(env, TARGET_MIME_TEXT_PLAIN_ATOM, &ctx)) {
        result = env->NewStringUTF((char *)ctx.data);
        EXCEPTION_OCCURED(env);
        g_free(ctx.data);
    }
    // TODO find out how to convert from compound text
    // if (!result && dnd_target_receive_data(env, TARGET_COMPOUND_TEXT_ATOM, &ctx)) {
    // }
    if (!result && dnd_target_receive_data(env, TARGET_STRING_ATOM, &ctx)) {
        gchar *str;
        str = g_convert( (gchar *)ctx.data, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        if (str != NULL) {
            result = env->NewStringUTF(str);
            EXCEPTION_OCCURED(env);
            g_free(str);
        }
        g_free(ctx.data);
    }
    return result;
}

static jobject dnd_target_get_list(JNIEnv *env, gboolean files)
{
    jobject result = NULL;
    selection_data_ctx ctx;

    if (dnd_target_receive_data(env, TARGET_MIME_URI_LIST_ATOM, &ctx)) {
        result = uris_to_java(env, g_uri_list_extract_uris((gchar *)ctx.data), files);
        g_free(ctx.data);
    }

    return result;
}

static jobject dnd_target_get_image(JNIEnv *env)
{
    GdkPixbuf *buf;
    GInputStream *stream;
    jobject result = NULL;
    GdkAtom targets[] = {
        TARGET_MIME_PNG_ATOM,
        TARGET_MIME_JPEG_ATOM,
        TARGET_MIME_TIFF_ATOM,
        TARGET_MIME_BMP_ATOM,
        0};
    GdkAtom *cur_target = targets;
    selection_data_ctx ctx;

    while(*cur_target != 0 && result == NULL) {
        if (dnd_target_receive_data(env, *cur_target, &ctx)) {
            const gint fmtDiv8 = ctx.format / 8;
            if (ctx.length <= 0 || fmtDiv8 <= 0 || ctx.length >= INT_MAX / fmtDiv8) {
                continue;
            }
            stream = g_memory_input_stream_new_from_data(ctx.data, ctx.length * fmtDiv8,
                    (GDestroyNotify)g_free);
            buf = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
            if (buf) {
                int w;
                int h;
                int stride;
                guchar *data;
                jbyteArray data_array;
                jobject buffer;

                if (!gdk_pixbuf_get_has_alpha(buf)) {
                    GdkPixbuf *tmp_buf = gdk_pixbuf_add_alpha(buf, FALSE, 0, 0, 0);
                    g_object_unref(buf);
                    buf = tmp_buf;
                }

                w = gdk_pixbuf_get_width(buf);
                h = gdk_pixbuf_get_height(buf);
                stride = gdk_pixbuf_get_rowstride(buf);

                if (h <= 0 || stride <= 0 || h >= INT_MAX / stride) {
                    g_object_unref(buf);
                    g_object_unref(stream);
                    continue;
                }

                data = gdk_pixbuf_get_pixels(buf);

                //Actually, we are converting RGBA to BGRA, but that's the same operation
                data = (guchar*) convert_BGRA_to_RGBA((int*) data, stride, h);
                data_array = env->NewByteArray(stride * h);
                EXCEPTION_OCCURED(env);
                env->SetByteArrayRegion(data_array, 0, stride*h, (jbyte*) data);
                EXCEPTION_OCCURED(env);

                buffer = env->CallStaticObjectMethod(jByteBufferCls, jByteBufferWrap, data_array);
                EXCEPTION_OCCURED(env);
                result = env->NewObject(jGtkPixelsCls, jGtkPixelsInit, w, h, buffer);
                EXCEPTION_OCCURED(env);

                g_object_unref(buf);
                g_free(data); // data from convert_BGRA_to_RGBA
            }
            g_object_unref(stream);
        }
        ++cur_target;
    }
    return result;
}

static jobject dnd_target_get_raw(JNIEnv *env, GdkAtom target, gboolean string_data)
{
    selection_data_ctx ctx;
    jobject result = NULL;
    if (dnd_target_receive_data(env, target, &ctx)) {
        if (string_data) {
             result = env->NewStringUTF((char *)ctx.data);
             EXCEPTION_OCCURED(env);
        } else {
            const gint fmtDiv8 = ctx.format / 8;
            if (ctx.length <= 0 || fmtDiv8 <= 0 || ctx.length >= INT_MAX / fmtDiv8) {
                g_free(ctx.data);
                return result;
            }
            jsize length = ctx.length * fmtDiv8;
            jbyteArray array = env->NewByteArray(length);
            EXCEPTION_OCCURED(env);
            env->SetByteArrayRegion(array, 0, length, (const jbyte*)ctx.data);
            EXCEPTION_OCCURED(env);
            result = env->CallStaticObjectMethod(jByteBufferCls, jByteBufferWrap, array);
            EXCEPTION_OCCURED(env);
        }
    }
    g_free(ctx.data);
    return result;
}

jobject dnd_target_get_data(JNIEnv *env, jstring mime)
{
    if (check_state_in_drag(env)) {
        return NULL;
    }
    const char *cmime = env->GetStringUTFChars(mime, NULL);
    jobject ret = NULL;

    init_target_atoms();

    if (g_strcmp0(cmime, "text/plain") == 0) {
        ret = dnd_target_get_string(env);
    } else if (g_strcmp0(cmime, "text/uri-list") == 0) {
        ret = dnd_target_get_list(env, FALSE);
    } else if (g_str_has_prefix(cmime, "text/")) {
        ret = dnd_target_get_raw(env, gdk_atom_intern(cmime, FALSE), TRUE);
    } else if (g_strcmp0(cmime, "application/x-java-file-list") == 0) {
        ret = dnd_target_get_list(env, TRUE);
    } else if (g_strcmp0(cmime, "application/x-java-rawimage") == 0 ) {
        ret = dnd_target_get_image(env);
    } else {
        ret = dnd_target_get_raw(env, gdk_atom_intern(cmime, FALSE), FALSE);
    }
    LOG_EXCEPTION(env)
    env->ReleaseStringUTFChars(mime, cmime);

    return ret;
}

/************************* SOURCE *********************************************/
static GdkWindow *dnd_window = NULL;
static jint dnd_performed_action;

const char * const SOURCE_DND_CONTEXT = "fx-dnd-context";
const char * const SOURCE_DND_DATA = "fx-dnd-data";
const char * const SOURCE_DND_ACTIONS = "fx-dnd-actions";

static GdkWindow* get_dnd_window()
{
    if (dnd_window == NULL) {
        GdkWindowAttr attr;
        memset(&attr, 0, sizeof (GdkWindowAttr));
        attr.override_redirect = TRUE;
        attr.window_type = GDK_WINDOW_TEMP;
        attr.type_hint = GDK_WINDOW_TYPE_HINT_UTILITY;
        attr.wclass = GDK_INPUT_OUTPUT;
        attr.event_mask = GDK_ALL_EVENTS_MASK;
        dnd_window = gdk_window_new(NULL, &attr, GDK_WA_NOREDIR | GDK_WA_TYPE_HINT);

        gdk_window_move(dnd_window, -99, -99);
        gdk_window_resize(dnd_window, 1, 1);
        gdk_window_show(dnd_window);
    }
    return dnd_window;
}

static void dnd_set_performed_action(jint performed_action) {
    dnd_performed_action = performed_action;
}

static jint dnd_get_performed_action() {
    return dnd_performed_action;
}

static void dnd_pointer_grab(GdkEvent *event, GdkCursor *cursor)
{
    glass_gdk_master_pointer_grab(event, dnd_window, cursor);
}

static GdkDragContext *get_drag_context() {
    GdkDragContext *ctx;
    ctx = (GdkDragContext*)g_object_get_data(G_OBJECT(dnd_window), SOURCE_DND_CONTEXT);
    return ctx;
}

static gboolean dnd_finish_callback(gpointer) {
    if (dnd_window) {
        dnd_set_performed_action(
                translate_gdk_action_to_glass(
                    gdk_drag_context_get_selected_action(
                        get_drag_context())));

        gdk_window_destroy(dnd_window);
        dnd_window = NULL;
        DragView::reset_drag_view();
    }

    return FALSE;
}

#ifdef GLASS_GTK3
static void dnd_finished_callback(GdkDragContext *ctx, gpointer user_data) {
    dnd_finish_callback(user_data);
}

static gboolean dnd_update_drag_view(gpointer) {
    gint x, y;

    gdk_device_get_position(gdk_drag_context_get_device(get_drag_context()),
                                NULL, &x, &y);


    DragView::move(x, y);

    return FALSE;
}
#endif

gboolean is_in_drag()
{
    return dnd_window != NULL;
}

static void determine_actions(guint state, GdkDragAction *action, GdkDragAction *possible_actions)
{
    GdkDragAction suggested = static_cast<GdkDragAction>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dnd_window), SOURCE_DND_ACTIONS)));

    if (state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) {
        if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) && (suggested & GDK_ACTION_LINK)) {
            *action = *possible_actions = GDK_ACTION_LINK;
            return;
        } else if ((state & GDK_SHIFT_MASK) && (suggested & GDK_ACTION_MOVE)) {
            *action = *possible_actions = GDK_ACTION_MOVE;
            return;
        } else if (suggested & GDK_ACTION_COPY){
            *action = *possible_actions = GDK_ACTION_COPY;
            return;
        }
    }

    *possible_actions = suggested;

    if (suggested & GDK_ACTION_COPY) {
        *action = GDK_ACTION_COPY;
    } else if (suggested & GDK_ACTION_MOVE) {
        *action = GDK_ACTION_MOVE;
    } else if (suggested & GDK_ACTION_LINK) {
        *action = GDK_ACTION_LINK;
    } else {
        *action = static_cast<GdkDragAction>(0);
    }
}

static jobject dnd_source_get_data(const char *key)
{
    jobject data = (jobject)g_object_get_data(G_OBJECT(dnd_window), SOURCE_DND_DATA);
    jstring string = mainEnv->NewStringUTF(key);
    EXCEPTION_OCCURED(mainEnv);
    jobject result = mainEnv->CallObjectMethod(data, jMapGet, string, NULL);

    return (EXCEPTION_OCCURED(mainEnv)) ? NULL : result;
}

static gboolean dnd_source_set_utf8_string(GdkWindow *requestor, GdkAtom property)
{
    jstring string = (jstring)dnd_source_get_data("text/plain");
    if (!string) {
        return FALSE;
    }

    const char *cstring = mainEnv->GetStringUTFChars(string, NULL);
    if (!cstring) {
        return FALSE;
    }
    gint size = strlen(cstring);

    gdk_property_change(requestor, property, GDK_SELECTION_TYPE_STRING,
            8, GDK_PROP_MODE_REPLACE, (guchar *)cstring, size);

    mainEnv->ReleaseStringUTFChars(string, cstring);
    return TRUE;
}

static gboolean dnd_source_set_string(GdkWindow *requestor, GdkAtom property)
{
    jstring string = (jstring)dnd_source_get_data("text/plain");
    if (!string) {
        return FALSE;
    }

    gboolean is_data_set = FALSE;
    const char *cstring = mainEnv->GetStringUTFChars(string, NULL);
    if (cstring) {
        gchar *res_str = g_convert((gchar *)cstring, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);

        if (res_str) {
            gdk_property_change(requestor, property, GDK_SELECTION_TYPE_STRING,
                    8, GDK_PROP_MODE_REPLACE, (guchar *)res_str, strlen(res_str));
            g_free(res_str);
            is_data_set = TRUE;
        }

        mainEnv->ReleaseStringUTFChars(string, cstring);
    }
    return is_data_set;
}

static gboolean dnd_source_set_image(GdkWindow *requestor, GdkAtom property, GdkAtom target)
{
    jobject pixels = dnd_source_get_data("application/x-java-rawimage");
    if (!pixels) {
        return FALSE;
    }

    gchar *buffer;
    gsize size;
    const char * type;
    GdkPixbuf *pixbuf = NULL;
    gboolean result = FALSE;

    if (target == TARGET_MIME_PNG_ATOM) {
        type = "png";
    } else if (target == TARGET_MIME_JPEG_ATOM) {
        type = "jpeg";
    } else if (target == TARGET_MIME_TIFF_ATOM) {
        type = "tiff";
    } else if (target == TARGET_MIME_BMP_ATOM) {
        type = "bmp";
    } else {
        return FALSE;
    }

    mainEnv->CallVoidMethod(pixels, jPixelsAttachData, PTR_TO_JLONG(&pixbuf));

    if (!EXCEPTION_OCCURED(mainEnv)
            && gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, type, NULL, NULL)) {
        gdk_property_change(requestor, property, target,
                8, GDK_PROP_MODE_REPLACE, (guchar *)buffer, size);
        result = TRUE;
    }
    g_object_unref(pixbuf);
    return result;
}

static gboolean dnd_source_set_uri_list(GdkWindow *requestor, GdkAtom property)
{
    const gchar* url = NULL;
    jstring jurl = NULL;

    jobjectArray files_array = NULL;
    gsize files_cnt = 0;

    if (jurl = (jstring) dnd_source_get_data("text/uri-list")) {
        url = mainEnv->GetStringUTFChars(jurl, NULL);
    }

    if (files_array = (jobjectArray) dnd_source_get_data("application/x-java-file-list")) {
        files_cnt = mainEnv->GetArrayLength(files_array);
    }
    if (!url && !files_cnt) {
        return FALSE;
    }

    GString* res = g_string_new (NULL); //http://www.ietf.org/rfc/rfc2483.txt

    if (files_cnt > 0) {
        for (gsize i = 0; i < files_cnt; ++i) {
            jstring string = (jstring) mainEnv->GetObjectArrayElement(files_array, i);
            EXCEPTION_OCCURED(mainEnv);
            const gchar* file = mainEnv->GetStringUTFChars(string, NULL);
            gchar* uri = g_filename_to_uri(file, NULL, NULL);

            g_string_append(res, uri);
            g_string_append(res, URI_LIST_LINE_BREAK);

            g_free(uri);
            mainEnv->ReleaseStringUTFChars(string, file);
        }
    }
    if (url) {
        g_string_append(res, url);
        g_string_append(res, URI_LIST_LINE_BREAK);
        mainEnv->ReleaseStringUTFChars(jurl, url);
    }

    gdk_property_change(requestor, property, GDK_SELECTION_TYPE_STRING,
            8, GDK_PROP_MODE_REPLACE, (guchar *) res->str, res->len);

    g_string_free(res, TRUE);
    return TRUE;
}

static gboolean dnd_source_set_raw(GdkWindow *requestor, GdkAtom property, GdkAtom target)
{
    gchar *target_name = gdk_atom_name(target);
    jobject data = dnd_source_get_data(target_name);
    gboolean is_data_set = FALSE;
    if (data) {
        if (mainEnv->IsInstanceOf(data, jStringCls)) {
            const char *cstring = mainEnv->GetStringUTFChars((jstring)data, NULL);
            if (cstring) {
                gdk_property_change(requestor, property, GDK_SELECTION_TYPE_STRING,
                        8, GDK_PROP_MODE_REPLACE, (guchar *) cstring, strlen(cstring));

                mainEnv->ReleaseStringUTFChars((jstring)data, cstring);
                is_data_set = TRUE;
            }
        } else if (mainEnv->IsInstanceOf(data, jByteBufferCls)) {
            jbyteArray byteArray = (jbyteArray)mainEnv->CallObjectMethod(data, jByteBufferArray);
            if (!EXCEPTION_OCCURED(mainEnv)) {
                jbyte* raw = mainEnv->GetByteArrayElements(byteArray, NULL);
                if (raw) {
                    jsize nraw = mainEnv->GetArrayLength(byteArray);

                    gdk_property_change(requestor, property, target,
                            8, GDK_PROP_MODE_REPLACE, (guchar *) raw, nraw);

                    mainEnv->ReleaseByteArrayElements(byteArray, raw, JNI_ABORT);
                    is_data_set = TRUE;
                }
            }
        }
    }

    g_free(target_name);
    return is_data_set;
}

static void process_dnd_source_selection_req(GdkWindow *window, GdkEvent *gdkEvent)
{
    (void)window;

    GdkEventSelection *event = &gdkEvent->selection;

#ifdef GLASS_GTK3
    GdkWindow *requestor = (event->requestor);
#else
    GdkWindow *requestor =
        gdk_x11_window_foreign_new_for_display(gdk_display_get_default(), event->requestor);
#endif

    gboolean is_data_set = FALSE;
    if (event->target == TARGET_UTF8_STRING_ATOM
            || event->target == TARGET_MIME_TEXT_PLAIN_ATOM) {
        is_data_set = dnd_source_set_utf8_string(requestor, event->property);
    } else if (event->target == TARGET_STRING_ATOM) {
        is_data_set = dnd_source_set_string(requestor, event->property);
//    } else if (event->target == TARGET_COMPOUND_TEXT_ATOM) { // XXX compound text
    } else if (target_is_image(event->target)) {
        is_data_set = dnd_source_set_image(requestor, event->property, event->target);
    } else if (event->target == TARGET_MIME_URI_LIST_ATOM) {
        is_data_set = dnd_source_set_uri_list(requestor, event->property);
    } else {
        is_data_set = dnd_source_set_raw(requestor, event->property, event->target);
    }

    gdk_selection_send_notify(event->requestor, event->selection, event->target,
                               (is_data_set) ? event->property : GDK_NONE, event->time);

}

static void process_dnd_source_mouse_release(GdkWindow *window, GdkEvent *event) {
    (void)window;

    glass_gdk_master_pointer_ungrab(event);

    if (gdk_drag_context_get_selected_action(get_drag_context())) {
        gdk_drag_drop(get_drag_context(), GDK_CURRENT_TIME);
    } else {
        gdk_drag_abort(get_drag_context(), GDK_CURRENT_TIME);
        /* let the gdk_drag_abort messages handled before finish */
        gdk_threads_add_idle((GSourceFunc) dnd_finish_callback, NULL);
    }
}

static void process_drag_motion(gint x_root, gint y_root, guint state)
{
    GdkWindow *dest_window;
    GdkDragProtocol prot;

#ifndef GLASS_GTK3
    // gtk3 uses a move-when-idle method
    DragView::move(x_root, y_root);
#endif

    gdk_drag_find_window_for_screen(get_drag_context(), NULL, gdk_screen_get_default(),
            x_root, y_root, &dest_window, &prot);

    if (prot != GDK_DRAG_PROTO_NONE) {
        GdkDragAction action, possible_actions;
        determine_actions(state, &action, &possible_actions);
        gdk_drag_motion(get_drag_context(), dest_window, prot, x_root, y_root,
                action, possible_actions, GDK_CURRENT_TIME);
    }
}

static void process_dnd_source_mouse_motion(GdkWindow *window, GdkEvent *event)
{
    (void)window;

    GdkEventMotion *eventMotion = &event->motion;
    process_drag_motion(eventMotion->x_root, eventMotion->y_root, eventMotion->state);
}

static void process_dnd_source_key_press_release(GdkWindow *window, GdkEvent *event)
{
    (void)window;

    GdkEventKey *eventKey = &event->key;

    if (eventKey->is_modifier) {
        guint state = eventKey->state;
        guint new_mod = 0;
        gint x,y;
        if (eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Control_L) ||
                eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Control_R)) {
            new_mod = GDK_CONTROL_MASK;
        } else if (eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Alt_L) ||
                eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Alt_R)) {
            new_mod = GDK_MOD1_MASK;
        } else if (eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Shift_L) ||
                eventKey->keyval == GLASS_GDK_KEY_CONSTANT(Shift_R)) {
            new_mod = GDK_SHIFT_MASK;
        }

        if (eventKey->type == GDK_KEY_PRESS) {
            state |= new_mod;
        } else {
            state ^= new_mod;
        }

#ifdef GLASS_GTK3
        gdk_device_get_position(gdk_drag_context_get_device(get_drag_context()), NULL, &x, &y);
#else
        glass_gdk_master_pointer_get_position(&x, &y);
#endif
        process_drag_motion(x, y, state);
    }
}

static void process_dnd_source_drag_status(GdkWindow *window, GdkEvent *event)
{
    (void)window;

    GdkEventDND *eventDnd = &event->dnd;
    GdkDragAction selected = gdk_drag_context_get_selected_action(eventDnd->context);
    GdkCursor* cursor;

#ifdef GLASS_GTK3
    // dragview shows content when dragging - this updates the position
    gdk_threads_add_idle_full (GDK_PRIORITY_REDRAW + 5, dnd_update_drag_view,
                               NULL, NULL);

#endif

    if (selected & GDK_ACTION_COPY) {
        cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "dnd-copy");
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "copy");
        }
    } else if (selected & (GDK_ACTION_MOVE | GDK_ACTION_PRIVATE)) {
        cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "dnd-move");
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "move");
        }
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "closedhand");
        }
    } else if (selected & GDK_ACTION_LINK) {
        cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "dnd-link");
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "link");
        }
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "alias");
        }
    } else {
        cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "dnd-no-drop");
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "no-drop");
        }
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "not-allowed");
        }
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "forbidden");
        }
        if (cursor == NULL) {
            cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "dnd-none");
        }
    }

    if (cursor == NULL) {
        cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    }

    dnd_pointer_grab(event, cursor);
}

static void process_dnd_source_drop_finished(GdkWindow *window, GdkEvent *event)
{
    (void)window;
    (void)event;

    gdk_threads_add_idle((GSourceFunc) dnd_finish_callback, NULL);
}

void process_dnd_source(GdkWindow *window, GdkEvent *event) {
    switch(event->type) {
        case GDK_MOTION_NOTIFY:
            process_dnd_source_mouse_motion(window, event);
            break;
        case GDK_BUTTON_RELEASE:
            process_dnd_source_mouse_release(window, event);
            break;
        case GDK_DRAG_STATUS:
            process_dnd_source_drag_status(window, event);
            break;
        case GDK_DROP_FINISHED:
            process_dnd_source_drop_finished(window, event);
            break;
        case GDK_KEY_PRESS:
        case GDK_KEY_RELEASE:
            process_dnd_source_key_press_release(window, event);
            break;
        case GDK_DRAG_ENTER:
            gdk_selection_owner_set(dnd_window, gdk_drag_get_selection(get_drag_context()),
                                    GDK_CURRENT_TIME, FALSE);
            break;
        case GDK_SELECTION_REQUEST:
            process_dnd_source_selection_req(window, event);
            break;
        default:
            break;
    }
}

static void add_target_from_jstring(JNIEnv *env, GList **list, jstring string)
{
    const char *gstring = env->GetStringUTFChars(string, NULL);
    if (g_strcmp0(gstring, "text/plain") == 0) {
        *list = g_list_append(*list, TARGET_UTF8_STRING_ATOM);
        *list = g_list_append(*list, TARGET_MIME_TEXT_PLAIN_ATOM);
        *list = g_list_append(*list, TARGET_STRING_ATOM);
        //*list = g_list_append(list, TARGET_COMPOUND_TEXT_ATOM);
    } else if (g_strcmp0(gstring, "application/x-java-rawimage") == 0) {
        *list = g_list_append(*list, TARGET_MIME_PNG_ATOM);
        *list = g_list_append(*list, TARGET_MIME_JPEG_ATOM);
        *list = g_list_append(*list, TARGET_MIME_TIFF_ATOM);
        *list = g_list_append(*list, TARGET_MIME_BMP_ATOM);
    } else if (g_strcmp0(gstring, "application/x-java-file-list") == 0) {
        *list = g_list_append(*list, TARGET_MIME_URI_LIST_ATOM);
    } else {
        *list = g_list_append(*list, gdk_atom_intern(gstring, FALSE));
    }
    env->ReleaseStringUTFChars(string, gstring);
}

static GList* data_to_targets(JNIEnv *env, jobject data)
{
    jobject keys;
    jobject keysIterator;
    jstring next;

    GList *list = NULL;

    init_target_atoms();

    keys = env->CallObjectMethod(data, jMapKeySet, NULL);
    JNI_EXCEPTION_TO_CPP(env)
    keysIterator = env->CallObjectMethod(keys, jIterableIterator, NULL);
    JNI_EXCEPTION_TO_CPP(env)
    while (env->CallBooleanMethod(keysIterator, jIteratorHasNext) == JNI_TRUE) {
        next = (jstring)env->CallObjectMethod(keysIterator, jIteratorNext, NULL);
        JNI_EXCEPTION_TO_CPP(env)
        add_target_from_jstring(env, &list, next);
    }
    return list;
}

static void dnd_source_push_data(JNIEnv *env, jobject data, jint supported)
{
    GdkWindow *src_window = get_dnd_window();
    GList *targets;
    GdkDragContext *ctx;

    if (supported == 0) {
        return; // No supported actions, do nothing
    }

    targets = data_to_targets(env, data);

    data = env->NewGlobalRef(data);

    GdkDragAction actions = translate_glass_action_to_gdk(supported);
    g_object_set_data_full(G_OBJECT(src_window), SOURCE_DND_DATA, data, clear_global_ref);
    g_object_set_data(G_OBJECT(src_window), SOURCE_DND_ACTIONS, (gpointer)actions);

    DragView::set_drag_view();

#ifdef GLASS_GTK3
    GdkDevice *device = gdk_device_manager_get_client_pointer(
                    gdk_display_get_device_manager(
                        gdk_display_get_default()));

    ctx = gdk_drag_begin_for_device(src_window, device, targets);
#else
    ctx = gdk_drag_begin(src_window, targets);
#endif

    g_list_free(targets);
    g_object_set_data(G_OBJECT(src_window), SOURCE_DND_CONTEXT, ctx);

#ifdef GLASS_GTK3
    if (gtk_get_minor_version() >= 20) {
        // according to GDK docs, this is only fired on managed mode, but
        // on 3.20+ the GDK_DROP_FINISHED event stopped working and this signal
        // works (even when not managed).
        g_signal_connect(ctx, "dnd-finished",
            G_CALLBACK(dnd_finished_callback), NULL);
    }

    if(gdk_device_grab(device, src_window, GDK_OWNERSHIP_NONE, FALSE,
                    (GdkEventMask)
                         (GDK_POINTER_MOTION_MASK
                             | GDK_BUTTON_MOTION_MASK
                             | GDK_BUTTON1_MOTION_MASK
                             | GDK_BUTTON2_MOTION_MASK
                             | GDK_BUTTON3_MOTION_MASK
                             | GDK_BUTTON_RELEASE_MASK),
                       NULL, GDK_CURRENT_TIME) != GDK_GRAB_SUCCESS) {
        g_warning("Usable to grab pointer device.");
    }
#else
    dnd_pointer_grab(NULL, NULL);
#endif

    is_dnd_owner = TRUE;
}

jint execute_dnd(JNIEnv *env, jobject data, jint supported) {
    try {
        dnd_source_push_data(env, data, supported);
    } catch (jni_exception&) {
        return 0;
    }

    while (is_in_drag()) {
        gtk_main_iteration();
    }

    return dnd_get_performed_action();
}

 /******************** DRAG VIEW ***************************/
DragView::View* DragView::view = NULL;

void DragView::reset_drag_view() {
    delete view;
    view = NULL;
}

gboolean DragView::get_drag_image_offset(int* x, int* y) {
    gboolean offset_set = FALSE;
    jobject bb = dnd_source_get_data("application/x-java-drag-image-offset");
    if (bb) {
        jbyteArray byteArray = (jbyteArray)mainEnv->CallObjectMethod(bb, jByteBufferArray);
        if (!EXCEPTION_OCCURED(mainEnv)) {
            jbyte* raw = mainEnv->GetByteArrayElements(byteArray, NULL);
            jsize nraw = mainEnv->GetArrayLength(byteArray);

            if ((size_t) nraw >= sizeof(jint) * 2) {
                jint* r = (jint*) raw;
                *x = BSWAP_32(r[0]);
                *y = BSWAP_32(r[1]);
                offset_set = TRUE;
            }

            mainEnv->ReleaseByteArrayElements(byteArray, raw, JNI_ABORT);
        }
    }
    return offset_set;
}

static void pixbufDestroyNotifyFunc(guchar *pixels, gpointer) {
    if (pixels != NULL) {
        g_free(pixels);
    }
}

GdkPixbuf* DragView::get_drag_image(gboolean* is_raw_image, gint* width, gint* height) {
    GdkPixbuf *pixbuf = NULL;
    gboolean is_raw = FALSE;

    jobject drag_image = dnd_source_get_data("application/x-java-drag-image");

    if (drag_image) {
        jbyteArray byteArray = (jbyteArray) mainEnv->CallObjectMethod(drag_image, jByteBufferArray);
        if (!EXCEPTION_OCCURED(mainEnv)) {

            jbyte* raw = mainEnv->GetByteArrayElements(byteArray, NULL);
            jsize nraw = mainEnv->GetArrayLength(byteArray);

            int w = 0, h = 0;
            int whsz = sizeof(jint) * 2; // Pixels are stored right after two ints
                                         // in this byteArray: width and height
            if (nraw > whsz) {
                jint* int_raw = (jint*) raw;
                w = BSWAP_32(int_raw[0]);
                h = BSWAP_32(int_raw[1]);

                // We should have enough pixels for requested width and height
                if (w > 0 && h > 0 &&  w < (INT_MAX / 4) / h &&
                        (nraw - whsz) / 4 - w * h >= 0) {
                    guchar* data = (guchar*) g_try_malloc0(nraw - whsz);
                    if (data) {
                        memcpy(data, (raw + whsz), nraw - whsz);
                        pixbuf = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8,
                                w, h, w * 4, pixbufDestroyNotifyFunc, NULL);
                    }
                }
            }
            mainEnv->ReleaseByteArrayElements(byteArray, raw, JNI_ABORT);
        }
    }

    if (!GDK_IS_PIXBUF(pixbuf)) {
        jobject pixels = dnd_source_get_data("application/x-java-rawimage");
        if (pixels) {
            is_raw = TRUE;
            mainEnv->CallVoidMethod(pixels, jPixelsAttachData, PTR_TO_JLONG(&pixbuf));
            CHECK_JNI_EXCEPTION_RET(mainEnv, NULL)
        }
    }

    if (!GDK_IS_PIXBUF(pixbuf)) {
        return NULL;
    }

    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);

    if (w > DRAG_IMAGE_MAX_WIDTH || h > DRAG_IMAGE_MAX_HEIGH) {
        double rw = DRAG_IMAGE_MAX_WIDTH / (double)w;
        double rh =  DRAG_IMAGE_MAX_HEIGH / (double)h;
        double r = MIN(rw, rh);


        int new_w = w * r;
        int new_h = h * r;

        w = new_w;
        h = new_h;

        GdkPixbuf *tmp_pixbuf = gdk_pixbuf_scale_simple(pixbuf, new_w, new_h, GDK_INTERP_TILES);
        g_object_unref(pixbuf);
        if (!GDK_IS_PIXBUF(tmp_pixbuf)) {
            return NULL;
        }
        pixbuf = tmp_pixbuf;
    }

    *is_raw_image = is_raw;
    *width = w;
    *height = h;

    return pixbuf;
}

void DragView::set_drag_view() {
    reset_drag_view();

    gboolean is_raw_image = FALSE;
    gint w = 0, h = 0;
    GdkPixbuf* pixbuf = get_drag_image(&is_raw_image, &w, &h);

    if (GDK_IS_PIXBUF(pixbuf)) {
        gint offset_x = w / 2;
        gint offset_y = h / 2;

        gboolean is_offset_set = get_drag_image_offset(&offset_x, &offset_y);

        DragView::view = new DragView::View(pixbuf, is_raw_image, is_offset_set, offset_x, offset_y);
    }
}
void DragView::move(gint x, gint y) {
    if (view) {
        view->move(x, y);
    }
}

static void on_screen_changed(GtkWidget *widget, GdkScreen *previous_screen, gpointer view) {
    (void)widget;
    (void)previous_screen;

    ((DragView::View*) view)->screen_changed();
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer view) {
    (void)widget;

    ((DragView::View*) view)->expose(cr);
    return FALSE;
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer view) {
    (void)widget;
    (void)event;

    ((DragView::View*) view)->expose(NULL);
    return FALSE;
}

DragView::View::View(GdkPixbuf* _pixbuf, gboolean _is_raw_image,
                                gboolean _is_offset_set, gint _offset_x, gint _offset_y) :
        pixbuf(_pixbuf),
        is_raw_image(_is_raw_image),
        is_offset_set(_is_offset_set),
        offset_x(_offset_x),
        offset_y(_offset_y)
{
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);

    widget = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(widget), GDK_WINDOW_TYPE_HINT_DND);
    gtk_widget_set_events(widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

    screen_changed();

    gtk_widget_realize(widget);

    gtk_widget_set_app_paintable(widget, TRUE);

#ifdef GLASS_GTK3
    g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(on_draw), this);
#else
    g_signal_connect(G_OBJECT(widget), "expose-event", G_CALLBACK(on_expose), this);
#endif
    g_signal_connect(G_OBJECT(widget), "screen-changed", G_CALLBACK(on_screen_changed), this);

    gtk_widget_set_size_request(widget, width, height);

    gtk_window_set_decorated(GTK_WINDOW(widget), FALSE);
    gtk_window_set_opacity(GTK_WINDOW(widget), .7);
}

void DragView::View::screen_changed() {
    GdkScreen *screen = gtk_widget_get_screen(widget);

    glass_configure_window_transparency(widget, true);

    if (!gdk_screen_is_composited(screen)) {
        if (!is_offset_set) {
            offset_x = 1;
            offset_y = 1;
        }
    }
}

void DragView::View::expose(cairo_t *context) {
#ifndef GLASS_GTK3
    context = gdk_cairo_create(gtk_widget_get_window(widget));
#endif
    cairo_surface_t* cairo_surface;

    guchar* pixels = is_raw_image
            ? (guchar*) convert_BGRA_to_RGBA((const int*) gdk_pixbuf_get_pixels(pixbuf),
                                                gdk_pixbuf_get_rowstride(pixbuf),
                                                height)
            : gdk_pixbuf_get_pixels(pixbuf);

    cairo_surface = cairo_image_surface_create_for_data(
            pixels,
            CAIRO_FORMAT_ARGB32,
            width, height, width * 4);

    cairo_set_source_surface(context, cairo_surface, 0, 0);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_paint(context);

    if (is_raw_image) {
        g_free(pixels);
    }

    cairo_surface_destroy(cairo_surface);

#ifndef GLASS_GTK3
    cairo_destroy(context);
#endif
}

void DragView::View::move(gint x, gint y) {
#ifdef GLASS_GTK3
    // gtk3 method uses a move-when-idle method
    gtk_window_move(GTK_WINDOW(widget), x - offset_x, y - offset_y);
#else
    if (!gtk_events_pending()) { // avoid sluggish window move
        gtk_window_move(GTK_WINDOW(widget), x - offset_x, y - offset_y);
    }
#endif

    if (gtk_widget_get_visible(widget)) {
        gdk_window_raise(gtk_widget_get_window(widget));
    }
    else {
        gtk_widget_show(widget);
    }
}

DragView::View::~View() {
    if (widget) {
        gtk_widget_destroy(widget);
        widget == NULL;
    }
    if (pixbuf) {
        g_object_unref(pixbuf);
        pixbuf == NULL;
    }
}
