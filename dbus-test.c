#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>

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
#define CHARACTERISTIC_UUID "5678"
#define CHARACTERISTIC_VALUE "012"
#define CHARACTERISTIC_LENGTH (16)

#define BLUEZ_GATT_OBJECT_PATH "/org/bluez/gatt"
#define BLUEZ_GATT_SERVICE_PATH "/org/bluez/gatt/service0"
#define BLUEZ_GATT_CHARACTERISTIC_PATH "/org/bluez/gatt/service0/char0"

// Structure definitions

LEAdvertisement1 * leadvertisement;
LEAdvertisingManager1 * leadvertisingmanager;
GattManager1 * gattmanager;
GattService1 * gattservice;
GattCharacteristic1 * gattcharacteristic;
char characteristic[CHARACTERISTIC_LENGTH];

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

static gboolean handle_read_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, GVariant *arg_options, gpointer user_data) {
	printf("Read value: %s\n", characteristic);

	gatt_characteristic1_complete_read_value(object, invocation, characteristic);
	
	return TRUE;
}

static gboolean handle_write_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, const gchar *arg_value, GVariant *arg_options, gpointer user_data) {
	printf("Write value: %s\n", arg_value);

	strncpy(characteristic, arg_value, CHARACTERISTIC_LENGTH);
	characteristic[CHARACTERISTIC_LENGTH - 1] = 0;

	gatt_characteristic1_complete_write_value(object, invocation);
	
	return TRUE;
}

static gboolean handle_start_notify(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, gpointer user_data) {
	printf("Start notify\n");

	gatt_characteristic1_complete_start_notify(object, invocation);
	
	return TRUE;
}

static gboolean handle_stop_notify(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, gpointer user_data) {
	printf("Stop notify\n");

	gatt_characteristic1_complete_stop_notify(object, invocation);
	
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

static void on_register_application(GattManager1 *proxy, GAsyncResult *res, gpointer user_data) {
	gboolean result;
	GError *error;

	error = NULL;

	result = gatt_manager1_call_register_application_finish(proxy, res, &error);
	report_error(&error, "registering application callback");

	printf("Registered application with result %d\n", result);
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
	GDBusConnection * connection;
	GVariantDict dict_options;
	GVariant * arg_options;
	gboolean result;
	gchar const * const uuids[] = {SERVICE_UUID, NULL};
	GDBusObjectManagerServer * object_manager_advert;
	ObjectSkeleton * object_advert;
	GDBusObjectManagerServer * object_manager_gatt;
	ObjectSkeleton * object_gatt_service;
	ObjectSkeleton * object_gatt_characteristic;
	const gchar * const charflags[] = {"read", "write"};

	strncpy(characteristic, CHARACTERISTIC_VALUE, CHARACTERISTIC_LENGTH);
	characteristic[CHARACTERISTIC_LENGTH - 1] = 0;
	error = NULL;
	loop = g_main_loop_new(NULL, FALSE);

	///////////////////////////////////////////////////////

	printf("Creating object manager server\n");

	object_manager_advert = g_dbus_object_manager_server_new(BLUEZ_OBJECT_PATH);

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

	object_advert = object_skeleton_new (BLUEZ_ADVERT_PATH);
	object_skeleton_set_leadvertisement1(object_advert, leadvertisement);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(object_manager_advert, G_DBUS_OBJECT_SKELETON(object_advert));
	g_dbus_object_manager_server_set_connection(object_manager_advert, connection);

	///////////////////////////////////////////////////////

	printf("Creating advertising manager\n");

	// Obtain a proxy for the LEAdvertisementMAanager1 interface
	leadvertisingmanager = leadvertising_manager1_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, &error);
	report_error(&error, "creating advertising manager");

	///////////////////////////////////////////////////////
	
	printf("Register advertisement\n");

	// Call the RegisterAdvertisement method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	leadvertising_manager1_call_register_advertisement(leadvertisingmanager, BLUEZ_ADVERT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_advert), NULL);

	///////////////////////////////////////////////////////
	
	printf("Creating Gatt manager\n");

	// Obtain a proxy for the Gattmanager1 interface
	gattmanager = gatt_manager1_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, &error);
	report_error(&error, "creating gatt manager");

	///////////////////////////////////////////////////////

	printf("Creating Gatt service\n");

	// Publish the gatt service interface
	error = NULL;
	gattservice = gatt_service1_skeleton_new();

	// Set the gatt service properties
	gatt_service1_set_uuid(gattservice, SERVICE_UUID);
	gatt_service1_set_primary(gattservice, TRUE);

	object_gatt_service = object_skeleton_new (BLUEZ_GATT_SERVICE_PATH);
	object_skeleton_set_gatt_service1(object_gatt_service, gattservice);

	///////////////////////////////////////////////////////

	printf("Creating Gatt characteristic\n");

	// Publish the gatt characteristic interface
	error = NULL;
	gattcharacteristic = gatt_characteristic1_skeleton_new();

	// Set the gatt characteristic properties
	gatt_characteristic1_set_uuid (gattcharacteristic, CHARACTERISTIC_UUID);
	gatt_characteristic1_set_service (gattcharacteristic, BLUEZ_GATT_SERVICE_PATH);
	gatt_characteristic1_set_flags (gattcharacteristic, charflags);

	object_gatt_characteristic = object_skeleton_new (BLUEZ_GATT_CHARACTERISTIC_PATH);
	object_skeleton_set_gatt_characteristic1(object_gatt_service, gattcharacteristic);

	g_signal_connect(gattcharacteristic, "handle-read-value", G_CALLBACK(&handle_read_value), NULL);
	g_signal_connect(gattcharacteristic, "handle-write-value", G_CALLBACK(&handle_write_value), NULL);
	g_signal_connect(gattcharacteristic, "handle-start-notify", G_CALLBACK(&handle_start_notify), NULL);
	g_signal_connect(gattcharacteristic, "handle-stop-notify", G_CALLBACK(&handle_stop_notify), NULL);

	///////////////////////////////////////////////////////

	printf("Creating object manager server\n");

	object_manager_gatt = g_dbus_object_manager_server_new(BLUEZ_GATT_OBJECT_PATH);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(object_manager_gatt, G_DBUS_OBJECT_SKELETON(object_gatt_service));
	g_dbus_object_manager_server_export(object_manager_gatt, G_DBUS_OBJECT_SKELETON(object_gatt_characteristic));
	g_dbus_object_manager_server_set_connection(object_manager_gatt, connection);

	///////////////////////////////////////////////////////
	
	printf("Register gatt service\n");

	// Call the RegisterApplication method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	gatt_manager1_call_register_application(gattmanager, BLUEZ_GATT_OBJECT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_application), NULL);

	///////////////////////////////////////////////////////

	printf("Entering main loop\n");
	g_main_loop_run(loop);

	printf("Exited main loop\n");	
	g_bus_unown_name(id);
	g_main_loop_unref(loop);

	printf("The End\n");

	return 0;
}


