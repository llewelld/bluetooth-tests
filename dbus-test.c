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
#include "pico/fsmservice.h"

#include "bluetooth/bluetooth.h"
#include "bluetooth/hci.h"
#include "bluetooth/hci_lib.h"

// Defines

#define BLUEZ_SERVICE_NAME "org.bluez"
#define BLUEZ_OBJECT_PATH "/org/bluez"
#define BLUEZ_ADVERT_PATH "/org/bluez/hci0/advert1"
#define BLUEZ_DEVICE_PATH "/org/bluez/hci0"
#define SERVICE_UUID "68F9A6EE-0000-1000-8000-00805F9B34FB"
//#define CHARACTERISTIC_UUID "68F9A6EF-0000-1000-8000-00805F9B34FB"

#define CHARACTERISTIC_UUID_INCOMING "56add98a-0e8a-4113-85bf-6dc97b58a9c1"
#define CHARACTERISTIC_UUID_OUTGOING "56add98a-0e8a-4113-85bf-6dc97b58a9c2"

//#define SERVICE_UUID "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa0"
//#define CHARACTERISTIC_UUID_INCOMING "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa1"
//#define CHARACTERISTIC_UUID_OUTGOING "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa2"


#define CHARACTERISTIC_VALUE "012"
#define CHARACTERISTIC_LENGTH (208)
#define MAX_SEND_SIZE (128)

#if (MAX_SEND_SIZE > CHARACTERISTIC_LENGTH)
#error "The maximum length to send can't be larger than the characteristic size"
#endif

#define BLUEZ_GATT_OBJECT_PATH "/org/bluez/gatt"
#define BLUEZ_GATT_SERVICE_PATH "/org/bluez/gatt/service0"
#define BLUEZ_GATT_CHARACTERISTIC_PATH_OUTGOING "/org/bluez/gatt/service0/char0"
#define BLUEZ_GATT_CHARACTERISTIC_PATH_INCOMING "/org/bluez/gatt/service0/char1"

// Structure definitions

typedef enum _SERVICESTATE {
	SERVICESTATEBLE_INVALID = -1,

	SERVICESTATEBLE_DORMANT,
	SERVICESTATEBLE_INITIALISING,
	SERVICESTATEBLE_INITIALISED,
	SERVICESTATEBLE_ADVERTISING,
	SERVICESTATEBLE_ADVERTISINGCONTINUOUS,
	SERVICESTATEBLE_CONNECTED,
	SERVICESTATEBLE_UNADVERTISING,
	SERVICESTATEBLE_UNADVERTISED,
	SERVICESTATEBLE_FINALISING,
	SERVICESTATEBLE_FINALISED,

	SERVICESTATEBLE_NUM
} SERVICESTATE;

typedef struct _ServiceBle {
	GMainLoop * loop;
	FsmService * fsmservice;
	guint timeoutid;
	guint cycletimeoutid;

	LEAdvertisement1 * leadvertisement;
	LEAdvertisingManager1 * leadvertisingmanager;
	GattManager1 * gattmanager;
	GattService1 * gattservice;
	GattCharacteristic1 * gattcharacteristic_outgoing;
	GattCharacteristic1 * gattcharacteristic_incoming;
	unsigned char characteristic_outgoing[CHARACTERISTIC_LENGTH];
	unsigned char characteristic_incoming[CHARACTERISTIC_LENGTH];
	int charlength;
	size_t remaining_write;
	Buffer * buffer_write;
	Buffer * buffer_read;
	bool connected;
	SERVICESTATE state;
	bool cycling;
	size_t maxsendsize;
	size_t sendpos;
	GDBusObjectManagerServer * object_manager_advert;
	GDBusConnection * connection;
	GDBusObjectManagerServer * object_manager_gatt;
	ObjectSkeleton * object_gatt_service;
	ObjectSkeleton * object_gatt_characteristic_outgoing;
	ObjectSkeleton * object_gatt_characteristic_incoming;
	bool finalise;
} ServiceBle;

// Function prototypes

void serviceble_start(ServiceBle * serviceble);
void serviceble_stop(ServiceBle * serviceble);

static void serviceble_write(char const * data, size_t length, void * user_data);
static void serviceble_set_timeout(int timeout, void * user_data);
static void serviceble_error(void * user_data);
static void serviceble_listen(void * user_data);
static void serviceble_disconnect(void * user_data);
static void serviceble_authenticated(int status, void * user_data);
static void serviceble_session_ended(void * user_data);
static void serviceble_status_updated(int state, void * user_data);
static gboolean serviceble_timeout(gpointer user_data);

