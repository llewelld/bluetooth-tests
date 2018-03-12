#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>

#include <gio/gio.h>
#include <dbus/dbus.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <gdbus-generated.h>

#include "pico/pico.h"
#include "pico/debug.h"
#include "pico/log.h"
#include "pico/buffer.h"
#include "pico/base64.h"
#include "pico/keypair.h"
#include "pico/cryptosupport.h"

// Defines

#define BLUEZ_SERVICE_NAME "org.bluez"
#define BLUEZ_OBJECT_PATH "/org/bluez"
#define BLUEZ_ADVERT_PATH "/org/bluez/hci0/advert1"
#define BLUEZ_DEVICE_PATH "/org/bluez/hci0"
//#define SERVICE_UUID "68F9A6EE-0000-1000-8000-00805F9B34FB"
//#define CHARACTERISTIC_UUID "68F9A6EF-0000-1000-8000-00805F9B34FB"

//#define CHARACTERISTIC_UUID_INCOMING "56add98a-0e8a-4113-85bf-6dc97b58a9c1"
//#define CHARACTERISTIC_UUID_OUTGOING "56add98a-0e8a-4113-85bf-6dc97b58a9c2"

#define SERVICE_UUID "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa0"
#define CHARACTERISTIC_UUID_INCOMING "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa1"
#define CHARACTERISTIC_UUID_OUTGOING "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa2"


#define CHARACTERISTIC_VALUE "012"
#define CHARACTERISTIC_LENGTH (256)

#define BLUEZ_GATT_OBJECT_PATH "/org/bluez/gatt"
#define BLUEZ_GATT_SERVICE_PATH "/org/bluez/gatt/service0"
#define BLUEZ_GATT_CHARACTERISTIC_PATH_OUTGOING "/org/bluez/gatt/service0/char0"
#define BLUEZ_GATT_CHARACTERISTIC_PATH_INCOMING "/org/bluez/gatt/service0/char1"

// Structure definitions


// Function prototypes

typedef struct _ServiceBle {
	LEAdvertisement1 * leadvertisement;
	LEAdvertisingManager1 * leadvertisingmanager;
	GattManager1 * gattmanager;
	GattService1 * gattservice;
	GattCharacteristic1 * gattcharacteristic_outgoing;
	GattCharacteristic1 * gattcharacteristic_incoming;
	unsigned char characteristic_outgoing[CHARACTERISTIC_LENGTH];
	unsigned char characteristic_incoming[CHARACTERISTIC_LENGTH];
	int charlength;
	GMainLoop * loop;
	size_t remaining_write;
	Buffer * buffer_write;
	Buffer * buffer_read;
} ServiceBle;

ServiceBle * serviceble_new() {
	ServiceBle * serviceble;

	serviceble = CALLOC(sizeof(ServiceBle), 1);

	serviceble->leadvertisement = NULL;
	serviceble->leadvertisingmanager = NULL;
	serviceble->gattmanager = NULL;
	serviceble->gattservice = NULL;
	serviceble->gattcharacteristic_outgoing = NULL;
	serviceble->gattcharacteristic_incoming = NULL;
	serviceble->charlength = 0;
	serviceble->loop = NULL;
	serviceble->remaining_write = 0;
	serviceble->buffer_write = buffer_new(0);
	serviceble->buffer_read = buffer_new(0);

	return serviceble;
}

void service_delete(ServiceBle * serviceble) {
	if (serviceble != NULL) {
		if (serviceble->buffer_write) {
			buffer_delete(serviceble->buffer_write);
			serviceble->buffer_write = NULL;
		}

		if (serviceble->buffer_read) {
			buffer_delete(serviceble->buffer_read);
			serviceble->buffer_read = NULL;
		}

		FREE(serviceble);
		serviceble = NULL;
	}
}



