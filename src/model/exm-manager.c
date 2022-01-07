#include "exm-manager.h"

#include "exm-extension.h"

#include <gio/gio.h>

struct _ExmManager
{
    GObject parent_instance;

    GDBusProxy* proxy;
    GListModel *model;
};

G_DEFINE_FINAL_TYPE (ExmManager, exm_manager, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_LIST_MODEL,
    N_PROPS
};

static GParamSpec *properties [N_PROPS];

ExmManager *
exm_manager_new (void)
{
    return g_object_new (EXM_TYPE_MANAGER, NULL);
}

static void
exm_manager_finalize (GObject *object)
{
    ExmManager *self = (ExmManager *)object;

    G_OBJECT_CLASS (exm_manager_parent_class)->finalize (object);
}

static void
exm_manager_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    ExmManager *self = EXM_MANAGER (object);

    switch (prop_id)
    {
    case PROP_LIST_MODEL:
        g_value_set_object (value, self->model);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
exm_manager_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    ExmManager *self = EXM_MANAGER (object);

    switch (prop_id)
      {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
}

static void
enable_extension_done (GDBusProxy   *proxy,
                       GAsyncResult *res,
                       ExmExtension *extension)
{
    GError *error = NULL;
    g_dbus_proxy_call_finish (proxy, res, &error);

    if (error)
    {
        gchar *uuid;
        g_object_get (extension, "uuid", &uuid, NULL);
        g_critical ("Could not enable extension: %s\n", uuid);
    }
}

void
exm_manager_enable_extension (ExmManager *self, ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    g_dbus_proxy_call (self->proxy,
                       "EnableExtension",
                       g_variant_new ("(s)", uuid, NULL),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                       (GAsyncReadyCallback) enable_extension_done,
                       extension);
}

static void
disable_extension_done (GDBusProxy   *proxy,
                        GAsyncResult *res,
                        ExmExtension *extension)
{
    GError *error = NULL;
    g_dbus_proxy_call_finish (proxy, res, &error);

    if (error)
    {
        gchar *uuid;
        g_object_get (extension, "uuid", &uuid, NULL);
        g_critical ("Could not disable extension: %s\n", uuid);
    }
}

void
exm_manager_disable_extension (ExmManager *self, ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    g_dbus_proxy_call (self->proxy,
                       "DisableExtension",
                       g_variant_new ("(s)", uuid, NULL),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                       (GAsyncReadyCallback) disable_extension_done,
                       extension);
}

static void
exm_manager_class_init (ExmManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = exm_manager_finalize;
    object_class->get_property = exm_manager_get_property;
    object_class->set_property = exm_manager_set_property;

    properties [PROP_LIST_MODEL]
        = g_param_spec_object ("list-model",
                               "List Model",
                               "List Model",
                               G_TYPE_LIST_MODEL,
                               G_PARAM_READABLE);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static GListModel *
parse_extension_list (GVariant *exlist)
{
    GListStore *store;

    /* format: a{sa{sv}}
     * array of interfaces, where each interface is an array of properties
     * each interface corresponds to one extension
     * this is terrible >:(
     * see also: https://stackoverflow.com/questions/54131543/how-can-i-get-the-g-dbus-connection-signal-subscribe-function-to-tell-me-about-p
     */
    GVariantIter *iter, *iter2;
    gchar *exname, *prop_name;
    GVariant *prop_value;

    store = g_list_store_new (EXM_TYPE_EXTENSION);

    g_variant_get (exlist, "(a{sa{sv}})", &iter);
    while (g_variant_iter_loop(iter, "{sa{sv}}", &exname, &iter2)) {
        g_print ("Extension Discovered: %s\n", exname);

        ExmExtension *extension;

        // Well-Defined Properties
        gchar *uuid = NULL;
        gchar *display_name = NULL;
        gchar *description = NULL;
        gboolean enabled = FALSE;

        while (g_variant_iter_loop(iter2, "{sv}", &prop_name, &prop_value))
        {
            g_print (" - Property: %s=%s\n", prop_name, g_variant_print(prop_value, 0));

            // Compare with DBus property names
            if (strcmp (prop_name, "uuid") == 0)
            {
                g_variant_get (prop_value, "s", &uuid);

                // Assert that this is the same as the extension name
                g_assert (strcmp(exname, uuid) == 0);
            }
            else if (strcmp (prop_name, "name") == 0)
            {
                g_variant_get (prop_value, "s", &display_name);
            }
            else if (strcmp (prop_name, "description") == 0)
            {
                g_variant_get (prop_value, "s", &description);
            }
            else if (strcmp (prop_name, "state") == 0)
            {
                double state;
                g_variant_get (prop_value, "d", &state);
                enabled = (state == 1);
            }
        }

        extension = exm_extension_new (uuid, display_name, description, enabled);
        g_list_store_append (G_LIST_STORE (store), extension);

        g_free (uuid);
        g_free (display_name);
        g_free (description);
    }
    g_variant_iter_free (iter);

    return G_LIST_MODEL (store);
}

static void
exm_manager_init (ExmManager *self)
{
    GError *error = NULL;

    self->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                                 "org.gnome.Shell.Extensions", "/org/gnome/Shell/Extensions",
                                                 "org.gnome.Shell.Extensions", NULL, &error);

    if (error != NULL)
    {
        g_critical ("Could not create proxy: %s\n", error->message);
        return;
    }

    GVariant* exlist = g_dbus_proxy_call_sync (self->proxy, "ListExtensions", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error != NULL)
    {
        g_critical ("Could not list extensions: %s\n", error->message);
        return;
    }

    self->model = parse_extension_list (exlist);
}