void advertising_start(ServiceBle * serviceble, bool continuous);
void advertising_stop(ServiceBle * serviceble, bool finalise);

static void finalise(ServiceBle * serviceble);
static void set_advertising_frequency();
static void appendbytes(char unsigned const * bytes, int num, Buffer * out);
static void create_uuid(KeyPair * keypair, bool continuous, Buffer * uuid);
static gboolean handle_release(LEAdvertisement1 * object, GDBusMethodInvocation * invocation, gpointer user_data);
static gboolean handle_read_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, GVariant *arg_options, gpointer user_data);
static void send_data(ServiceBle * serviceble, char const * data, size_t size);
static gboolean handle_write_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, GVariant *arg_value, GVariant *arg_options, gpointer user_data);
static gboolean handle_start_notify(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, gpointer user_data);
static gboolean handle_stop_notify(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, gpointer user_data);
static void report_error(GError ** error, char const * hint);
static void on_register_advert(LEAdvertisingManager1 *proxy, GAsyncResult *res, gpointer user_data);
static void on_register_application(GattManager1 *proxy, GAsyncResult *res, gpointer user_data);
static void on_unregister_advert(LEAdvertisingManager1 *proxy, GAsyncResult *res, gpointer user_data);
static gboolean key_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void generate_uuid(ServiceBle * serviceble, bool continuous, Buffer * uuid);
static void on_g_bus_get (GObject *source_object, GAsyncResult *res, gpointer user_data);
static void on_leadvertising_manager1_proxy_new(GDBusConnection * connection, GAsyncResult *res, gpointer user_data);
static void on_gatt_manager1_proxy_new(GDBusConnection * connection, GAsyncResult *res, gpointer user_data);
static void on_gatt_manager1_call_unregister_application(GattManager1 * gattmanager, GAsyncResult *res, gpointer user_data);
static gboolean cycle_timeout(gpointer user_data);
static void set_state(ServiceBle * serviceble, SERVICESTATE state);


ServiceBle * serviceble_new() {
	ServiceBle * serviceble;

	serviceble = CALLOC(sizeof(ServiceBle), 1);

	serviceble->loop = NULL;
	serviceble->fsmservice = fsmservice_new();
	serviceble->timeoutid = 0;
	serviceble->cycletimeoutid = 0;

	serviceble->leadvertisement = NULL;
	serviceble->leadvertisingmanager = NULL;
	serviceble->gattmanager = NULL;
	serviceble->gattservice = NULL;
	serviceble->gattcharacteristic_outgoing = NULL;
	serviceble->gattcharacteristic_incoming = NULL;
	serviceble->charlength = 0;
	serviceble->remaining_write = 0;
	serviceble->buffer_write = buffer_new(0);
	serviceble->buffer_read = buffer_new(0);
	serviceble->connected = FALSE;
	serviceble->state = SERVICESTATEBLE_INVALID;
	serviceble->cycling = FALSE;
	serviceble->maxsendsize = MAX_SEND_SIZE;
	serviceble->sendpos = 0;
	serviceble->object_manager_advert = NULL;
	serviceble->connection = NULL;
	serviceble->object_manager_gatt = NULL;
	serviceble->object_gatt_service = NULL;
	serviceble->object_gatt_characteristic_outgoing = NULL;
	serviceble->object_gatt_characteristic_incoming = NULL;
	serviceble->finalise = FALSE;

	fsmservice_set_functions(serviceble->fsmservice, serviceble_write, serviceble_set_timeout, serviceble_error, serviceble_listen, serviceble_disconnect, serviceble_authenticated, serviceble_session_ended, serviceble_status_updated);
	fsmservice_set_userdata(serviceble->fsmservice, serviceble);
	fsmservice_set_continuous(serviceble->fsmservice, TRUE);

	return serviceble;
}