void appendbytes(char unsigned const * bytes, int num, Buffer * out) {
	int pos;
	char letters[3];

	for (pos = 0; pos < num; pos++) {
		snprintf(letters, 3, "%02X", bytes[pos]);
		buffer_append(out, letters, 2);
	}
}

void create_uuid(char const * commitmentb64, bool continuous, char uuid[sizeof(SERVICE_UUID) + 1]) {
	unsigned char a[4];
	unsigned char b[2];
	unsigned char c[2];
	unsigned char d[8];
	Buffer * commitment;
	Buffer * generated;
	unsigned int pos;
	char const * commitmentbytes;

	commitment = buffer_new(0);
	generated = buffer_new(0);
	base64_decode_string(commitmentb64, commitment);

	if (buffer_get_pos(commitment) != 32) {
		printf("Incorrect commitment length\n");
	}

	commitmentbytes = buffer_get_buffer(commitment);
	for (pos = 0; pos < 4; pos++) {
		a[pos] = commitmentbytes[16 + pos];
	}

	for (pos = 0; pos < 2; pos++) {
		b[pos] = commitmentbytes[20 + pos];
	}

	for (pos = 0; pos < 2; pos++) {
		c[pos] = commitmentbytes[22 + pos];
	}

	for (pos = 0; pos < 8; pos++) {
		d[pos] = commitmentbytes[24 + pos];
	}

	if (continuous) {
		d[7] |= 0x01;
	}
	else {
		d[7] &= 0xFE;
	}

	appendbytes(a, 4, generated);
	buffer_append_string(generated, "-");
	appendbytes(b, 2, generated);
	buffer_append_string(generated, "-");
	appendbytes(c, 2, generated);
	buffer_append_string(generated, "-");
	appendbytes(d, 2, generated);
	buffer_append_string(generated, "-");
	appendbytes(d + 2, 6, generated);

	commitmentbytes = buffer_get_buffer(generated);
	for (pos = 0; pos < sizeof(SERVICE_UUID) + 1; pos++) {
		uuid[pos] = commitmentbytes[pos];
	}

	buffer_delete(commitment);
	buffer_delete(generated);
}

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
	ServiceBle * serviceble;

	serviceble = (ServiceBle *)user_data;

	GVariant * variant;

	printf("Read value: %s\n", serviceble->characteristic_incoming);

	variant = g_variant_new_from_data (G_VARIANT_TYPE("ay"), serviceble->characteristic_incoming, serviceble->charlength, TRUE, NULL, NULL);

	gatt_characteristic1_complete_read_value(object, invocation, variant);
	
	return TRUE;
}

static void send_data(ServiceBle * serviceble, char const * data, int length) {
	GVariant * variant1;
	GVariant * variant2;

	// Store the data to send
	buffer_append(serviceble->buffer_read, data, length);

	variant1 = g_variant_new_from_data (G_VARIANT_TYPE("ay"), buffer_get_buffer(serviceble->buffer_read), buffer_get_pos(serviceble->buffer_read), TRUE, NULL, NULL);

	gatt_characteristic1_set_value (serviceble->gattcharacteristic_outgoing, variant1);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(serviceble->gattcharacteristic_outgoing));

	variant1 = g_variant_new_from_data (G_VARIANT_TYPE("ay"), "", 0, TRUE, NULL, NULL);

	//gatt_characteristic1_set_value (gattcharacteristic_outgoing, variant2);
	//g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(gattcharacteristic_outgoing));
}

