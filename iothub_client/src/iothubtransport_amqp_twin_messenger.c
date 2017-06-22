// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdbool.h>
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/agenttime.h" 
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_uamqp_c/messaging.h"
#include "uamqp_messaging.h"
#include "iothub_client_private.h"
#include "iothubtransport_amqp_messenger.h"
#include "iothubtransport_amqp_twin_messenger.h"

#define RESULT_OK 0
#define INDEFINITE_TIME ((time_t)(-1))

#define CLIENT_VERSION_PROPERTY_NAME					"com.microsoft:client-version"
#define IOTHUB_DEVICES_PATH_FMT                         "%s/devices/%s"
#define IOTHUB_EVENT_SEND_ADDRESS_FMT                   "amqps://%s/messages/events"
#define IOTHUB_MESSAGE_RECEIVE_ADDRESS_FMT              "amqps://%s/messages/devicebound"
#define MESSAGE_SENDER_LINK_NAME_PREFIX                 "link-snd"
#define MESSAGE_SENDER_MAX_LINK_SIZE                    UINT64_MAX
#define MESSAGE_RECEIVER_LINK_NAME_PREFIX               "link-rcv"
#define MESSAGE_RECEIVER_MAX_LINK_SIZE                  65536
#define DEFAULT_EVENT_SEND_RETRY_LIMIT                  10
#define DEFAULT_EVENT_SEND_TIMEOUT_SECS                 600
#define MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS    300
#define MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS  300
#define UNIQUE_ID_BUFFER_SIZE                           37
#define STRING_NULL_TERMINATOR                          '\0'

#define EMPTY_TWIN_BODY_DATA                            ((const unsigned char*)" ")
#define EMPTY_TWIN_BODY_SIZE                            1

#define TWIN_MESSAGE_PROPERTY_OPERATION	"operation"
#define TWIN_MESSAGE_PROPERTY_RESOURCE  "resource"
#define TWIN_MESSAGE_PROPERTY_VERSION   "version"

#define TWIN_RESOURCE					"/notifications/twin/properties/desired"

#define TWIN_CORRELATION_ID_PROPERTY_NAME				"com.microsoft:channel-correlation-id"
#define TWIN_API_VERSION_PROPERTY_NAME					"com.microsoft:api-version"
#define TWIN_CORRELATION_ID_PROPERTY_FORMAT				"twin:%s"
#define TWIN_API_VERSION_NUMBER							"2016-11-14"
 
static char* DEFAULT_DEVICES_PATH_FORMAT = "%s/devices/%s";
static char* DEFAULT_TWIN_SEND_LINK_SOURCE_NAME = "twin/";
static char* DEFAULT_TWIN_RECEIVE_LINK_TARGET_NAME = "twin/";

static const char* TWIN_OPERATION_PATCH = "PATCH";
static const char* TWIN_OPERATION_GET = "GET";
static const char* TWIN_OPERATION_PUT = "PUT";
static const char* TWIN_OPERATION_DELETE = "DELETE";

#define TWIN_OPERATION_TYPE_STRINGS \
	TWIN_OPERATION_TYPE_PATCH, \
	TWIN_OPERATION_TYPE_GET, \
	TWIN_OPERATION_TYPE_PUT, \
	TWIN_OPERATION_TYPE_DELETE

DEFINE_LOCAL_ENUM(TWIN_OPERATION_TYPE, TWIN_OPERATION_TYPE_STRINGS);

#define TWIN_SUBSCRIPTION_STATE_STRINGS \
	TWIN_SUBSCRIPTION_STATE_NOT_SUBSCRIBED, \
	TWIN_SUBSCRIPTION_STATE_GET_COMPLETE_PROPERTIES, \
	TWIN_SUBSCRIPTION_STATE_SUBSCRIBE_FOR_UPDATES, \
	TWIN_SUBSCRIPTION_STATE_SUBSCRIBED, \
	TWIN_SUBSCRIPTION_STATE_UNSUBSCRIBE

DEFINE_LOCAL_ENUM(TWIN_SUBSCRIPTION_STATE, TWIN_SUBSCRIPTION_STATE_STRINGS);

typedef struct TWIN_MESSENGER_INSTANCE_TAG
{
	char* client_version;
	char* device_id;
	char* iothub_host_fqdn;

	TWIN_MESSENGER_STATE state;
	
	TWIN_MESSENGER_STATE_CHANGED_CALLBACK on_state_changed_callback;
	void* on_state_changed_context;

	TWIN_SUBSCRIPTION_STATE subscription_state;
	TWIN_STATE_UPDATE_CALLBACK on_message_received_callback;
	void* on_message_received_context;

	AMQP_MESSENGER_HANDLE amqp_msgr;
} TWIN_MESSENGER_INSTANCE;