void service_delete(ServiceBle * serviceble) {
	if (serviceble != NULL) {
		if (serviceble->fsmservice != NULL) {
			fsmservice_set_functions(serviceble->fsmservice, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
			fsmservice_set_userdata(serviceble->fsmservice, NULL);
			fsmservice_delete(serviceble->fsmservice);
			serviceble->fsmservice = NULL;
		}

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

static void set_advertising_frequency() {
	int result;
	int dd;
	int dev_id;
	char bytes_disable[] = {0x00};
	char bytes_interval[] = {0xA0, 0x00, 0xAF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00};
	char bytes_enable[] = {0x01};

	dev_id = hci_get_route(NULL);

	// Open device and return device descriptor
	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		printf("Device open failed");
	}

	// LE Set Advertising Enable Command
	// See section 7.8.9 of the Core Bluetooth Specification version 5
	// Parameters:
	// - Advertising_Enable (0 = disable; 1 = enable)
	result = hci_send_cmd(dd, 0x08, 0x000a, sizeof(bytes_disable), bytes_disable);
	if (result < 0) {
		printf("Error sending HCI command: disable");
	}

	// LE Set Advertising Parameters Command
	// See section 7.8.5 of the Core Bluetooth Specification version 5
	// Parameters:
	//  - Advertising_Interval_Min (0x000020 to 0xFFFFFF; Time = N * 0.625 ms)
	//  - Advertising_Interval_Max (0x000020 to 0xFFFFFF; Time = N * 0.625 ms)
	//  - Advertising_Type (0 = Connectable and scannable undirected advertising)
	//  - Own_Address_Type (0 = Public, 1 = Random)
	//  - Peer_Address_Type (0 = Public, 1 = Random)
	//  - Peer_Address (0xXXXXXXXXXXXX)
	//  - Advertising_Channel_Map (xxxxxxx1b = Chan 37, xxxxxx1xb = Chan 38, xxxxx1xxb = Chan 39, 00000111b = All)
	//  - Advertising_Filter_Policy (0 = No white list)
	result = hci_send_cmd(dd, 0x08, 0x0006, sizeof(bytes_interval), bytes_interval);
	if (result < 0) {
		printf("Error sending HCI command: disable");
	}

	// LE Set Advertising Enable Command
	// See section 7.8.9 of the Core Bluetooth Specification version 5
	// Parameters:
	// - Advertising_Enable (0 = disable; 1 = enable)
	result = hci_send_cmd(dd, 0x08, 0x000a, sizeof(bytes_enable), bytes_enable);
	if (result < 0) {
		printf("Error sending HCI command: disable");
	}

	hci_close_dev(dd);
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



static void send_data(ServiceBle * serviceble, char const * data, size_t size) {
	GVariant * variant;
	size_t sendsize;
	size_t buffersize;
	char * sendstart;
	//GVariant * variant2;

	// Store the data to send
	buffer_append_lengthprepend(serviceble->buffer_read, data, size);

	// Send in chunks
	buffersize = buffer_get_pos(serviceble->buffer_read);

	while (buffersize > 0) {
		sendsize = buffersize - serviceble->sendpos;
		sendstart = buffer_get_buffer(serviceble->buffer_read) + serviceble->sendpos;
		if (sendsize > serviceble->maxsendsize) {
			sendsize = serviceble->maxsendsize;
		}

		if (sendsize > 0) {
			printf("Sending chunk size %lu\n", sendsize);
			variant = g_variant_new_from_data (G_VARIANT_TYPE("ay"), sendstart, sendsize, TRUE, NULL, NULL);

			gatt_characteristic1_set_value (serviceble->gattcharacteristic_outgoing, variant);
			g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(serviceble->gattcharacteristic_outgoing));

			serviceble->sendpos += sendsize;
			if (serviceble->sendpos >= buffersize) {
				buffer_clear(serviceble->buffer_read);
				serviceble->sendpos = 0;
				buffersize = 0;
			}
		}
		else {
			printf("WARNING: send data size must be greater than zero\n");
		}
	}


	//variant1 = g_variant_new_from_data (G_VARIANT_TYPE("ay"), "", 0, TRUE, NULL, NULL);

	//gatt_characteristic1_set_value (gattcharacteristic_outgoing, variant2);
	//g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(gattcharacteristic_outgoing));
}

static gboolean handle_write_value(GattCharacteristic1 * object, GDBusMethodInvocation * invocation, GVariant *arg_value, GVariant *arg_options, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	GVariantIter * iter;
	guchar data;

	if (serviceble->connected == FALSE) {
		serviceble->connected = TRUE;
		set_state(serviceble, SERVICESTATEBLE_CONNECTED);
		fsmservice_connected(serviceble->fsmservice);
	}

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

		fsmservice_read(serviceble->fsmservice, buffer_get_buffer(serviceble->buffer_write), buffer_get_pos(serviceble->buffer_write));
	}

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
static void report_error(GError ** error, char const * hint) {
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

	result = leadvertising_manager1_call_unregister_advertisement_finish(proxy, res, &error);
	report_error(&error, "registering advert callback");

	printf("Registered advert with result %d\n", result);

	printf("Setting advertising frequency\n");
	set_advertising_frequency();
	printf("Advertising frequency set\n");
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

	set_state(serviceble, SERVICESTATEBLE_UNADVERTISED);

	// All stopped
	if (serviceble->connected == TRUE) {
		printf("Setting as disconnected\n");
		serviceble->connected = FALSE;
		fsmservice_disconnected(serviceble->fsmservice);
	}

	if (serviceble->finalise == TRUE) {
		finalise(serviceble);
	}
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	g_printerr("%s\n", gdk_keyval_name (event->keyval));
	if (event->keyval == 's') {
		serviceble_start(serviceble);
	}
	if (event->keyval == 'f') {
		serviceble_stop(serviceble);
	}
	if (event->keyval == 'q') {
		g_main_loop_quit(serviceble->loop);
	}

	return FALSE;
}

static void generate_uuid(ServiceBle * serviceble, bool continuous, Buffer * uuid) {
	KeyPair * keypair;
	gboolean result;

	serviceble->charlength = CHARACTERISTIC_LENGTH;

	keypair = keypair_new();

	result = keypair_import(keypair, "pico_pub_key.der", "pico_priv_key.der");
	if (result == FALSE) {
		printf("Failed to load keys\n");
	}

	// "NdzdISywn1akt21lD/68HRlL6SHNguPSI2ULXXcHjzM="
	create_uuid(keypair, continuous, uuid);
	//strcpy(uuid, SERVICE_UUID);
	//printf("UUID: %s\n", uuid);

	keypair_delete(keypair);
}

static void appendbytes(char unsigned const * bytes, int num, Buffer * out) {
	int pos;
	char letters[3];

	for (pos = 0; pos < num; pos++) {
		snprintf(letters, 3, "%02X", bytes[pos]);
		buffer_append(out, letters, 2);
	}
}

static void create_uuid(KeyPair * keypair, bool continuous, Buffer * uuid) {
	unsigned char a[4];
	unsigned char b[2];
	unsigned char c[2];
	unsigned char d[8];
	unsigned int pos;
	Buffer * commitment;
	char const * commitmentbytes;
	EC_KEY * publickey;
	gboolean result;

	commitment = buffer_new(0);
	publickey = keypair_getpublickey(keypair);

	result = cryptosupport_generate_commitment(publickey, commitment);
	if (result == FALSE) {
		printf("Failed to generate commitment\n");
	}

	buffer_print_base64(commitment);

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

	buffer_clear(uuid);
	appendbytes(a, 4, uuid);
	buffer_append_string(uuid, "-");
	appendbytes(b, 2, uuid);
	buffer_append_string(uuid, "-");
	appendbytes(c, 2, uuid);
	buffer_append_string(uuid, "-");
	appendbytes(d, 2, uuid);
	buffer_append_string(uuid, "-");
	appendbytes(d + 2, 6, uuid);

	buffer_delete(commitment);
}

void serviceble_stop(ServiceBle * serviceble) {
	advertising_stop(serviceble, TRUE);
}

void serviceble_start(ServiceBle * serviceble) {
	set_state(serviceble, SERVICESTATEBLE_INITIALISING);

	printf("Creating object manager server\n");

	serviceble->object_manager_advert = g_dbus_object_manager_server_new(BLUEZ_OBJECT_PATH);

	///////////////////////////////////////////////////////

	printf("Getting bus\n");

	// This is an asynchronous call, so initialisation continuous in the callback
	g_bus_get(G_BUS_TYPE_SYSTEM, NULL, (GAsyncReadyCallback)(&on_g_bus_get), serviceble);

	// Set up to periodically restart
	serviceble->cycletimeoutid = g_timeout_add(10000, cycle_timeout, serviceble);

	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////
}

static gboolean cycle_timeout(gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	bool recycle;

	printf("\n\nXXXXXXXXXXXXXXXXXX CYCLE\n");

	recycle = TRUE;

	switch (serviceble->state) {
		case SERVICESTATEBLE_INITIALISING:
		case SERVICESTATEBLE_UNADVERTISING:
		case SERVICESTATEBLE_FINALISING:
		case SERVICESTATEBLE_CONNECTED:
		case SERVICESTATEBLE_ADVERTISINGCONTINUOUS:
			// Do nothing, wait again
			break;
		case SERVICESTATEBLE_ADVERTISING:
		case SERVICESTATEBLE_INITIALISED:
		case SERVICESTATEBLE_UNADVERTISED:
			serviceble->cycling = TRUE;
			break;
		case SERVICESTATEBLE_FINALISED:
			recycle = FALSE;
			break;
		case SERVICESTATEBLE_DORMANT:
		case SERVICESTATEBLE_INVALID:
		case SERVICESTATEBLE_NUM:
		default:
			printf("XXXXXXXXXXXXXXXXXX ERROR\n\n\n");
			printf("Cycle during invalid state\n");
			break;
	}

	if (serviceble->cycling == TRUE) {
		printf("XXXXXXXXXXXXXXXXXX RECYCLE\n\n\n");
		recycle = FALSE;
		serviceble_stop(serviceble);
	}
	else {
		printf("XXXXXXXXXXXXXXXXXX IGNORE\n\n\n");
	}

	if (recycle == FALSE) {
		// This timeout fires only once
		serviceble->cycletimeoutid = 0;
	}

	return recycle;
}




static void on_g_bus_get (GObject *source_object, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	GError *error;

	error = NULL;

	serviceble->connection = g_bus_get_finish(res, & error);
	report_error(&error, "getting bus");

	if (serviceble->connection != NULL) {
		printf("Creating advertising manager\n");

		// Obtain a proxy for the LEAdvertisementMAanager1 interface
		// This is an asynchronous call, so initialisation continuous in the callback
		leadvertising_manager1_proxy_new(serviceble->connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, (GAsyncReadyCallback)(&on_leadvertising_manager1_proxy_new), serviceble);
	}
}

static void on_leadvertising_manager1_proxy_new(GDBusConnection * connection, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	GError *error;

	error = NULL;

	serviceble->leadvertisingmanager = leadvertising_manager1_proxy_new_finish(res, &error);
	report_error(&error, "creating advertising manager");

	if (serviceble->leadvertisingmanager != NULL) {
		printf("Creating Gatt manager\n");

		// Obtain a proxy for the Gattmanager1 interface
		// This is an asynchronous call, so initialisation continuous in the callback
		gatt_manager1_proxy_new(serviceble->connection, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_SERVICE_NAME, BLUEZ_DEVICE_PATH, NULL, (GAsyncReadyCallback)(&on_gatt_manager1_proxy_new), serviceble);
	}
}

static void on_gatt_manager1_proxy_new(GDBusConnection * connection, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	GError *error;

	error = NULL;

	serviceble->gattmanager = gatt_manager1_proxy_new_finish (res, &error);
	report_error(&error, "creating gatt manager");

	if (serviceble->gattmanager != NULL) {
		///////////////////////////////////////////////////////

		printf("Creating object manager server\n");

		serviceble->object_manager_gatt = g_dbus_object_manager_server_new(BLUEZ_GATT_OBJECT_PATH);

		printf("Service established\n");
		set_state(serviceble, SERVICESTATEBLE_INITIALISED);

		// Initialisation is complete, now start advertising
		advertising_start(serviceble, FALSE);
	}
}


static void finalise(ServiceBle * serviceble) {
	set_state(serviceble, SERVICESTATEBLE_FINALISING);

	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////
	///////////////////////////////////////////////////////

	printf("Releasing object manager server\n");

	g_object_unref(serviceble->object_manager_advert);
	serviceble->object_manager_advert = NULL;

	///////////////////////////////////////////////////////

	printf("Releasing bus\n");

	g_object_unref(serviceble->connection);
	serviceble->connection = NULL;

	///////////////////////////////////////////////////////

	printf("Releasing advertising manager\n");

	g_object_unref(serviceble->leadvertisingmanager);
	serviceble->leadvertisingmanager = NULL;

	///////////////////////////////////////////////////////

	printf("Releasing Gatt manager\n");

	g_object_unref(serviceble->gattmanager);
	serviceble->gattmanager = NULL;

	///////////////////////////////////////////////////////

	printf("Releasing object manager server\n");
	g_object_unref(serviceble->object_manager_gatt);
	serviceble->object_manager_gatt = NULL;

	///////////////////////////////////////////////////////

	set_state(serviceble, SERVICESTATEBLE_FINALISED);

	// Remove the timeout
	g_source_remove(serviceble->cycletimeoutid);
	serviceble->cycletimeoutid = 0;

	// This is a recycle stop, so we need to start again
	if (serviceble->cycling == TRUE) {
		serviceble->cycling = FALSE;
		serviceble_start(serviceble);
	}
}

void advertising_start(ServiceBle * serviceble, bool continuous) {
	gchar const * uuids[] = {SERVICE_UUID, NULL};
	Buffer * uuid;
	ObjectSkeleton * object_advert;
	GVariantDict dict_options;
	const gchar * const charflags_outgoing[] = {"notify", NULL};
	const gchar * const charflags_incoming[] = {"write", "write-without-response", NULL};
	GVariant * variant1;
	GVariant * variant2;
	GVariant * arg_options;

	uuid = buffer_new(0);
	generate_uuid(serviceble, continuous, uuid);
	uuids[0] = buffer_get_buffer(uuid);

	printf("Creating advertisement\n");

	// Publish the advertisement interface
	serviceble->leadvertisement = leadvertisement1_skeleton_new();
	g_signal_connect(serviceble->leadvertisement, "handle-release", G_CALLBACK(&handle_release), NULL);

	// Set the advertisement properties
	leadvertisement1_set_service_uuids(serviceble->leadvertisement, uuids);
	leadvertisement1_set_type_(serviceble->leadvertisement, "peripheral");

	object_advert = object_skeleton_new (BLUEZ_ADVERT_PATH);
	object_skeleton_set_leadvertisement1(object_advert, serviceble->leadvertisement);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(serviceble->object_manager_advert, G_DBUS_OBJECT_SKELETON(object_advert));
	g_dbus_object_manager_server_set_connection(serviceble->object_manager_advert, serviceble->connection);

	///////////////////////////////////////////////////////
	
	printf("Register advertisement\n");

	// Call the RegisterAdvertisement method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	leadvertising_manager1_call_register_advertisement(serviceble->leadvertisingmanager, BLUEZ_ADVERT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_advert), NULL);

	///////////////////////////////////////////////////////

	printf("Creating Gatt service\n");

	// Publish the gatt service interface
	serviceble->gattservice = gatt_service1_skeleton_new();

	// Set the gatt service properties
	gatt_service1_set_uuid(serviceble->gattservice, buffer_get_buffer(uuid));
	gatt_service1_set_primary(serviceble->gattservice, TRUE);

	serviceble->object_gatt_service = object_skeleton_new (BLUEZ_GATT_SERVICE_PATH);
	object_skeleton_set_gatt_service1(serviceble->object_gatt_service, serviceble->gattservice);

	///////////////////////////////////////////////////////

	printf("Creating Gatt characteristic outgoing\n");

	// Publish the gatt characteristic interface
	serviceble->gattcharacteristic_outgoing = gatt_characteristic1_skeleton_new();

	// Initialise the characteristic value
	buffer_clear(serviceble->buffer_read);
	variant1 = g_variant_new_from_data (G_VARIANT_TYPE("ay"), buffer_get_buffer(serviceble->buffer_read), buffer_get_pos(serviceble->buffer_read), TRUE, NULL, NULL);
	gatt_characteristic1_set_value (serviceble->gattcharacteristic_outgoing, variant1);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(serviceble->gattcharacteristic_outgoing));

	// Set the gatt characteristic properties
	gatt_characteristic1_set_uuid (serviceble->gattcharacteristic_outgoing, CHARACTERISTIC_UUID_OUTGOING);
	gatt_characteristic1_set_service (serviceble->gattcharacteristic_outgoing, BLUEZ_GATT_SERVICE_PATH);
	gatt_characteristic1_set_notifying (serviceble->gattcharacteristic_outgoing, FALSE);
	gatt_characteristic1_set_flags (serviceble->gattcharacteristic_outgoing, charflags_outgoing);

	serviceble->object_gatt_characteristic_outgoing = object_skeleton_new (BLUEZ_GATT_CHARACTERISTIC_PATH_OUTGOING);
	object_skeleton_set_gatt_characteristic1(serviceble->object_gatt_characteristic_outgoing, serviceble->gattcharacteristic_outgoing);

	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-read-value", G_CALLBACK(&handle_read_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-write-value", G_CALLBACK(&handle_write_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-start-notify", G_CALLBACK(&handle_start_notify), NULL);
	g_signal_connect(serviceble->gattcharacteristic_outgoing, "handle-stop-notify", G_CALLBACK(&handle_stop_notify), NULL);

	///////////////////////////////////////////////////////

	printf("Creating Gatt characteristic incoming\n");

	// Publish the gatt characteristic interface
	serviceble->gattcharacteristic_incoming = gatt_characteristic1_skeleton_new();

	// Initialise the characteristic value
	buffer_clear(serviceble->buffer_write);
	variant2 = g_variant_new_from_data (G_VARIANT_TYPE("ay"), buffer_get_buffer(serviceble->buffer_write), buffer_get_pos(serviceble->buffer_write), TRUE, NULL, NULL);
	gatt_characteristic1_set_value (serviceble->gattcharacteristic_incoming, variant2);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(serviceble->gattcharacteristic_incoming));

	// Set the gatt characteristic properties
	gatt_characteristic1_set_uuid (serviceble->gattcharacteristic_incoming, CHARACTERISTIC_UUID_INCOMING);
	gatt_characteristic1_set_service (serviceble->gattcharacteristic_incoming, BLUEZ_GATT_SERVICE_PATH);
	gatt_characteristic1_set_flags (serviceble->gattcharacteristic_incoming, charflags_incoming);

	serviceble->object_gatt_characteristic_incoming = object_skeleton_new (BLUEZ_GATT_CHARACTERISTIC_PATH_INCOMING);
	object_skeleton_set_gatt_characteristic1(serviceble->object_gatt_characteristic_incoming, serviceble->gattcharacteristic_incoming);

	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-read-value", G_CALLBACK(&handle_read_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-write-value", G_CALLBACK(&handle_write_value), serviceble);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-start-notify", G_CALLBACK(&handle_start_notify), NULL);
	g_signal_connect(serviceble->gattcharacteristic_incoming, "handle-stop-notify", G_CALLBACK(&handle_stop_notify), NULL);

	///////////////////////////////////////////////////////

	printf("Exporting object manager server\n");

	g_dbus_object_manager_server_export(serviceble->object_manager_gatt, G_DBUS_OBJECT_SKELETON(serviceble->object_gatt_service));
	g_dbus_object_manager_server_export(serviceble->object_manager_gatt, G_DBUS_OBJECT_SKELETON(serviceble->object_gatt_characteristic_outgoing));
	g_dbus_object_manager_server_export(serviceble->object_manager_gatt, G_DBUS_OBJECT_SKELETON(serviceble->object_gatt_characteristic_incoming));
	g_dbus_object_manager_server_set_connection(serviceble->object_manager_gatt, serviceble->connection);

	///////////////////////////////////////////////////////
	
	printf("Register gatt service\n");

	// Call the RegisterApplication method on the proxy
	g_variant_dict_init(& dict_options, NULL);
	arg_options = g_variant_dict_end(& dict_options);

	gatt_manager1_call_register_application(serviceble->gattmanager, BLUEZ_GATT_OBJECT_PATH, arg_options, NULL, (GAsyncReadyCallback)(&on_register_application), NULL);

	if (continuous) {
		set_state(serviceble, SERVICESTATEBLE_ADVERTISINGCONTINUOUS);
	}
	else {
		set_state(serviceble, SERVICESTATEBLE_ADVERTISING);
	}
	// All started
	//if (serviceble->connected == FALSE) {
	//	serviceble->connected = TRUE;
	//	fsmservice_connected(serviceble->fsmservice);
	//}
	
	buffer_delete(uuid);
	uuid = NULL;
}

