#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

#include <gio/gio.h>
#include <dbus/dbus.h>
#include <glib.h>

#include <gdbus-generated.h>

// Defines

#define BLUEZ_SERVICE_NAME "org.bluez"
#define BLUEZ_OBJECT_PATH "/org/bluez"
#define BLUEZ_ADVERT_PATH "/org/bluez/hci0/advert1"
#define BLUEZ_DEVICE_PATH "/org/bluez/hci0"
#define SERVICE_UUID "0123"

// Structure definitions

LEAdvertisement1 * leadvertisement;
LEAdvertisingManager1 * leadvertisingmanager;

// Function prototypes

/**
 * Handle the advertisement release signal.
 *
 * @param object the advertisement object being released
 * @param invocation the message invocation details
 * @param user_data the user data passed to the signal connect
 */
static gboolean handle_release(LEAdvertisement1 * object, GDBusMethodInvocation * invocation, gpointer user_data) {
	printf("Advert released\n");

	leadvertisement1_complete_release(object, invocation);
	
	return TRUE;
}

/**
 * Deal with errors by printing them to stderr if there is one, then freeing 
 * and clearning the error structure.
 *
 * @param error the error structure to check and report if it exists
 * @param hint a human-readable hint that will be output alongside the error
 */
void report_error(GError ** error, char const * hint) {
	if (*error) {
		fprintf(stderr, "Error %s: %s\n", hint, (*error)->message);
		g_error_free(*error);
		*error = NULL;
	}
}

/**
 * Advertisement registration callback
 *
 * @param proxy the advertisement manager proxy object
 * @param res the result of the operation
 * @param user_data the user data passed to the async callback
 */
static void on_register_advert(LEAdvertisingManager1 *proxy, GAsyncResult *res, gpointer user_data) {
	gboolean result;
	GError *error;

	error = NULL;

	result = leadvertising_manager1_call_register_advertisement_finish(proxy, res, &error);
	report_error(&error, "registering advert callback");

	printf("Registered advert with result %d\n", result);
}

/**
 * Main; the entry point of the service.
 *
 * @param argc the number of arguments passed in
 * @param argv array of arguments passed in
 * @return value returned on service exit
 */
gint main(gint argc, gchar * argv[]) {
	GError *error;
	GMainLoop * loop;
	guint id;
	GDBusObjectManagerServer * object_manager;
	GDBusConnection * connection;
	GVariantDict dict_options;
	GVariant * arg_options;
	gboolean result;
	gchar const * const uuids[] = {SERVICE_UUID, NULL};
	ObjectSkeleton * object;

	error = NULL;
	loop = g_main_loop_new(NULL, FALSE);

	///////////////////////////////////////////////////////

	printf("Creating object manager server\n");

	object_manager = g_dbus_object_manager_server_new(BLUEZ_OBJECT_PATH);

	///////////////////////////////////////////////////////

	printf("Getting bus\n");

	connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	report_error(&error, "getting bus");

	///////////////////////////////////////////////////////

	printf("Creating advertisement\n");

	// Publish the advertisement interface
	error = NULL;
	leadvertisement = leadvertisement1_skeleton_new();
	g_signal_connect(leadvertisement, "handle-release", G_CALLBACK(&handle_release), NULL);

	// Set the advertisement properties
	leadvertisement1_set_service_uuids(leadvertisement, uuids);
	leadvertisement1_set_type_(leadvertisement, "peripheral");

	object = object_skeleton_new (BLUEZ_ADVERT_PATH);
	object_skeleton_set_leadvertisement1(object, leadvertisement);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(object_manager, G_DBUS_OBJECT_SKELETON(object));
	g_dbus_object_manager_server_set_connection(object_manager, connection);

	///////////////////////////////////////////////////////

	printf("Creating advertising manager\n");

	// Obtain a proxy for the interface
	leadvertisingmanager = leadvertising_manager1_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, &error);
	report_error(&error, "creating advertising manager");

	///////////////////////////////////////////////////////
	
	printf("Registering advertisement\n");

	// Call the RegisterAdvertisement methodo on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	leadvertising_manager1_call_register_advertisement(leadvertisingmanager, BLUEZ_ADVERT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_advert), NULL);

	///////////////////////////////////////////////////////

	printf("Entering main loop\n");
	g_main_loop_run(loop);

	printf("Exited main loop\n");	
	g_bus_unown_name(id);
	g_main_loop_unref(loop);

	printf("The End\n");

	return 0;
}