typedef struct TWIN_OPERATION_CONTEXT_TAG
{
	TWIN_OPERATION_TYPE type;
	TWIN_MESSENGER_INSTANCE* msgr;
	char* correlation_id;
	TWIN_MESSENGER_REPORT_STATE_COMPLETE_CALLBACK on_report_state_complete_callback;
	const void* on_report_state_complete_context;
} TWIN_OPERATION_CONTEXT;


//---------- AMQP Helper Functions ----------//

static int set_message_correlation_id(MESSAGE_HANDLE message, const char* value)
{
	int result;
	PROPERTIES_HANDLE properties;

	if (message_get_properties(message, &properties) != 0)
	{
		LogError("Failed getting the AMQP message properties");
		result = __FAILURE__;
	}
	else if (properties == NULL && (properties = properties_create()) == NULL)
	{
		LogError("Failed creating properties for AMQP message");
		result = __FAILURE__;
	}
	else
	{
		AMQP_VALUE amqp_value;

		if ((amqp_value = amqpvalue_create_string(value)) == NULL)
		{
			LogError("Failed creating AMQP value for correlation-id");
			result = __FAILURE__;
		}
		else
		{
			if (properties_set_correlation_id(properties, amqp_value) != RESULT_OK)
			{
				LogError("Failed setting the correlation id");
				result = __FAILURE__;
			}
			else if (message_set_properties(message, properties) != RESULT_OK)
			{
				LogError("Failed setting the AMQP message properties");
				result = __FAILURE__;
			}
			else
			{
				result = RESULT_OK;
			}

			amqpvalue_destroy(amqp_value);
		}

		properties_destroy(properties);
	}

	return result;
}

static int add_map_item(AMQP_VALUE map, const char* name, const char* value)
{
	int result;
	AMQP_VALUE amqp_value_name;

	if ((amqp_value_name = amqpvalue_create_string(name)) == NULL)
	{
		LogError("Failed creating AMQP_VALUE for name");
		result = __FAILURE__;
	}
	else
	{
		AMQP_VALUE amqp_value_value = NULL;

		if (value == NULL && (amqp_value_value = amqpvalue_create_null()) == NULL)
		{
			LogError("Failed creating AMQP_VALUE for NULL value");
			result = __FAILURE__;
		}
		else if (value != NULL && (amqp_value_value = amqpvalue_create_string(value)) == NULL)
		{
			LogError("Failed creating AMQP_VALUE for value");
			result = __FAILURE__;
		}
		else
		{
			if (amqpvalue_set_map_value(map, amqp_value_name, amqp_value_value) != 0)
			{
				LogError("Failed adding key/value pair to map");
				result = __FAILURE__;
			}
			else
			{
				result = RESULT_OK;
			}

			amqpvalue_destroy(amqp_value_value);
		}

		amqpvalue_destroy(amqp_value_name);
	}

	return result;
}

static int add_amqp_message_annotation(MESSAGE_HANDLE message, AMQP_VALUE msg_annotations_map)
{
	int result;
	AMQP_VALUE msg_annotations;

	if ((msg_annotations = amqpvalue_create_message_annotations(msg_annotations_map)) == NULL)
	{
		LogError("Failed creating new AMQP message annotations");
		result = __FAILURE__;
	}
	else
	{
		if (message_set_message_annotations(message, (annotations)msg_annotations) != 0)
		{
			LogError("Failed setting AMQP message annotations");
			result = __FAILURE__;
		}
		else
		{
			result = RESULT_OK;
		}

		amqpvalue_destroy(msg_annotations);
	}

	return result;
}


//---------- TWIN Helpers ----------//

static char* generate_unique_id()
{
	char* result;

	if ((result = (char*)malloc(sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1)) == NULL)
	{
		LogError("Failed generating an unique tag (malloc failed)");
	}
	else
	{
		memset(result, 0, sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1);

		if (UniqueId_Generate(result, UNIQUE_ID_BUFFER_SIZE) != UNIQUEID_OK)
		{
			LogError("Failed generating an unique tag (UniqueId_Generate failed)");
			free(result);
			result = NULL;
		}
	}

	return result;
}