void advertising_stop(ServiceBle * serviceble, bool finalise) {
	set_state(serviceble, SERVICESTATEBLE_UNADVERTISING);

	serviceble->finalise = finalise;

	///////////////////////////////////////////////////////

	printf("Unregister gatt service\n");

	// This is an asynchronous call, so advertisement stopping continuous in the callback
	gatt_manager1_call_unregister_application(serviceble->gattmanager, BLUEZ_GATT_OBJECT_PATH, NULL, (GAsyncReadyCallback)(&on_gatt_manager1_call_unregister_application), serviceble);
}


static void on_gatt_manager1_call_unregister_application(GattManager1 * gattmanager, GAsyncResult *res, gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;
	GError *error;
	gboolean result;
	guint matchedsignals;

	error = NULL;

	result = gatt_manager1_call_unregister_application_finish(gattmanager, res, &error);
	report_error(&error, "unregistering gatt service");
	if (result == FALSE) {
		printf("Gatt service failed to unregister\n");
	}

	///////////////////////////////////////////////////////

	printf("Unexporting object manager server\n");

	g_dbus_object_manager_server_unexport (serviceble->object_manager_gatt, BLUEZ_GATT_SERVICE_PATH);
	g_dbus_object_manager_server_unexport (serviceble->object_manager_gatt, BLUEZ_GATT_CHARACTERISTIC_PATH_OUTGOING);
	g_dbus_object_manager_server_unexport (serviceble->object_manager_gatt, BLUEZ_GATT_CHARACTERISTIC_PATH_INCOMING);

	///////////////////////////////////////////////////////

	printf("Disconnect signals\n");

	matchedsignals = 0;

	// Disconnect signals on outgoing characteristic
	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_outgoing, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_read_value), serviceble);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_outgoing, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_write_value), serviceble);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_outgoing, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_start_notify), NULL);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_outgoing, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_stop_notify), NULL);

	// Disconnect signals on incoming characteristic
	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_incoming, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_read_value), serviceble);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_incoming, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_write_value), serviceble);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_incoming, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_start_notify), NULL);

	matchedsignals += g_signal_handlers_disconnect_matched (serviceble->gattcharacteristic_incoming, (G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA), 0, 0, NULL, G_CALLBACK(&handle_stop_notify), NULL);

	printf("Removed %u signals\n", matchedsignals);

	///////////////////////////////////////////////////////

	printf("Destroy server-side dbus objecs\n");

	g_object_unref(serviceble->object_gatt_characteristic_incoming);
	g_object_unref(serviceble->object_gatt_characteristic_outgoing);
	g_object_unref(serviceble->object_gatt_service);
	g_object_unref(serviceble->gattservice);

	///////////////////////////////////////////////////////

	printf("Unregister advertisement\n");

	leadvertising_manager1_call_unregister_advertisement (serviceble->leadvertisingmanager, BLUEZ_ADVERT_PATH, NULL, (GAsyncReadyCallback)(&on_unregister_advert), serviceble);

	///////////////////////////////////////////////////////

	printf("Release advertisement\n");

	// Publish the advertisement interface
	//g_object_unref(serviceble->leadvertisement);
}