static gboolean handle_write_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, GVariant *arg_value, GVariant *arg_options, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	GVariantIter * iter;
	guchar data;

	g_variant_get(arg_value, "ay", &iter);

	serviceble->charlength = 0;
	while ((g_variant_iter_loop(iter, "y", &data) && (serviceble->charlength < (CHARACTERISTIC_LENGTH - 1)))) {
		serviceble->characteristic_outgoing[serviceble->charlength] = data;
		serviceble->charlength++;
	}
	g_variant_iter_free(iter);

	serviceble->characteristic_outgoing[serviceble->charlength] = 0;

	if ((serviceble->remaining_write == 0) && (serviceble->charlength > 5)) {
		buffer_clear(serviceble->buffer_write);

		// We can read off the length
		serviceble->remaining_write = 0;
		serviceble->remaining_write |= ((unsigned char)serviceble->characteristic_outgoing[1]) << 24;
		serviceble->remaining_write |= ((unsigned char)serviceble->characteristic_outgoing[2]) << 16;
		serviceble->remaining_write |= ((unsigned char)serviceble->characteristic_outgoing[3]) << 8;
		serviceble->remaining_write |= ((unsigned char)serviceble->characteristic_outgoing[4]) << 0;
		printf("Receiving length: %ld\n", serviceble->remaining_write);

		printf("Received chunk: %d\n", serviceble->characteristic_outgoing[0]);
		//printf("Write value: %s\n", characteristic_outgoing + 5);

		buffer_append(serviceble->buffer_write, serviceble->characteristic_outgoing + 5, serviceble->charlength - 5);

		serviceble->remaining_write -= serviceble->charlength - 5;
	}
	else {
		if ((serviceble->charlength - 1) > serviceble->remaining_write) {
			printf("Error, received too many bytes (%d out of %lu)\n", serviceble->charlength - 1, serviceble->remaining_write);
		}
		else {
			printf("Received chunk: %d\n", serviceble->characteristic_outgoing[0]);
			//printf("Write value: %s\n", characteristic_outgoing + 1);

			buffer_append(serviceble->buffer_write, serviceble->characteristic_outgoing + 1, serviceble->charlength - 1);

			serviceble->remaining_write -= serviceble->charlength - 1;
		}
	}
	
	if (serviceble->remaining_write == 0) {
		printf("Received: ");
		buffer_print(serviceble->buffer_write);
	}

	gatt_characteristic1_complete_write_value(object, invocation);

	send_data(serviceble, "BA", 5);

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

static void on_unregister_application(GattManager1 *proxy, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	gboolean result;
	GError *error;

	error = NULL;

	result = gatt_manager1_call_unregister_application_finish(proxy, res, &error);
	report_error(&error, "unregistering application callback");

	printf("Unregistered application with result %d\n", result);

	g_main_loop_quit(serviceble->loop);
}

/**
 * Advertisement unregistration callback
 *
 * @param proxy the advertisement manager proxy object
 * @param res the result of the operation
 * @param user_data the user data passed to the async callback
 */
static void on_unregister_advert(LEAdvertisingManager1 *proxy, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	gboolean result;
	GError *error;

	error = NULL;

	result = leadvertising_manager1_call_unregister_advertisement_finish(proxy, res, &error);
	report_error(&error, "unregistering advert callback");

	printf("Unregistered advert with result %d\n", result);

	gatt_manager1_call_unregister_application(serviceble->gattmanager, BLUEZ_GATT_OBJECT_PATH, NULL, (GAsyncReadyCallback)(&on_unregister_application), serviceble);
}