static char* generate_twin_correlation_id()
{
	char* result;
	char* unique_id;

	if ((unique_id = generate_unique_id()) == NULL)
	{
		LogError("Failed generating unique ID for correlation-id");
		result = NULL;
	}
	else
	{
		if ((result = (char*)malloc(strlen(TWIN_CORRELATION_ID_PROPERTY_FORMAT) + strlen(unique_id) + 1)) == NULL)
		{
			LogError("Failed allocating correlation-id");
			result = NULL;
		}
		else
		{
			(void)sprintf(result, TWIN_CORRELATION_ID_PROPERTY_FORMAT, unique_id);
		}

		free(unique_id);
	}

	return result;
}

static TWIN_OPERATION_CONTEXT* create_twin_operation_context(TWIN_MESSENGER_INSTANCE* twin_msgr, TWIN_OPERATION_TYPE type)
{
	TWIN_OPERATION_CONTEXT* result;

	if ((result = (TWIN_OPERATION_CONTEXT*)malloc(sizeof(TWIN_OPERATION_CONTEXT))) == NULL)
	{
		LogError("Failed creating context for %s", ENUM_TO_STRING(TWIN_OPERATION_TYPE, type));
	}
	else
	{
		memset(result, 0, sizeof(TWIN_OPERATION_CONTEXT));

		if ((result->correlation_id = generate_unique_id()) == NULL)
		{
			LogError("Failed setting context correlation-id");
			free(result);
			result = NULL;
		}
		else
		{
			result->type = type;
			result->msgr = twin_msgr;
		}
	}

	return result;
}

static void destroy_twin_operation_context(TWIN_OPERATION_CONTEXT* op_ctx)
{
	free(op_ctx->correlation_id);
	free(op_ctx);
}


//---------- TWIN <-> AMQP Translation Functions ----------//

static void destroy_link_attach_properties(MAP_HANDLE properties)
{
	Map_Destroy(properties);
}

static MAP_HANDLE create_link_attach_properties(TWIN_MESSENGER_INSTANCE* twin_msgr)
{
	MAP_HANDLE result;

	if ((result = Map_Create(NULL)) == NULL)
	{
		LogError("Failed creating map for AMQP link properties");
	}
	else
	{
		char* correlation_id;

		if ((correlation_id = generate_twin_correlation_id()) == NULL)
		{
			LogError("Failed adding AMQP link property ");
			destroy_link_attach_properties(result);
			result = NULL;
		}
		else
		{
			if (Map_Add(result, CLIENT_VERSION_PROPERTY_NAME, twin_msgr->client_version) != MAP_OK)
			{
				LogError("Failed adding AMQP link property (client version)");
				destroy_link_attach_properties(result);
				result = NULL;
			}
			else if (Map_Add(result, TWIN_CORRELATION_ID_PROPERTY_NAME, correlation_id) != MAP_OK)
			{
				LogError("Failed adding AMQP link property (correlation-id)");
				destroy_link_attach_properties(result);
				result = NULL;
			}
			else if (Map_Add(result, TWIN_API_VERSION_PROPERTY_NAME, TWIN_API_VERSION_NUMBER) != MAP_OK)
			{
				LogError("Failed adding AMQP link property (api-version)");
				destroy_link_attach_properties(result);
				result = NULL;
			}

			free(correlation_id);
		}
	}

	return result;
}

static const char* get_twin_operation_name(TWIN_OPERATION_TYPE op_type)
{
	const char* result;

	switch (op_type)
	{
		default:
			LogError("Unrecognized TWIN operation (%s)", ENUM_TO_STRING(TWIN_OPERATION_TYPE, op_type));
			result = NULL;
			break;
		case TWIN_OPERATION_TYPE_PATCH:
			result = TWIN_OPERATION_PATCH;
			break;
		case TWIN_OPERATION_TYPE_GET:
			result = TWIN_OPERATION_GET;
			break;
		case TWIN_OPERATION_TYPE_PUT:
			result = TWIN_OPERATION_PUT;
			break;
		case TWIN_OPERATION_TYPE_DELETE:
			result = TWIN_OPERATION_DELETE;
			break;
	}

	return result;
}