static void serviceble_write(char const * data, size_t length, void * user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	printf("Sending data %s\n", data);

	send_data(serviceble, data, length);
}

static void serviceble_set_timeout(int timeout, void * user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Requesting timeout of %d", timeout);
	printf("Requesting timeout of %d\n", timeout);

	// Remove any previous timeout
	if (serviceble->timeoutid != 0) {
		g_source_remove(serviceble->timeoutid);
		serviceble->timeoutid = 0;
	}

	serviceble->timeoutid = g_timeout_add(timeout, serviceble_timeout, serviceble);
}

static void serviceble_error(void * user_data) {
	//ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Error");
	printf("Error\n");
}

static void serviceble_listen(void * user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	printf("Requesting to listen\n");
	if (serviceble->connected == FALSE) {
		LOG(LOG_DEBUG, "Listening");
		printf("Listening\n");

		advertising_start(serviceble, TRUE);
	}
}

static void serviceble_disconnect(void * user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Requesting disconnect");
	printf("Requesting disconnect\n");

	if (serviceble->connected == TRUE) {
		advertising_stop(serviceble, FALSE);
	}
}

static void serviceble_authenticated(int status, void * user_data) {
	//ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Authenticated");
	printf("Authenticated status: %d\n", status);
}

static void serviceble_session_ended(void * user_data) {
	//ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Session ended");
	printf("Session ended\n");
}