static void finish(ServiceBle * serviceble) {
	leadvertising_manager1_call_unregister_advertisement(serviceble->leadvertisingmanager, BLUEZ_ADVERT_PATH, NULL, (GAsyncReadyCallback)(&on_unregister_advert), serviceble);
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	g_printerr("%s\n", gdk_keyval_name (event->keyval));
	if (event->keyval == 'q') {
		finish(serviceble);
	}

	return FALSE;
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
	guint id;
	GDBusConnection * connection;
	GVariantDict dict_options;
	GVariant * arg_options;
	gboolean result;
	gchar const * uuids[] = {SERVICE_UUID, NULL};
	GDBusObjectManagerServer * object_manager_advert;
	ObjectSkeleton * object_advert;
	GDBusObjectManagerServer * object_manager_gatt;
	ObjectSkeleton * object_gatt_service;
	ObjectSkeleton * object_gatt_characteristic_outgoing;
	ObjectSkeleton * object_gatt_characteristic_incoming;
	const gchar * const charflags_outgoing[] = {"notify"};
	const gchar * const charflags_incoming[] = {"write", "write-without-response"};
	char uuid[sizeof(SERVICE_UUID) + 1];
	KeyPair * keypair;
	EC_KEY * publickey;
	Buffer * commitment;
	char characteristic[CHARACTERISTIC_LENGTH];
	ServiceBle * serviceble;

	serviceble = serviceble_new();

	gtk_init(&argc, &argv);

	strncpy(characteristic, CHARACTERISTIC_VALUE, CHARACTERISTIC_LENGTH);
	serviceble->charlength = strlen(characteristic);
	characteristic[CHARACTERISTIC_LENGTH - 1] = 0;
	error = NULL;
	serviceble->loop = g_main_loop_new(NULL, FALSE);

	keypair = keypair_new();
	commitment = buffer_new(0);
	result = keypair_import(keypair, "pico_pub_key.der", "pico_priv_key.der");
	if (result == FALSE) {
		printf("Failed to load keys\n");
	}


	publickey = keypair_getpublickey(keypair);


	result = cryptosupport_generate_commitment_base64(publickey, commitment);
	if (result == FALSE) {
		printf("Failed to generate commitment\n");
	}

	buffer_print(commitment);


	// "NdzdISywn1akt21lD/68HRlL6SHNguPSI2ULXXcHjzM="
	create_uuid(buffer_get_buffer(commitment), FALSE, uuid);
	strcpy(uuid, SERVICE_UUID);
	printf("UUID: %s\n", uuid);
	uuids[0] = uuid;

	buffer_delete(commitment);
	keypair_delete(keypair);


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
	serviceble->leadvertisement = leadvertisement1_skeleton_new();
	g_signal_connect(serviceble->leadvertisement, "handle-release", G_CALLBACK(&handle_release), NULL);

	// Set the advertisement properties
	leadvertisement1_set_service_uuids(serviceble->leadvertisement, uuids);
	leadvertisement1_set_type_(serviceble->leadvertisement, "peripheral");

	object_advert = object_skeleton_new (BLUEZ_ADVERT_PATH);
	object_skeleton_set_leadvertisement1(object_advert, serviceble->leadvertisement);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(object_manager_advert, G_DBUS_OBJECT_SKELETON(object_advert));
	g_dbus_object_manager_server_set_connection(object_manager_advert, connection);

	///////////////////////////////////////////////////////

	printf("Creating advertising manager\n");

	// Obtain a proxy for the LEAdvertisementMAanager1 interface
	serviceble->leadvertisingmanager = leadvertising_manager1_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, &error);
	report_error(&error, "creating advertising manager");

	///////////////////////////////////////////////////////
	
	printf("Register advertisement\n");

	// Call the RegisterAdvertisement method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	leadvertising_manager1_call_register_advertisement(serviceble->leadvertisingmanager, BLUEZ_ADVERT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_advert), NULL);

	///////////////////////////////////////////////////////
	
	printf("Creating Gatt manager\n");

	// Obtain a proxy for the Gattmanager1 interface
	serviceble->gattmanager = gatt_manager1_proxy_new_sync(connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, &error);
	report_error(&error, "creating gatt manager");

	///////////////////////////////////////////////////////

	printf("Creating Gatt service\n");

	// Publish the gatt service interface
	error = NULL;
	serviceble->gattservice = gatt_service1_skeleton_new();

	// Set the gatt service properties
	gatt_service1_set_uuid(serviceble->gattservice, uuid);
	gatt_service1_set_primary(serviceble->gattservice, TRUE);

	object_gatt_service = object_skeleton_new (BLUEZ_GATT_SERVICE_PATH);
	object_skeleton_set_gatt_service1(object_gatt_service, serviceble->gattservice);

	///////////////////////////////////////////////////////

	printf("Creating Gatt characteristic outgoing\n");

	// Publish the gatt characteristic interface
	error = NULL;
	serviceble->gattcharacteristic_outgoing = gatt_characteristic1_skeleton_new();

	// Set the gatt characteristic properties
	gatt_characteristic1_set_uuid (serviceble->gattcharacteristic_outgoing, CHARACTERISTIC_UUID_OUTGOING);
	gatt_characteristic1_set_service (serviceble->gattcharacteristic_outgoing, BLUEZ_GATT_SERVICE_PATH);
	gatt_characteristic1_set_notifying (serviceble->gattcharacteristic_outgoing, FALSE);
	gatt_characteristic1_set_flags (serviceble->gattcharacteristic_outgoing, charflags_outgoing);

	object_gatt_characteristic_outgoing = object_skeleton_new (BLUEZ_GATT_CHARACTERISTIC_PATH_OUTGOING);
	object_skeleton_set_gatt_characteristic1(object_gatt_characteristic_outgoing, serviceble->gattcharacteristic_outgoing);

	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-read-value", G_CALLBACK(&handle_read_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-write-value", G_CALLBACK(&handle_write_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-start-notify", G_CALLBACK(&handle_start_notify), NULL);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-stop-notify", G_CALLBACK(&handle_stop_notify), NULL);

	///////////////////////////////////////////////////////

	printf("Creating Gatt characteristic incoming\n");

	// Publish the gatt characteristic interface
	error = NULL;
	serviceble->gattcharacteristic_incoming = gatt_characteristic1_skeleton_new();

	// Set the gatt characteristic properties
	gatt_characteristic1_set_uuid (serviceble->gattcharacteristic_incoming, CHARACTERISTIC_UUID_INCOMING);
	gatt_characteristic1_set_service (serviceble->gattcharacteristic_incoming, BLUEZ_GATT_SERVICE_PATH);
	gatt_characteristic1_set_flags (serviceble->gattcharacteristic_incoming, charflags_incoming);

	object_gatt_characteristic_incoming = object_skeleton_new (BLUEZ_GATT_CHARACTERISTIC_PATH_INCOMING);
	object_skeleton_set_gatt_characteristic1(object_gatt_characteristic_incoming, serviceble->gattcharacteristic_incoming);

	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-read-value", G_CALLBACK(&handle_read_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-write-value", G_CALLBACK(&handle_write_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-start-notify", G_CALLBACK(&handle_start_notify), NULL);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-stop-notify", G_CALLBACK(&handle_stop_notify), NULL);

	///////////////////////////////////////////////////////

	printf("Creating object manager server\n");

	object_manager_gatt = g_dbus_object_manager_server_new(BLUEZ_GATT_OBJECT_PATH);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(object_manager_gatt, G_DBUS_OBJECT_SKELETON(object_gatt_service));
	g_dbus_object_manager_server_export(object_manager_gatt, G_DBUS_OBJECT_SKELETON(object_gatt_characteristic_outgoing));
	g_dbus_object_manager_server_export(object_manager_gatt, G_DBUS_OBJECT_SKELETON(object_gatt_characteristic_incoming));
	g_dbus_object_manager_server_set_connection(object_manager_gatt, connection);

	///////////////////////////////////////////////////////
	
	printf("Register gatt service\n");

	// Call the RegisterApplication method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	gatt_manager1_call_register_application(serviceble->gattmanager, BLUEZ_GATT_OBJECT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_application), NULL);

	///////////////////////////////////////////////////////

	GtkWidget * window;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	g_signal_connect(window, "key-release-event", G_CALLBACK(key_event), serviceble);

	gtk_widget_show (window);





	printf("Entering main loop\n");
	g_main_loop_run(serviceble->loop);

	printf("Exited main loop\n");	
	g_main_loop_unref(serviceble->loop);

	service_delete(serviceble);

	printf("The End\n");

	return 0;
}