static MESSAGE_HANDLE create_amqp_message_for_twin_operation(TWIN_OPERATION_TYPE op_type, char* correlation_id, CONSTBUFFER_HANDLE data)
{
	MESSAGE_HANDLE result;
	const char* twin_op_name;

	if ((twin_op_name = get_twin_operation_name(op_type))== NULL)
	{
		result = NULL;
	}
	else if ((result = message_create()) == NULL)
	{
		LogError("Failed creating AMQP message (%s)", twin_op_name);
	}
	else
	{
		AMQP_VALUE msg_annotations_map;

		if ((msg_annotations_map = amqpvalue_create_map()) == NULL)
		{
			LogError("Failed creating map for message annotations");
			message_destroy(result);
			result = NULL;
		}
		else
		{
			if (add_map_item(msg_annotations_map, TWIN_MESSAGE_PROPERTY_OPERATION, twin_op_name) != RESULT_OK)
			{
				LogError("Failed adding operation to AMQP message annotations (%s)", twin_op_name);
				message_destroy(result);
				result = NULL;
			}
			else if (add_map_item(msg_annotations_map, TWIN_MESSAGE_PROPERTY_RESOURCE, TWIN_RESOURCE) != RESULT_OK)
			{
				LogError("Failed adding resource to AMQP message annotations (%s)", twin_op_name);
				message_destroy(result);
				result = NULL;
			}
			else if (add_amqp_message_annotation(result, msg_annotations_map) != RESULT_OK)
			{
				LogError("Failed adding annotations to AMQP message (%s)", twin_op_name);
				message_destroy(result);
				result = NULL;
			}
			else if (set_message_correlation_id(result, correlation_id) != RESULT_OK)
			{
				LogError("Failed AMQP message correlation-id (%s)", twin_op_name);
				message_destroy(result);
				result = NULL;
			}
			else
			{
				BINARY_DATA binary_data;

				if (data != NULL)
				{
					const CONSTBUFFER* data_buffer;
					data_buffer = CONSTBUFFER_GetContent(data);
					binary_data.bytes = data_buffer->buffer;
					binary_data.length = data_buffer->size;
				}
				else
				{
					binary_data.bytes = EMPTY_TWIN_BODY_DATA;
					binary_data.length = EMPTY_TWIN_BODY_SIZE;
				}

				if (message_add_body_amqp_data(result, binary_data) != 0)
				{
					LogError("Failed adding twin patch data to AMQP message body");
					message_destroy(result);
					result = NULL;
				}
			}

			amqpvalue_destroy(msg_annotations_map);
		}
	}

	return result;
}

static void on_amqp_send_complete_callback(AMQP_MESSENGER_SEND_RESULT result, void* context)
{
	if (context == NULL)
	{
		LogError("Invalid argument (context is NULL)");
	}
	else if (result != AMQP_MESSENGER_SEND_RESULT_OK)
	{
		TWIN_OPERATION_CONTEXT* twin_op_ctx = (TWIN_OPERATION_CONTEXT*)context;

		(void)twin_op_ctx;

		//if (twin_op_ctx->type == TWIN_OPERATION_TYPE_PATCH)
		//{
		//	if (twin_op_ctx->on_report_state_complete_callback != NULL)
		//	{
		//		twin_op_ctx->on_report_state_complete_callback(TWIN_REPORT_STATE_RESULT_ERROR, 0, (void*)twin_op_ctx->on_report_state_complete_context);
		//	}

		//	free(twin_op_ctx);
		//}
	}
}

static int send_twin_operation_request(TWIN_MESSENGER_INSTANCE* twin_msgr, TWIN_OPERATION_CONTEXT* op_ctx, CONSTBUFFER_HANDLE data)
{
	int result;
	MESSAGE_HANDLE amqp_message;

	if ((amqp_message = create_amqp_message_for_twin_operation(op_ctx->type, op_ctx->correlation_id, data)) == NULL)
	{
		LogError("Failed creating request message for %s", ENUM_TO_STRING(TWIN_OPERATION_TYPE, op_ctx->type));
		result = __FAILURE__;
	}
	else
	{
		if (amqp_messenger_send_async(twin_msgr->amqp_msgr, amqp_message, on_amqp_send_complete_callback, (void*)op_ctx) != 0)
		{
			LogError("Failed sending request message for %s", ENUM_TO_STRING(TWIN_OPERATION_TYPE, op_ctx->type));
			result = __FAILURE__;
		}
		else
		{
			result = RESULT_OK;
		}

		message_destroy(amqp_message);
	}

	return result;
}


//---------- internal_ Helpers----------//

static void update_state(TWIN_MESSENGER_INSTANCE* twin_msgr, TWIN_MESSENGER_STATE new_state)
{
	if (new_state != twin_msgr->state)
	{
		TWIN_MESSENGER_STATE previous_state = twin_msgr->state;
		twin_msgr->state = new_state;

		if (twin_msgr->on_state_changed_callback != NULL)
		{
			twin_msgr->on_state_changed_callback(twin_msgr->on_message_received_context, previous_state, new_state);
		}
	}
}