static void serviceble_status_updated(int state, void * user_data) {
	//ServiceBle * serviceble = (ServiceBle *)user_data;

	LOG(LOG_DEBUG, "Update, state: %d", state);
	printf("Update, state: %d\n", state);
}

static gboolean serviceble_timeout(gpointer user_data) {
	ServiceBle * serviceble = (ServiceBle *)user_data;

	// This timeout fires only once
	serviceble->timeoutid = 0;

	LOG(LOG_DEBUG, "Calling timeout");
	fsmservice_timeout(serviceble->fsmservice);

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
	ServiceBle * serviceble;
	GtkWidget * window;
	Shared * shared;
	Users * users;
	USERFILE usersresult;
	Buffer * extradata;

	gtk_init(&argc, &argv);

	printf("Initialising\n");
	serviceble = serviceble_new();

	serviceble->loop = g_main_loop_new(NULL, FALSE);

	serviceble_start(serviceble);

	shared = shared_new();
	shared_load_or_generate_keys(shared, "pico_pub_key.der", "pico_priv_key.der");

	users = users_new();
	usersresult = users_load(users, "users.txt");
	if (usersresult != USERFILE_SUCCESS) {
		printf("Failed to load user file\n");
	}

	extradata = buffer_new(0);

	fsmservice_start(serviceble->fsmservice, shared, users, extradata);

	///////////////////////////////////////////////////////

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(window, "key-release-event", G_CALLBACK(key_event), serviceble);
	gtk_widget_show (window);

	printf("Entering main loop\n");
	g_main_loop_run(serviceble->loop);

	printf("Exited main loop\n");
	g_main_loop_unref(serviceble->loop);

	service_delete(serviceble);
	shared_delete(shared);
	users_delete(users);
	buffer_delete(extradata);

	printf("The End\n");

	return 0;
}

static void set_state(ServiceBle * serviceble, SERVICESTATE state) {
	printf("State transition: %d -> %d\n", serviceble->state, state);

	serviceble->state = state;
}