static void process_twin_subscription(TWIN_MESSENGER_INSTANCE* twin_msgr)
{
	if (twin_msgr->subscription_state == TWIN_SUBSCRIPTION_STATE_GET_COMPLETE_PROPERTIES)
	{
		TWIN_OPERATION_CONTEXT* twin_op_ctx;

		if ((twin_op_ctx = create_twin_operation_context(twin_msgr, TWIN_OPERATION_TYPE_GET)) == NULL)
		{
			LogError("Failed creating a context for requesting complete TWIN desired properties");
		}
		else
		{
			// Add context to some list to control the traffic.

			if (send_twin_operation_request(twin_msgr, twin_op_ctx, NULL) != RESULT_OK)
			{
				LogError("Failed sending request for complete TWIN desired properties");
				destroy_twin_operation_context(twin_op_ctx);
			}
			else
			{
				twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_SUBSCRIBE_FOR_UPDATES;
			}
		}
	}
	else if (twin_msgr->subscription_state == TWIN_SUBSCRIPTION_STATE_SUBSCRIBE_FOR_UPDATES)
	{
		TWIN_OPERATION_CONTEXT* twin_op_ctx;

		if ((twin_op_ctx = create_twin_operation_context(twin_msgr, TWIN_OPERATION_TYPE_PUT)) == NULL)
		{
			LogError("Failed creating a context for requesting TWIN desired properties updates");
		}
		else
		{
			// Add context to some list to control the traffic.

			if (send_twin_operation_request(twin_msgr, twin_op_ctx, NULL) != RESULT_OK)
			{
				LogError("Failed sending request for TWIN desired properties updates");
				destroy_twin_operation_context(twin_op_ctx);
			}
			else
			{
				twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_SUBSCRIBED;
			}
		}
	}
	else if (twin_msgr->subscription_state == TWIN_SUBSCRIPTION_STATE_UNSUBSCRIBE)
	{
		TWIN_OPERATION_CONTEXT* twin_op_ctx;

		if ((twin_op_ctx = create_twin_operation_context(twin_msgr, TWIN_OPERATION_TYPE_DELETE)) == NULL)
		{
			LogError("Failed creating a context for stopping TWIN desired properties updates");
		}
		else
		{
			// Add context to some list to control the traffic.

			if (send_twin_operation_request(twin_msgr, twin_op_ctx, NULL) != RESULT_OK)
			{
				LogError("Failed sending request for stopping TWIN desired properties updates");
				destroy_twin_operation_context(twin_op_ctx);
			}
			else
			{
				twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_NOT_SUBSCRIBED;
			}
		}
	}
}

static void internal_twin_messenger_destroy(TWIN_MESSENGER_INSTANCE* twin_msgr)
{
	if (twin_msgr->amqp_msgr != NULL)
	{
		amqp_messenger_destroy(twin_msgr->amqp_msgr);
	}

	if (twin_msgr->device_id != NULL)
	{
		free(twin_msgr->device_id);
	}

	if (twin_msgr->iothub_host_fqdn != NULL)
	{
		free(twin_msgr->iothub_host_fqdn);
	}

	free(twin_msgr);
}


//---------- OptionHandler Functions ----------//

static void* OptionHandler_clone_option(const char* name, const void* value)
{
	(void)name;
	(void)value;

	return (void*)value;
}

static void OptionHandler_destroy_option(const char* name, const void* value)
{
	(void)name;
	(void)value;
}

static int OptionHandler_set_option(void* handle, const char* name, const void* value)
{
	(void)handle;
	(void)name;
	(void)value;

	int result;

	result = __FAILURE__;

	return result;
}


//---------- Internal Callbacks ----------//

static AMQP_MESSENGER_DISPOSITION_RESULT on_amqp_message_received(MESSAGE_HANDLE message, AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info, void* context)
{
	(void)context;
	// TODO: continue from here.

	// TODO: remove these
	PROPERTIES_HANDLE properties;
	if (message_get_properties(message, &properties) != 0 && properties != NULL)
	{
		AMQP_VALUE correlation_id;

		if (properties_get_correlation_id(properties, &correlation_id) != 0 && correlation_id != NULL)
		{
			char* value = NULL;

			if (amqpvalue_get_string(correlation_id, &value) != 0 && value != NULL)
			{
				LogInfo("Received message: %s", value);
			}
		}

		properties_destroy(properties);
	}

	MESSAGE_BODY_TYPE body_type;
	if (message_get_body_type(message, &body_type) == 0)
	{
		LogInfo("Body Type: %d", body_type);

		size_t body_count;
		if (message_get_body_amqp_data_count(message, &body_count) == 0)
		{
			LogInfo("Body count: %d", body_count);

			if (body_count > 0)
			{
				BINARY_DATA body;
				if (message_get_body_amqp_data_in_place(message, 0, &body) == 0)
				{
					LogInfo("Body: [%s]", body.bytes);
				}
			}
		}
	}


	amqp_messenger_destroy_disposition_info(disposition_info);
	return AMQP_MESSENGER_DISPOSITION_RESULT_ACCEPTED;
}

static TWIN_MESSENGER_STATE get_twin_state_from(AMQP_MESSENGER_STATE amqp_messenger_state)
{
	TWIN_MESSENGER_STATE result;

	switch (amqp_messenger_state)
	{
		case AMQP_MESSENGER_STATE_STOPPED:
			result = TWIN_MESSENGER_STATE_STOPPED;
			break;
		case AMQP_MESSENGER_STATE_STOPPING:
			result = TWIN_MESSENGER_STATE_STOPPING;
			break;
		case AMQP_MESSENGER_STATE_STARTED:
			result = TWIN_MESSENGER_STATE_STARTED;
			break;
		case AMQP_MESSENGER_STATE_STARTING:
			result = TWIN_MESSENGER_STATE_STARTING;
			break;
		case AMQP_MESSENGER_STATE_ERROR:
		default:
			result = TWIN_MESSENGER_STATE_ERROR;
			break;
	};

	return result;
}

static void on_amqp_messenger_state_changed_callback(void* context, AMQP_MESSENGER_STATE previous_state, AMQP_MESSENGER_STATE new_state)
{
	if (context == NULL)
	{
		LogError("Invalid argument (context is NULL)");
	}
	else if (new_state != previous_state)
	{
		TWIN_MESSENGER_STATE twin_new_state = get_twin_state_from(new_state);

		update_state((TWIN_MESSENGER_INSTANCE*)context, twin_new_state);
	}
}


//---------- Public APIs ----------//

TWIN_MESSENGER_HANDLE twin_messenger_create(const TWIN_MESSENGER_CONFIG* messenger_config)
{
	TWIN_MESSENGER_INSTANCE* twin_msgr;

	if (messenger_config == NULL)
	{
		LogError("invalid argument (messenger_config is NULL)");
		twin_msgr = NULL;
	}
	else if (messenger_config->device_id == NULL || messenger_config->iothub_host_fqdn == NULL || messenger_config->client_version == NULL)
	{
		LogError("invalid argument (device_id=%p, iothub_host_fqdn=%p, client_version=%p)", 
			messenger_config->device_id, messenger_config->iothub_host_fqdn, messenger_config->client_version);
		twin_msgr = NULL;
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_006: [TWIN_MESSENGER_create() shall allocate memory for the messenger instance structure (aka `instance`)]
		if ((twin_msgr = (TWIN_MESSENGER_INSTANCE*)malloc(sizeof(TWIN_MESSENGER_INSTANCE))) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_007: [If malloc() fails, TWIN_MESSENGER_create() shall fail and return NULL]
			LogError("failed allocating TWIN_MESSENGER_INSTANCE");
		}
		else
		{
			MAP_HANDLE link_attach_properties;

			memset(twin_msgr, 0, sizeof(TWIN_MESSENGER_INSTANCE));
			twin_msgr->state = TWIN_MESSENGER_STATE_STOPPED;
			twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_NOT_SUBSCRIBED;

			if (mallocAndStrcpy_s(&(char*)twin_msgr->client_version, messenger_config->client_version) != 0)
			{
				LogError("failed copying client_version");
				internal_twin_messenger_destroy(twin_msgr);
				twin_msgr = NULL;
			}
			// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_008: [TWIN_MESSENGER_create() shall save a copy of `messenger_config->device_id` into `twin_msgr->device_id`]
			else if (mallocAndStrcpy_s(&twin_msgr->device_id, messenger_config->device_id) != 0)
			{
				// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_009: [If STRING_construct() fails, TWIN_MESSENGER_create() shall fail and return NULL]
				LogError("failed copying device_id");
				internal_twin_messenger_destroy(twin_msgr);
				twin_msgr = NULL;
			}
			// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_010: [TWIN_MESSENGER_create() shall save a copy of `messenger_config->iothub_host_fqdn` into `twin_msgr->iothub_host_fqdn`]
			else if (mallocAndStrcpy_s(&twin_msgr->iothub_host_fqdn, messenger_config->iothub_host_fqdn) != 0)
			{
				// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_011: [If STRING_construct() fails, TWIN_MESSENGER_create() shall fail and return NULL]
				LogError("failed copying iothub_host_fqdn");
				internal_twin_messenger_destroy(twin_msgr);
				twin_msgr = NULL;
			}
			else if ((link_attach_properties = create_link_attach_properties(twin_msgr)) == NULL)
			{
				LogError("failed creating link attach properties");
				internal_twin_messenger_destroy(twin_msgr);
				twin_msgr = NULL;
			}
			else
			{
				AMQP_MESSENGER_CONFIG amqp_msgr_config;
				amqp_msgr_config.client_version = twin_msgr->client_version;
				amqp_msgr_config.device_id = twin_msgr->device_id;
				amqp_msgr_config.iothub_host_fqdn = twin_msgr->iothub_host_fqdn;
				amqp_msgr_config.send_link.target_suffix = DEFAULT_TWIN_SEND_LINK_SOURCE_NAME;
				amqp_msgr_config.send_link.attach_properties = link_attach_properties;
				amqp_msgr_config.receive_link.source_suffix = DEFAULT_TWIN_RECEIVE_LINK_TARGET_NAME;
				amqp_msgr_config.receive_link.attach_properties = link_attach_properties;
				amqp_msgr_config.on_state_changed_callback = on_amqp_messenger_state_changed_callback;
				amqp_msgr_config.on_state_changed_context = (void*)twin_msgr;

				if ((twin_msgr->amqp_msgr = amqp_messenger_create(&amqp_msgr_config)) == NULL)
				{
					LogError("failed creating the AMQP messenger");
					internal_twin_messenger_destroy(twin_msgr);
					twin_msgr = NULL;
				}
				else
				{
					// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_013: [`messenger_config->on_state_changed_callback` shall be saved into `twin_msgr->on_state_changed_callback`]
					twin_msgr->on_state_changed_callback = messenger_config->on_state_changed_callback;

					// Codes_SRS_IOTHUBTRANSPORT_TWIN_MESSENGER_09_014: [`messenger_config->on_state_changed_context` shall be saved into `twin_msgr->on_state_changed_context`]
					twin_msgr->on_state_changed_context = messenger_config->on_state_changed_context;
				}

				destroy_link_attach_properties(link_attach_properties);
			}
		}
	}

	return (TWIN_MESSENGER_HANDLE)twin_msgr;
}

int twin_messenger_report_state_async(TWIN_MESSENGER_HANDLE twin_msgr_handle, CONSTBUFFER_HANDLE data, TWIN_MESSENGER_REPORT_STATE_COMPLETE_CALLBACK on_report_state_complete_callback, const void* context)
{
	int result;

	if (twin_msgr_handle == NULL || data == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle=%p, data=%p)", twin_msgr_handle, data);
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;
		TWIN_OPERATION_CONTEXT* twin_op_ctx;

		if ((twin_op_ctx = create_twin_operation_context(twin_msgr, TWIN_OPERATION_TYPE_PATCH)) == NULL)
		{
			LogError("Failed creating context for sending reported state");
			result = __FAILURE__;
		}
		else
		{
			twin_op_ctx->on_report_state_complete_callback = on_report_state_complete_callback;
			twin_op_ctx->on_report_state_complete_context = context;

			if (send_twin_operation_request(twin_msgr, twin_op_ctx, data) != RESULT_OK)
			{
				LogError("Failed sending reported state");
				destroy_twin_operation_context(twin_op_ctx);
				result = __FAILURE__;
			}
			else
			{
				result = RESULT_OK;
			}
		}
	}

	return result;
}

int twin_messenger_subscribe(TWIN_MESSENGER_HANDLE twin_msgr_handle, TWIN_STATE_UPDATE_CALLBACK on_twin_state_update_callback, void* context)
{
	int result;

	if (twin_msgr_handle == NULL || on_twin_state_update_callback == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle=%p, on_twin_state_update_callback=%p)", twin_msgr_handle, on_twin_state_update_callback);
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;

		if (twin_msgr->subscription_state != TWIN_SUBSCRIPTION_STATE_NOT_SUBSCRIBED)
		{
			result = RESULT_OK;
		}
		else
		{
			twin_msgr->on_message_received_callback = on_twin_state_update_callback;
			twin_msgr->on_message_received_context = context;

			if (amqp_messenger_subscribe_for_messages(twin_msgr->amqp_msgr, on_amqp_message_received, (void*)twin_msgr) != 0)
			{
				LogError("Failed subscribing for TWIN updates");
				twin_msgr->on_message_received_callback = NULL;
				twin_msgr->on_message_received_context = NULL;
				result = __FAILURE__;
			}
			else
			{
				twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_GET_COMPLETE_PROPERTIES;
				result = RESULT_OK;
			}
		}
	}

	return result;
}

int twin_messenger_unsubscribe(TWIN_MESSENGER_HANDLE twin_msgr_handle)
{
	int result;

	if (twin_msgr_handle == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle is NULL)");
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;

		if (amqp_messenger_unsubscribe_for_messages(twin_msgr->amqp_msgr) != 0)
		{
			LogError("Failed unsubscribing for TWIN updates");
			result = __FAILURE__;
		}
		else
		{
			twin_msgr->subscription_state = TWIN_SUBSCRIPTION_STATE_UNSUBSCRIBE;
			twin_msgr->on_message_received_callback = NULL;
			twin_msgr->on_message_received_context = NULL;
			result = RESULT_OK;
		}
	}

	return result;
}

int twin_messenger_get_send_status(TWIN_MESSENGER_HANDLE twin_msgr_handle, TWIN_MESSENGER_SEND_STATUS* send_status)
{
	int result;

	if (twin_msgr_handle == NULL || send_status == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle=%p, send_status=%p)", twin_msgr_handle, send_status);
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;
		AMQP_MESSENGER_SEND_STATUS amqp_send_status;

		if (amqp_messenger_get_send_status(twin_msgr->amqp_msgr, &amqp_send_status) != 0)
		{
			LogError("Failed getting the send status of the AMQP messenger");
			result = __FAILURE__;
		}
		else
		{
			*send_status = (AMQP_MESSENGER_SEND_STATUS_BUSY ? TWIN_MESSENGER_SEND_STATUS_BUSY : TWIN_MESSENGER_SEND_STATUS_IDLE);
			result = RESULT_OK;
		}
	}

	return result;
}

int twin_messenger_start(TWIN_MESSENGER_HANDLE twin_msgr_handle, SESSION_HANDLE session_handle)
{
	int result;

	if (twin_msgr_handle == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle is NULL)");
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;

		if (amqp_messenger_start(twin_msgr->amqp_msgr, session_handle) != 0)
		{
			LogError("Failed starting the AMQP messenger");
			result = __FAILURE__;
		}
		else
		{
			result = RESULT_OK;
		}
	}

	return result;
}

int twin_messenger_stop(TWIN_MESSENGER_HANDLE twin_msgr_handle)
{
	int result;

	if (twin_msgr_handle == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle is NULL)");
		result = __FAILURE__;
	}
	else
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;

		if (amqp_messenger_stop(twin_msgr->amqp_msgr) != 0)
		{
			LogError("Failed stopping the AMQP messenger");
			result = __FAILURE__;
		}
		else
		{
			result = RESULT_OK;
		}
	}

	return result;
}

void twin_messenger_do_work(TWIN_MESSENGER_HANDLE twin_msgr_handle)
{
	if (twin_msgr_handle != NULL)
	{
		TWIN_MESSENGER_INSTANCE* twin_msgr = (TWIN_MESSENGER_INSTANCE*)twin_msgr_handle;

		process_twin_subscription(twin_msgr);

		amqp_messenger_do_work(twin_msgr->amqp_msgr);
	}
}

void twin_messenger_destroy(TWIN_MESSENGER_HANDLE twin_msgr_handle)
{
	if (twin_msgr_handle == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle is NULL)");
	}
	else
	{
		internal_twin_messenger_destroy((TWIN_MESSENGER_INSTANCE*)twin_msgr_handle);
	}
}

int twin_messenger_set_option(TWIN_MESSENGER_HANDLE twin_msgr_handle, const char* name, void* value)
{
	int result;

	if (twin_msgr_handle == NULL || name == NULL || value == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle=%p, name=%p, value=%p)", twin_msgr_handle, name, value);
		result = __FAILURE__;
	}
	else
	{
		// TODO: fill it up
		result = RESULT_OK;
	}

	return result;
}

OPTIONHANDLER_HANDLE twin_messenger_retrieve_options(TWIN_MESSENGER_HANDLE twin_msgr_handle)
{
	OPTIONHANDLER_HANDLE result;

	if (twin_msgr_handle == NULL)
	{
		LogError("Invalid argument (twin_msgr_handle is NULL)");
		result = NULL;
	}
	else if ((result = OptionHandler_Create(OptionHandler_clone_option, OptionHandler_destroy_option, OptionHandler_set_option)) == NULL)
	{
		LogError("Failed creating OptionHandler");
	}
	else
	{
		// TODO: fill it up
	}

	return result;
}