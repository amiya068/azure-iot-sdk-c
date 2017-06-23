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
#include "azure_uamqp_c/link.h"
#include "azure_uamqp_c/messaging.h"
#include "azure_uamqp_c/message_sender.h"
#include "azure_uamqp_c/message_receiver.h"
#include "message_queue.h"
#include "iothub_client_retry_control.h"
#include "iothubtransport_amqp_messenger.h"

DEFINE_ENUM_STRINGS(AMQP_MESSENGER_SEND_STATUS, AMQP_MESSENGER_SEND_STATUS_VALUES);
DEFINE_ENUM_STRINGS(AMQP_MESSENGER_SEND_RESULT, AMQP_MESSENGER_SEND_RESULT_VALUES);
DEFINE_ENUM_STRINGS(AMQP_MESSENGER_REASON, AMQP_MESSENGER_REASON_VALUES);
DEFINE_ENUM_STRINGS(AMQP_MESSENGER_DISPOSITION_RESULT, AMQP_MESSENGER_DISPOSITION_RESULT_VALUES);
DEFINE_ENUM_STRINGS(AMQP_MESSENGER_STATE, AMQP_MESSENGER_STATE_VALUES);


#define RESULT_OK 0
#define INDEFINITE_TIME ((time_t)(-1))

// Base address format: "amqps://<iot hub fqdn>/devices/<device-id>/<suffix>"
#define LINK_ADDRESS_FORMAT                             "amqps://%s/devices/%s/%s"
#define SEND_LINK_NAME_PREFIX							"link-snd"
#define MESSAGE_SENDER_MAX_LINK_SIZE                    UINT64_MAX
#define RECEIVE_LINK_NAME_PREFIX		                "link-rcv"
#define MESSAGE_RECEIVER_MAX_LINK_SIZE                  65536
#define DEFAULT_EVENT_SEND_RETRY_LIMIT                  0
#define DEFAULT_EVENT_SEND_TIMEOUT_SECS                 600
#define DEFAULT_MAX_SEND_ERROR_COUNT                    10
#define MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS    300
#define MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS  300
#define UNIQUE_ID_BUFFER_SIZE                           37

static const char* MESSENGER_SAVED_MQ_OPTIONS = "amqp_message_queue_options";

typedef struct AMQP_MESSENGER_INSTANCE_TAG
{
	AMQP_MESSENGER_CONFIG* config;

	AMQP_MESSENGER_STATE_CHANGED_CALLBACK on_state_changed_callback;
	void* on_state_changed_context;

	bool receive_messages;
	ON_AMQP_MESSENGER_MESSAGE_RECEIVED on_message_received_callback;
	void* on_message_received_context;

	MESSAGE_QUEUE_HANDLE send_queue;
	AMQP_MESSENGER_STATE state;

	SESSION_HANDLE session_handle;
	
	LINK_HANDLE sender_link;
	MESSAGE_SENDER_HANDLE message_sender;
	MESSAGE_SENDER_STATE message_sender_current_state;
	MESSAGE_SENDER_STATE message_sender_previous_state;

	LINK_HANDLE receiver_link;
	MESSAGE_RECEIVER_HANDLE message_receiver;
	MESSAGE_RECEIVER_STATE message_receiver_current_state;
	MESSAGE_RECEIVER_STATE message_receiver_previous_state;

	size_t send_error_count;
	size_t max_send_error_count;

	time_t last_message_sender_state_change_time;
	time_t last_message_receiver_state_change_time;
} AMQP_MESSENGER_INSTANCE;

typedef struct MESSAGE_SEND_CONTEXT_TAG
{
	MESSAGE_HANDLE message;
	bool is_destroyed;

	AMQP_MESSENGER_INSTANCE* messenger;

	AMQP_MESSENGER_SEND_COMPLETE_CALLBACK on_send_complete_callback;
	void* user_context;

	PROCESS_MESSAGE_COMPLETED_CALLBACK on_process_message_completed_callback;
} MESSAGE_SEND_CONTEXT;



static MESSAGE_SEND_CONTEXT* create_message_send_context()
{
	MESSAGE_SEND_CONTEXT* result;

	if ((result = (MESSAGE_SEND_CONTEXT*)malloc(sizeof(MESSAGE_SEND_CONTEXT))) == NULL)
	{
		LogError("Failed creating the message send context");
	}
	else
	{
		memset(result, 0, sizeof(MESSAGE_SEND_CONTEXT));
	}

	return result;
}

static bool is_valid_configuration(const AMQP_MESSENGER_CONFIG* config)
{
	bool result;

	if (config == NULL)
	{
		LogError("Invalid configuration (NULL)");
		result = false;
	}
	else if (config->client_version == NULL ||
		config->device_id == NULL ||
		config->iothub_host_fqdn == NULL ||
		config->receive_link.source_suffix == NULL ||
		config->send_link.target_suffix == NULL)
	{
		LogError("Invalid configuration (client_version=%p, device_id=%p, iothub_host_fqdn=%p, receive_link (source_suffix=%p), send_link (target_suffix=%s))",
			config->client_version, config->device_id, config->iothub_host_fqdn,
			config->receive_link.source_suffix, config->send_link.target_suffix);
		result = false;
	}
	else
	{
		result = true;
	}

	return result;
}

static void destroy_link_configuration(AMQP_MESSENGER_LINK_CONFIG* link_config)
{
	if (link_config->target_suffix != NULL)
	{
		free((void*)link_config->target_suffix);
	}

	if (link_config->source_suffix != NULL)
	{
		free((void*)link_config->source_suffix);
	}

	if (link_config->attach_properties != NULL)
	{
		Map_Destroy(link_config->attach_properties);
	}
}

static void destroy_configuration(AMQP_MESSENGER_CONFIG* config)
{
	if (config != NULL)
	{
		if (config->client_version != NULL)
		{
			free((void*)config->client_version);
		}

		if (config->device_id != NULL)
		{
			free((void*)config->device_id);
		}

		if (config->iothub_host_fqdn != NULL)
		{
			free((void*)config->iothub_host_fqdn);
		}

		destroy_link_configuration(&config->send_link);
		destroy_link_configuration(&config->receive_link);

		free(config);
	}
}

static int clone_link_configuration(role link_role, AMQP_MESSENGER_LINK_CONFIG* dst_config, const AMQP_MESSENGER_LINK_CONFIG* src_config)
{
	int result;

	if (link_role == role_sender &&
		mallocAndStrcpy_s(&(char*)dst_config->target_suffix, src_config->target_suffix) != 0)
	{
		LogError("Failed copying send_link_target_suffix");
		result = __FAILURE__;
	}
	else if (link_role == role_receiver &&
		mallocAndStrcpy_s(&(char*)dst_config->source_suffix, src_config->source_suffix) != 0)
	{
		LogError("Failed copying receive_link_source_suffix");
		result = __FAILURE__;
	}
	else if (src_config->attach_properties != NULL &&
		(dst_config->attach_properties = Map_Clone(src_config->attach_properties)) == NULL)
	{
		LogError("Failed copying link attach properties");
		result = __FAILURE__;
	}
	else
	{
		dst_config->snd_settle_mode = sender_settle_mode_settled;
		dst_config->rcv_settle_mode = receiver_settle_mode_first;

		result = RESULT_OK;
	}

	return result;
}

static AMQP_MESSENGER_CONFIG* clone_configuration(const AMQP_MESSENGER_CONFIG* config)
{
	AMQP_MESSENGER_CONFIG* result;

	if ((result = (AMQP_MESSENGER_CONFIG*)malloc(sizeof(AMQP_MESSENGER_CONFIG))) == NULL)
	{
		LogError("Failed allocating AMQP_MESSENGER_CONFIG");
	}
	else
	{
		memset(result, 0, sizeof(AMQP_MESSENGER_CONFIG));

		if (mallocAndStrcpy_s(&(char*)result->client_version, config->client_version) != 0)
		{
			LogError("Failed copying device_id");
			destroy_configuration(result);
			result = NULL;
		}
		else if (mallocAndStrcpy_s(&(char*)result->device_id, config->device_id) != 0)
		{
			LogError("Failed copying device_id");
			destroy_configuration(result);
			result = NULL;
		}
		else if (mallocAndStrcpy_s(&(char*)result->iothub_host_fqdn, config->iothub_host_fqdn) != 0)
		{
			LogError("Failed copying iothub_host_fqdn)");
			destroy_configuration(result);
			result = NULL;
		}
		else if (clone_link_configuration(role_sender, &result->send_link, &config->send_link) != RESULT_OK)
		{
			LogError("Failed copying send link configuration)");
			destroy_configuration(result);
			result = NULL;
		}
		else if (clone_link_configuration(role_receiver, &result->receive_link, &config->receive_link) != RESULT_OK)
		{
			LogError("Failed copying receive link configuration)");
			destroy_configuration(result);
			result = NULL;
		}
	}

	return result;
}

static void destroy_message_send_context(MESSAGE_SEND_CONTEXT* context)
{
	free(context);
}

static STRING_HANDLE create_link_address(const char* host_fqdn, const char* device_id, const char* address_suffix)
{
	STRING_HANDLE link_address;

	if ((link_address = STRING_new()) == NULL)
	{
		LogError("failed creating link_address (STRING_new failed)");
	}
	else if (STRING_sprintf(link_address, LINK_ADDRESS_FORMAT, host_fqdn, device_id, address_suffix) != RESULT_OK)
	{
		LogError("Failed creating the link_address (STRING_sprintf failed)");
		STRING_delete(link_address);
		link_address = NULL;
	}

	return link_address;
}

static STRING_HANDLE create_link_source_name(STRING_HANDLE link_name)
{
	STRING_HANDLE source_name;
	
	if ((source_name = STRING_new()) == NULL)
	{
		LogError("Failed creating the source_name (STRING_new failed)");
	}
	else
	{
		const char* link_name_char_ptr = STRING_c_str(link_name);
		if (STRING_sprintf(source_name, "%s-source", link_name_char_ptr) != RESULT_OK)
		{
			STRING_delete(source_name);
			source_name = NULL;
			LogError("Failed creating the source_name (STRING_sprintf failed)");
		}
	}

	return source_name;
}

static STRING_HANDLE create_link_target_name(STRING_HANDLE link_name)
{
	STRING_HANDLE target_name;

	if ((target_name = STRING_new()) == NULL)
	{
		LogError("Failed creating the target_name (STRING_new failed)");
	}
	else
	{
		const char* link_name_char_ptr = STRING_c_str(link_name);
		if (STRING_sprintf(target_name, "%s-target", link_name_char_ptr) != RESULT_OK)
		{
			STRING_delete(target_name);
			target_name = NULL;
			LogError("Failed creating the target_name (STRING_sprintf failed)");
		}
	}

	return target_name;
}

static STRING_HANDLE create_link_name(role link_role, const char* device_id)
{
	char* unique_id;
	STRING_HANDLE result;

	if ((unique_id = (char*)malloc(sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1)) == NULL)
	{
		LogError("Failed generating an unique tag (malloc failed)");
		result = NULL;
	}
	else
	{
		memset(unique_id, 0, sizeof(char) * UNIQUE_ID_BUFFER_SIZE + 1);

		if (UniqueId_Generate(unique_id, UNIQUE_ID_BUFFER_SIZE) != UNIQUEID_OK)
		{
			LogError("Failed generating an unique tag (UniqueId_Generate failed)");
			result = NULL;
		}
		else if ((result = STRING_new()) == NULL)
		{
			LogError("Failed generating an unique tag (STRING_new failed)");
		}
		else
		{
			char* prefix = (link_role == role_sender ? SEND_LINK_NAME_PREFIX : RECEIVE_LINK_NAME_PREFIX);

			if (STRING_sprintf(result, "%s-%s-%s", prefix, device_id, unique_id) != RESULT_OK)
			{
				LogError("Failed generating an unique tag (STRING_sprintf failed)");
				STRING_delete(result);
				result = NULL;
			}
		}

		free(unique_id);
	}

	return result;
}

static void update_messenger_state(AMQP_MESSENGER_INSTANCE* instance, AMQP_MESSENGER_STATE new_state)
{
	if (new_state != instance->state)
	{
		AMQP_MESSENGER_STATE previous_state = instance->state;
		instance->state = new_state;

		if (instance->on_state_changed_callback != NULL)
		{
			instance->on_state_changed_callback(instance->on_state_changed_context, previous_state, new_state);
		}
	}
}

static int add_link_attach_properties(LINK_HANDLE link, MAP_HANDLE user_defined_properties)
{
	int result;
	fields attach_properties;

	if ((attach_properties = amqpvalue_create_map()) == NULL)
	{
		LogError("Failed to create the map for attach properties.");
		result = __FAILURE__;
	}
	else
	{
		const char* const* keys;
		const char* const* values;
		size_t count;

		if (Map_GetInternals(user_defined_properties, &keys, &values, &count) != MAP_OK)
		{
			LogError("failed getting user defined properties details.");
			result = __FAILURE__;
		}
		else
		{
			size_t i;
			result = RESULT_OK;

			for (i = 0; i < count && result == RESULT_OK; i++)
			{
				AMQP_VALUE key;
				AMQP_VALUE value;

				if ((key = amqpvalue_create_symbol(keys[i])) == NULL)
				{
					LogError("Failed creating AMQP_VALUE For key %s.", keys[i]);
					result = __FAILURE__;
				}
				else
				{
					if ((value = amqpvalue_create_string(values[i])) == NULL)
					{
						LogError("Failed creating AMQP_VALUE For key %s value", keys[i]);
						result = __FAILURE__;
					}
					else
					{
						if ((result = amqpvalue_set_map_value(attach_properties, key, value)) != 0)
						{
							LogError("Failed adding property %s to map (%d)", keys[i], result);
							result = __FAILURE__;
						}

						amqpvalue_destroy(value);
					}

					amqpvalue_destroy(key);
				}
			}

			if (result == RESULT_OK)
			{
				if ((result = link_set_attach_properties(link, attach_properties)) != 0)
				{
					LogError("Failed attaching properties to link (%d)", result);
					result = __FAILURE__;
				}
				else
				{
					result = RESULT_OK;
				}
			}
		}

		amqpvalue_destroy(attach_properties);
	}

	return result;
}

static LINK_HANDLE create_link(role link_role, SESSION_HANDLE session_handle, AMQP_MESSENGER_LINK_CONFIG* link_config, const char* iothub_host_fqdn, const char* device_id)
{
	LINK_HANDLE result = NULL;
	STRING_HANDLE link_address;

	if ((link_address = create_link_address(iothub_host_fqdn, device_id, (link_role == role_sender ? link_config->target_suffix : link_config->source_suffix))) == NULL)
	{
		LogError("Failed creating the message sender (failed creating the 'link_address')");
		result = NULL;
	}
	else
	{
		STRING_HANDLE link_name;

		if ((link_name = create_link_name(link_role, device_id)) == NULL)
		{
			LogError("Failed creating the link name");
			result = NULL;
		}
		else
		{
			AMQP_VALUE source = NULL;
			AMQP_VALUE target = NULL;

			if (link_role == role_sender)
			{
				STRING_HANDLE source_name;

				if ((source_name = create_link_source_name(link_name)) != NULL)
				{
					if ((source = messaging_create_source(STRING_c_str(source_name))) != NULL)
					{
						if ((target = messaging_create_target(STRING_c_str(link_address))) == NULL)
						{
							amqpvalue_destroy(source);
							source = NULL;
						}
					}

					STRING_delete(source_name);
				}
			}
			else
			{
				STRING_HANDLE target_name;

				if ((target_name = create_link_target_name(link_name)) != NULL)
				{
					if ((source = messaging_create_source(STRING_c_str(link_address))) != NULL)
					{
						if ((target = messaging_create_target(STRING_c_str(target_name))) == NULL)
						{
							amqpvalue_destroy(source);
							source = NULL;
						}
					}

					STRING_delete(target_name);
				}
			}

			if (source != NULL && target != NULL)
			{
				if ((result = link_create(session_handle, STRING_c_str(link_name), link_role, source, target)) == NULL)
				{
					LogError("Failed creating the AMQP link");
				}
				else if (link_config->attach_properties != NULL &&
					add_link_attach_properties(result, link_config->attach_properties) != RESULT_OK)
				{
					LogError("Failed setting link attach properties");
					link_destroy(result);
					result = NULL;
				}
				
				if (link_set_max_message_size(result, MESSAGE_SENDER_MAX_LINK_SIZE) != RESULT_OK)
				{
					LogError("Failed setting link max message size.");
				}

				amqpvalue_destroy(source);
				amqpvalue_destroy(target);
			}

			STRING_delete(link_name);
		}

		STRING_delete(link_address);
	}

	return result;
}

static void destroy_message_sender(AMQP_MESSENGER_INSTANCE* instance)
{
	if (instance->message_sender != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_060: [`instance->message_sender` shall be destroyed using messagesender_destroy()]
		messagesender_destroy(instance->message_sender);
		instance->message_sender = NULL;
		instance->message_sender_current_state = MESSAGE_SENDER_STATE_IDLE;
		instance->message_sender_previous_state = MESSAGE_SENDER_STATE_IDLE;
		instance->last_message_sender_state_change_time = INDEFINITE_TIME;
	}

	if (instance->sender_link != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_063: [`instance->sender_link` shall be destroyed using link_destroy()]
		link_destroy(instance->sender_link);
		instance->sender_link = NULL;
	}
}

static void on_message_sender_state_changed_callback(void* context, MESSAGE_SENDER_STATE new_state, MESSAGE_SENDER_STATE previous_state)
{
	if (context == NULL)
	{
		LogError("on_message_sender_state_changed_callback was invoked with a NULL context; although unexpected, this failure will be ignored");
	}
	else if (new_state != previous_state)
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)context;
		instance->message_sender_current_state = new_state;
		instance->message_sender_previous_state = previous_state;
		instance->last_message_sender_state_change_time = get_time(NULL);
	}
}

static int create_message_sender(AMQP_MESSENGER_INSTANCE* instance)
{
	int result;

	if ((instance->sender_link = create_link(role_sender, 
		instance->session_handle, &instance->config->send_link, instance->config->iothub_host_fqdn, instance->config->device_id)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_044: [If link_create() fails, amqp_messenger_do_work() shall fail and return]
		LogError("Failed creating the message sender link");
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_051: [`instance->message_sender` shall be created using messagesender_create(), passing the `instance->sender_link` and `on_message_sender_state_changed_callback`]
	else if ((instance->message_sender = messagesender_create(instance->sender_link, on_message_sender_state_changed_callback, (void*)instance)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_052: [If messagesender_create() fails, amqp_messenger_do_work() shall fail and return]
		LogError("Failed creating the message sender (messagesender_create failed)");
		link_destroy(instance->sender_link);
		instance->sender_link = NULL;
		result = __FAILURE__;
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_053: [`instance->message_sender` shall be opened using messagesender_open()]
		if (messagesender_open(instance->message_sender) != RESULT_OK)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_054: [If messagesender_open() fails, amqp_messenger_do_work() shall fail and return]
			LogError("Failed opening the AMQP message sender.");
			messagesender_destroy(instance->message_sender);
			instance->message_sender = NULL;
			link_destroy(instance->sender_link);
			instance->sender_link = NULL;
			result = __FAILURE__;
		}
		else
		{
			result = RESULT_OK;
		}
	}

	return result;
}

static void destroy_message_receiver(AMQP_MESSENGER_INSTANCE* instance)
{
	if (instance->message_receiver != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_061: [`instance->message_receiver` shall be closed using messagereceiver_close()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_093: [`instance->message_receiver` shall be closed using messagereceiver_close()]
		if (messagereceiver_close(instance->message_receiver) != RESULT_OK)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_094: [If messagereceiver_close() fails, it shall be logged and ignored]
			LogError("Failed closing the AMQP message receiver (this failure will be ignored).");
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_062: [`instance->message_receiver` shall be destroyed using messagereceiver_destroy()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_095: [`instance->message_receiver` shall be destroyed using messagereceiver_destroy()]
		messagereceiver_destroy(instance->message_receiver);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_096: [`instance->message_receiver` shall be set to NULL]
		instance->message_receiver = NULL;
		instance->message_receiver_current_state = MESSAGE_RECEIVER_STATE_IDLE;
		instance->message_receiver_previous_state = MESSAGE_RECEIVER_STATE_IDLE;
		instance->last_message_receiver_state_change_time = INDEFINITE_TIME;
	}

	if (instance->receiver_link != NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_064: [`instance->receiver_link` shall be destroyed using link_destroy()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_097: [`instance->receiver_link` shall be destroyed using link_destroy()]
		link_destroy(instance->receiver_link);
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_098: [`instance->receiver_link` shall be set to NULL]
		instance->receiver_link = NULL;
	}
}

static void on_message_receiver_state_changed_callback(const void* context, MESSAGE_RECEIVER_STATE new_state, MESSAGE_RECEIVER_STATE previous_state)
{
	if (context == NULL)
	{
		LogError("on_message_receiver_state_changed_callback was invoked with a NULL context; although unexpected, this failure will be ignored");
	}
	else
	{
		if (new_state != previous_state)
		{
			AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)context;
			instance->message_receiver_current_state = new_state;
			instance->message_receiver_previous_state = previous_state;
			instance->last_message_receiver_state_change_time = get_time(NULL);
		}
	}
}

static AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* create_message_disposition_info(AMQP_MESSENGER_INSTANCE* messenger)
{
	AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* result;

	if ((result = (AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO*)malloc(sizeof(AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO))) == NULL)
	{
		LogError("Failed creating AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO container (malloc failed)");
		result = NULL;
	}
	else
	{
		delivery_number message_id;

		if (messagereceiver_get_received_message_id(messenger->message_receiver, &message_id) != RESULT_OK)
		{
			LogError("Failed creating AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO container (messagereceiver_get_received_message_id failed)");
			free(result);
			result = NULL;
		}
		else
		{
			const char* link_name;

			if (messagereceiver_get_link_name(messenger->message_receiver, &link_name) != RESULT_OK)
			{
				LogError("Failed creating AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO container (messagereceiver_get_link_name failed)");
				free(result);
				result = NULL;
			}
			else if (mallocAndStrcpy_s(&result->source, link_name) != RESULT_OK)
			{
				LogError("Failed creating AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO container (failed copying link name)");
				free(result);
				result = NULL;
			}
			else
			{
				result->message_id = message_id;
			}
		}
	}

	return result;
}

static void destroy_message_disposition_info(AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info)
{
	free(disposition_info->source);
	free(disposition_info);
}

static AMQP_VALUE create_uamqp_disposition_result_from(AMQP_MESSENGER_DISPOSITION_RESULT disposition_result)
{
	AMQP_VALUE uamqp_disposition_result;

	if (disposition_result == AMQP_MESSENGER_DISPOSITION_RESULT_NONE)
	{
		uamqp_disposition_result = NULL; // intentionally not sending an answer.
	}
	else if (disposition_result == AMQP_MESSENGER_DISPOSITION_RESULT_ACCEPTED)
	{
		uamqp_disposition_result = messaging_delivery_accepted();
	}
	else if (disposition_result == AMQP_MESSENGER_DISPOSITION_RESULT_RELEASED)
	{
		uamqp_disposition_result = messaging_delivery_released();
	}
	else if (disposition_result == AMQP_MESSENGER_DISPOSITION_RESULT_REJECTED)
	{
		uamqp_disposition_result = messaging_delivery_rejected("Rejected by application", "Rejected by application");
	}
	else
	{
		LogError("Failed creating a disposition result for messagereceiver (result %d is not supported)", disposition_result);
		uamqp_disposition_result = NULL;
	}

	return uamqp_disposition_result;
}

static AMQP_VALUE on_message_received_internal_callback(const void* context, MESSAGE_HANDLE message)
{
	AMQP_VALUE result;
	AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)context;
	AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* message_disposition_info;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_186: [A AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO instance shall be created containing the source link name and message delivery ID]
	if ((message_disposition_info = create_message_disposition_info(instance)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_187: [**If the AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO instance fails to be created, on_message_received_internal_callback shall return messaging_delivery_released()]
		LogError("on_message_received_internal_callback failed (failed creating AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO).");
		result = messaging_delivery_released();
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_123: [`instance->on_message_received_callback` shall be invoked passing the IOTHUB_MESSAGE_HANDLE and AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO instance]
		AMQP_MESSENGER_DISPOSITION_RESULT disposition_result = instance->on_message_received_callback(message, message_disposition_info, instance->on_message_received_context);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_125: [If `instance->on_message_received_callback` returns AMQP_MESSENGER_DISPOSITION_RESULT_ACCEPTED, on_message_received_internal_callback shall return the result of messaging_delivery_accepted()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_126: [If `instance->on_message_received_callback` returns AMQP_MESSENGER_DISPOSITION_RESULT_RELEASED, on_message_received_internal_callback shall return the result of messaging_delivery_released()]
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_127: [If `instance->on_message_received_callback` returns AMQP_MESSENGER_DISPOSITION_RESULT_REJECTED, on_message_received_internal_callback shall return the result of messaging_delivery_rejected()]
		result = create_uamqp_disposition_result_from(disposition_result);
	}

	return result;
}

static int create_message_receiver(AMQP_MESSENGER_INSTANCE* instance)
{
	int result;

	if ((instance->receiver_link = create_link(role_receiver,
		instance->session_handle, &instance->config->receive_link, instance->config->iothub_host_fqdn, instance->config->device_id)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_044: [If link_create() fails, amqp_messenger_do_work() shall fail and return]
		LogError("Failed creating the message receiver link");
		result = __FAILURE__;
	}
	else if ((instance->message_receiver = messagereceiver_create(instance->receiver_link, on_message_receiver_state_changed_callback, (void*)instance)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_087: [If messagereceiver_create() fails, amqp_messenger_do_work() shall fail and return]
		LogError("Failed creating the message receiver (messagereceiver_create failed)");
		link_destroy(instance->receiver_link);
		instance->receiver_link = NULL;
		result = __FAILURE__;
	}
	else if (messagereceiver_open(instance->message_receiver, on_message_received_internal_callback, (void*)instance) != RESULT_OK)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_089: [If messagereceiver_open() fails, amqp_messenger_do_work() shall fail and return]
		LogError("Failed opening the AMQP message receiver.");
		messagereceiver_destroy(instance->message_receiver);
		instance->message_receiver = NULL;
		link_destroy(instance->receiver_link);
		instance->receiver_link = NULL;
		result = __FAILURE__;
	}
	else
	{
		result = RESULT_OK;
	}

	return result;
}

static void on_send_complete_callback(void* context, MESSAGE_SEND_RESULT send_result)
{ 
	if (context != NULL)
	{
		MESSAGE_QUEUE_RESULT mq_result;
		MESSAGE_SEND_CONTEXT* msg_ctx = (MESSAGE_SEND_CONTEXT*)context;

		if (send_result == MESSAGE_SEND_OK)
		{
			mq_result = MESSAGE_QUEUE_SUCCESS;
		}
		else
		{
			mq_result = MESSAGE_QUEUE_ERROR;
		}

		msg_ctx->on_process_message_completed_callback(msg_ctx->messenger->send_queue, (MQ_MESSAGE_HANDLE)msg_ctx->message, mq_result, NULL);
	}
}

static void on_process_message_callback(MESSAGE_QUEUE_HANDLE message_queue, MQ_MESSAGE_HANDLE message, PROCESS_MESSAGE_COMPLETED_CALLBACK on_process_message_completed_callback, void* context)
{
	if (message_queue == NULL || message == NULL || on_process_message_completed_callback == NULL || context == NULL)
	{
		LogError("invalid argument (message_queue=%p, message=%p, on_process_message_completed_callback=%p, context=%p)", message_queue, message, on_process_message_completed_callback, context);
	}
	else
	{
		MESSAGE_SEND_CONTEXT* message_context = (MESSAGE_SEND_CONTEXT*)context;
		message_context->on_process_message_completed_callback = on_process_message_completed_callback;

		if (messagesender_send(message_context->messenger->message_sender, (MESSAGE_HANDLE)message, on_send_complete_callback, context) != 0)
		{
			LogError("Failed sending AMQP message");
			on_process_message_completed_callback(message_queue, message, MESSAGE_QUEUE_ERROR, NULL);
		}

		message_destroy((MESSAGE_HANDLE)message);
		message_context->is_destroyed = true;
	}
}



// ---------- Set/Retrieve Options Helpers ----------//

static void* amqp_messenger_clone_option(const char* name, const void* value)
{
	void* result;

	if (name == NULL || value == NULL)
	{
		LogError("invalid argument (name=%p, value=%p)", name, value);
		result = NULL;
	}
	else
	{
		if (strcmp(MESSENGER_SAVED_MQ_OPTIONS, name) == 0)
		{
			if ((result = (void*)OptionHandler_Clone((OPTIONHANDLER_HANDLE)value)) == NULL)
			{
				LogError("failed cloning option '%s'", name);
			}
		}
		else
		{
			LogError("Failed to clone messenger option (option with name '%s' is not suppported)", name);
			result = NULL;
		}
	}

	return result;
}

static void amqp_messenger_destroy_option(const char* name, const void* value)
{
	if (name == NULL || value == NULL)
	{
		LogError("invalid argument (name=%p, value=%p)", name, value);
	}
	else if (strcmp(MESSENGER_SAVED_MQ_OPTIONS, name) == 0)
	{
		OptionHandler_Destroy((OPTIONHANDLER_HANDLE)value);
	}
	else
	{
		LogError("invalid argument (option '%s' is not suppported)", name);
	}
}


// Public API:

int amqp_messenger_subscribe_for_messages(AMQP_MESSENGER_HANDLE messenger_handle, ON_AMQP_MESSENGER_MESSAGE_RECEIVED on_message_received_callback, void* context)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_016: [If `messenger_handle` is NULL, amqp_messenger_subscribe_for_messages() shall fail and return __FAILURE__]
	if (messenger_handle == NULL || on_message_received_callback == NULL || context == NULL)
	{
		LogError("Invalid argument (messenger_handle=%p, on_message_received_callback=%p, context=%p)", messenger_handle, on_message_received_callback, context);
		result = __FAILURE__;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		instance->on_message_received_callback = on_message_received_callback;
		instance->on_message_received_context = context;
		instance->receive_messages = true;

		result = RESULT_OK;
	}

	return result;
}

int amqp_messenger_unsubscribe_for_messages(AMQP_MESSENGER_HANDLE messenger_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_023: [If `messenger_handle` is NULL, amqp_messenger_unsubscribe_for_messages() shall fail and return __FAILURE__]
	if (messenger_handle == NULL)
	{
		LogError("amqp_messenger_unsubscribe_for_messages failed (messenger_handle is NULL)");
		result = __FAILURE__;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		instance->receive_messages = false;
		instance->on_message_received_callback = NULL;
		instance->on_message_received_context = NULL;
			
		result = RESULT_OK;
	}

	return result;
}

int amqp_messenger_send_message_disposition(AMQP_MESSENGER_HANDLE messenger_handle, AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info, AMQP_MESSENGER_DISPOSITION_RESULT disposition_result)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_179: [If `messenger_handle` or `disposition_info` are NULL, amqp_messenger_send_message_disposition() shall fail and return __FAILURE__]  
	if (messenger_handle == NULL || disposition_info == NULL)
	{
		LogError("Failed sending message disposition (either messenger_handle (%p) or disposition_info (%p) are NULL)", messenger_handle, disposition_info);
		result = __FAILURE__;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_180: [If `disposition_info->source` is NULL, amqp_messenger_send_message_disposition() shall fail and return __FAILURE__]  
	else if (disposition_info->source == NULL)
	{
		LogError("Failed sending message disposition (disposition_info->source is NULL)");
		result = __FAILURE__;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* messenger = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_189: [If `messenger_handle->message_receiver` is NULL, amqp_messenger_send_message_disposition() shall fail and return __FAILURE__]
		if (messenger->message_receiver == NULL)
		{
			LogError("Failed sending message disposition (message_receiver is not created; check if it is subscribed)");
			result = __FAILURE__;
		}
		else
		{
			AMQP_VALUE uamqp_disposition_result;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_181: [An AMQP_VALUE disposition result shall be created corresponding to the `disposition_result` provided]
			if ((uamqp_disposition_result = create_uamqp_disposition_result_from(disposition_result)) == NULL)
			{
				LogError("Failed sending message disposition (disposition result %d is not supported)", disposition_result);
				result = __FAILURE__;
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_182: [`messagereceiver_send_message_disposition()` shall be invoked passing `disposition_info->source`, `disposition_info->message_id` and the corresponding AMQP_VALUE disposition result]  
				if (messagereceiver_send_message_disposition(messenger->message_receiver, disposition_info->source, disposition_info->message_id, uamqp_disposition_result) != RESULT_OK)
				{
					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_183: [If `messagereceiver_send_message_disposition()` fails, amqp_messenger_send_message_disposition() shall fail and return __FAILURE__]  
					LogError("Failed sending message disposition (messagereceiver_send_message_disposition failed)");
					result = __FAILURE__;
				}
				else
				{
					destroy_message_disposition_info(disposition_info);

					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_185: [If no failures occurr, amqp_messenger_send_message_disposition() shall return 0]  
					result = RESULT_OK;
				}

				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_184: [amqp_messenger_send_message_disposition() shall destroy the AMQP_VALUE disposition result]
				amqpvalue_destroy(uamqp_disposition_result);
			}
		}
	}

	return result;
}

static void on_message_processing_completed_callback(MQ_MESSAGE_HANDLE message, MESSAGE_QUEUE_RESULT result, USER_DEFINED_REASON reason, void* message_context)
{
	(void)reason;

	if (message_context == NULL)
	{
		LogError("on_message_processing_completed_callback invoked with NULL context");
	}
	else
	{
		MESSAGE_SEND_CONTEXT* msg_ctx = (MESSAGE_SEND_CONTEXT*)message_context;
		AMQP_MESSENGER_SEND_RESULT messenger_send_result;
		AMQP_MESSENGER_REASON messenger_send_reason;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_107: [If no failure occurs, `task->on_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_OK]  
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_108: [If a failure occurred, `task->on_send_complete_callback` shall be invoked with result EVENT_SEND_COMPLETE_RESULT_ERROR_FAIL_SENDING] 
		if (result == MESSAGE_QUEUE_SUCCESS)
		{
			messenger_send_result = AMQP_MESSENGER_SEND_RESULT_SUCCESS;
			messenger_send_reason = AMQP_MESSENGER_REASON_NONE;
		}
		else if (result == MESSAGE_QUEUE_TIMEOUT)
		{
			messenger_send_result = AMQP_MESSENGER_SEND_RESULT_ERROR;
			messenger_send_reason = AMQP_MESSENGER_REASON_TIMEOUT;
		}
		else if (result == MESSAGE_QUEUE_CANCELLED && msg_ctx->messenger->state == AMQP_MESSENGER_STATE_STOPPED)
		{
			messenger_send_result = AMQP_MESSENGER_SEND_RESULT_CANCELLED;
			messenger_send_reason = AMQP_MESSENGER_REASON_MESSENGER_DESTROYED;
		}
		else
		{
			msg_ctx->messenger->send_error_count++;

			messenger_send_result = AMQP_MESSENGER_SEND_RESULT_ERROR;
			messenger_send_reason = AMQP_MESSENGER_REASON_FAIL_SENDING;
		}

		if (msg_ctx->on_send_complete_callback != NULL)
		{
			msg_ctx->on_send_complete_callback(messenger_send_result, messenger_send_reason, msg_ctx->user_context);
		}

		if (!msg_ctx->is_destroyed)
		{
			message_destroy((MESSAGE_HANDLE)message);
		}

		destroy_message_send_context(msg_ctx);
	}
}

int amqp_messenger_send_async(AMQP_MESSENGER_HANDLE messenger_handle, MESSAGE_HANDLE message, AMQP_MESSENGER_SEND_COMPLETE_CALLBACK on_user_defined_send_complete_callback, void* user_context)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_134: [If `messenger_handle` is NULL, amqp_messenger_send_async() shall fail and return a non-zero value]  
	if (messenger_handle == NULL || message == NULL || on_user_defined_send_complete_callback == NULL)
	{
		LogError("invalid argument (messenger_handle=%p, message=%p, on_user_defined_send_complete_callback=%p)", messenger_handle, message, on_user_defined_send_complete_callback);
		result = __FAILURE__;
	}
	else
	{
		MESSAGE_HANDLE cloned_message;

		if ((cloned_message = message_clone(message)) == NULL)
		{
			LogError("Failed cloning AMQP message");
			result = __FAILURE__;
		}
		else
		{
			MESSAGE_SEND_CONTEXT* message_context;
			AMQP_MESSENGER_INSTANCE *instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

			if ((message_context = create_message_send_context()) == NULL)
			{
				LogError("Failed creating context for sending message");
				message_destroy(cloned_message);
				result = __FAILURE__;
			}
			else
			{
				message_context->message = cloned_message;
				message_context->messenger = instance;
				message_context->on_send_complete_callback = on_user_defined_send_complete_callback;
				message_context->user_context = user_context;

				if (message_queue_add(instance->send_queue, (MQ_MESSAGE_HANDLE)cloned_message, on_message_processing_completed_callback, (void*)message_context) != RESULT_OK)
				{
					LogError("Failed adding message to send queue");
					destroy_message_send_context(message_context);
					message_destroy(cloned_message);
					result = __FAILURE__;
				}
				else
				{
					result = RESULT_OK;
				}
			}
		}
	}

	return result;
}

int amqp_messenger_get_send_status(AMQP_MESSENGER_HANDLE messenger_handle, AMQP_MESSENGER_SEND_STATUS* send_status)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_144: [If `messenger_handle` is NULL, amqp_messenger_get_send_status() shall fail and return a non-zero value] 
	if (messenger_handle == NULL || send_status == NULL)
	{
		LogError("invalid argument (messenger_handle=%p, send_status=%p)", messenger_handle, send_status);
		result = __FAILURE__;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;
		bool is_empty;

		if (message_queue_is_empty(instance->send_queue, &is_empty) != 0)
		{
			LogError("Failed verifying if send queue is empty");
			result = __FAILURE__;
		}
		else
		{
			*send_status = (is_empty ? AMQP_MESSENGER_SEND_STATUS_IDLE : AMQP_MESSENGER_SEND_STATUS_BUSY);
			result = RESULT_OK;
		}
	}

	return result;
}

int amqp_messenger_start(AMQP_MESSENGER_HANDLE messenger_handle, SESSION_HANDLE session_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_029: [If `messenger_handle` is NULL, amqp_messenger_start() shall fail and return __FAILURE__]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("amqp_messenger_start failed (messenger_handle is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_030: [If `session_handle` is NULL, amqp_messenger_start() shall fail and return __FAILURE__]
	else if (session_handle == NULL)
	{
		result = __FAILURE__;
		LogError("amqp_messenger_start failed (session_handle is NULL)");
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_031: [If `instance->state` is not AMQP_MESSENGER_STATE_STOPPED, amqp_messenger_start() shall fail and return __FAILURE__]
		if (instance->state != AMQP_MESSENGER_STATE_STOPPED)
		{
			result = __FAILURE__;
			LogError("amqp_messenger_start failed (current state is %d; expected AMQP_MESSENGER_STATE_STOPPED)", instance->state);
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_032: [`session_handle` shall be saved on `instance->session_handle`]
			instance->session_handle = session_handle;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_115: [If no failures occurr, `instance->state` shall be set to AMQP_MESSENGER_STATE_STARTING, and `instance->on_state_changed_callback` invoked if provided]
			update_messenger_state(instance, AMQP_MESSENGER_STATE_STARTING);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_056: [If no failures occurr, amqp_messenger_start() shall return 0]
			result = RESULT_OK;
		}
	}

	return result;
}

int amqp_messenger_stop(AMQP_MESSENGER_HANDLE messenger_handle)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_057: [If `messenger_handle` is NULL, amqp_messenger_stop() shall fail and return a non-zero value]
	if (messenger_handle == NULL)
	{
		result = __FAILURE__;
		LogError("amqp_messenger_stop failed (messenger_handle is NULL)");
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_058: [If `instance->state` is AMQP_MESSENGER_STATE_STOPPED, amqp_messenger_stop() shall fail and return a non-zero value]
		if (instance->state == AMQP_MESSENGER_STATE_STOPPED)
		{
			result = __FAILURE__;
			LogError("amqp_messenger_stop failed (messenger is already stopped)");
		}
		else
		{
			update_messenger_state(instance, AMQP_MESSENGER_STATE_STOPPING);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_152: [amqp_messenger_stop() shall close and destroy `instance->message_sender` and `instance->message_receiver`]  
			destroy_message_sender(instance);
			destroy_message_receiver(instance);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_162: [amqp_messenger_stop() shall move all items from `instance->in_progress_list` to the beginning of `instance->wait_to_send_list`]
			if (message_queue_move_all_back_to_pending(instance->send_queue) != RESULT_OK)
			{
				LogError("Messenger failed to move events in progress back to wait_to_send list");
				
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_163: [If not all items from `instance->in_progress_list` can be moved back to `instance->wait_to_send_list`, `instance->state` shall be set to AMQP_MESSENGER_STATE_ERROR, and `instance->on_state_changed_callback` invoked]
				update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				result = __FAILURE__;
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_164: [If all items get successfuly moved back to `instance->wait_to_send_list`, `instance->state` shall be set to AMQP_MESSENGER_STATE_STOPPED, and `instance->on_state_changed_callback` invoked]
				update_messenger_state(instance, AMQP_MESSENGER_STATE_STOPPED);
				result = RESULT_OK;
			}
		}
	}

	return result;
}

// @brief
//     Sets the messenger module state based on the state changes from messagesender and messagereceiver
static void process_state_changes(AMQP_MESSENGER_INSTANCE* instance)
{
	// Note: messagesender and messagereceiver are still not created or already destroyed 
	//       when state is AMQP_MESSENGER_STATE_STOPPED, so no checking is needed there.

	if (instance->state == AMQP_MESSENGER_STATE_STARTED)
	{
		if (instance->message_sender_current_state != MESSAGE_SENDER_STATE_OPEN)
		{
			LogError("messagesender reported unexpected state %d while messenger was started", instance->message_sender_current_state);
			update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
		}
		else if (instance->message_receiver != NULL && instance->message_receiver_current_state != MESSAGE_RECEIVER_STATE_OPEN)
		{
			if (instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_OPENING)
			{
				bool is_timed_out;
				if (is_timeout_reached(instance->last_message_receiver_state_change_time, MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS, &is_timed_out) != RESULT_OK)
				{
					LogError("messenger got an error (failed to verify messagereceiver start timeout)");
					update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				}
				else if (is_timed_out)
				{
					LogError("messenger got an error (messagereceiver failed to start within expected timeout (%d secs))", MAX_MESSAGE_RECEIVER_STATE_CHANGE_TIMEOUT_SECS);
					update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				}
			}
			else if (instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_ERROR ||
				instance->message_receiver_current_state == MESSAGE_RECEIVER_STATE_IDLE)
			{
				LogError("messagereceiver reported unexpected state %d while messenger is starting", instance->message_receiver_current_state);
				update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
			}
		}
	}
	else
	{
		if (instance->state == AMQP_MESSENGER_STATE_STARTING)
		{
			if (instance->message_sender_current_state == MESSAGE_SENDER_STATE_OPEN)
			{
				update_messenger_state(instance, AMQP_MESSENGER_STATE_STARTED);
			}
			else if (instance->message_sender_current_state == MESSAGE_SENDER_STATE_OPENING)
			{
				bool is_timed_out;
				if (is_timeout_reached(instance->last_message_sender_state_change_time, MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS, &is_timed_out) != RESULT_OK)
				{
					LogError("messenger failed to start (failed to verify messagesender start timeout)");
					update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				}
				else if (is_timed_out)
				{
					LogError("messenger failed to start (messagesender failed to start within expected timeout (%d secs))", MAX_MESSAGE_SENDER_STATE_CHANGE_TIMEOUT_SECS);
					update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				}
			}
			// For this module, the only valid scenario where messagesender state is IDLE is if 
			// the messagesender hasn't been created yet or already destroyed.
			else if ((instance->message_sender_current_state == MESSAGE_SENDER_STATE_ERROR) ||
				(instance->message_sender_current_state == MESSAGE_SENDER_STATE_CLOSING) ||
				(instance->message_sender_current_state == MESSAGE_SENDER_STATE_IDLE && instance->message_sender != NULL))
			{
				LogError("messagesender reported unexpected state %d while messenger is starting", instance->message_sender_current_state);
				update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
			}
		}
		// message sender and receiver are stopped/destroyed synchronously, so no need for state control.
	}
}

void amqp_messenger_do_work(AMQP_MESSENGER_HANDLE messenger_handle)
{
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_065: [If `messenger_handle` is NULL, amqp_messenger_do_work() shall fail and return]
	if (messenger_handle == NULL)
	{
		LogError("amqp_messenger_do_work failed (messenger_handle is NULL)");
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		process_state_changes(instance);

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_151: [If `instance->state` is AMQP_MESSENGER_STATE_STARTING, amqp_messenger_do_work() shall create and open `instance->message_sender`]
		if (instance->state == AMQP_MESSENGER_STATE_STARTING)
		{
			if (instance->message_sender == NULL)
			{
				if (create_message_sender(instance) != RESULT_OK)
				{
					update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
				}
			}
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_066: [If `instance->state` is not AMQP_MESSENGER_STATE_STARTED, amqp_messenger_do_work() shall return]
		else if (instance->state == AMQP_MESSENGER_STATE_STARTED)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_067: [If `instance->receive_messages` is true and `instance->message_receiver` is NULL, a message_receiver shall be created]
			if (instance->receive_messages == true &&
				instance->message_receiver == NULL &&
				create_message_receiver(instance) != RESULT_OK)
			{
				LogError("amqp_messenger_do_work warning (failed creating the message receiver [%s])", instance->config->device_id);
			}
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_092: [If `instance->receive_messages` is false and `instance->message_receiver` is not NULL, it shall be destroyed]
			else if (instance->receive_messages == false && instance->message_receiver != NULL)
			{
				destroy_message_receiver(instance);
			}

			message_queue_do_work(instance->send_queue);

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_161: [If amqp_messenger_do_work() fail sending events for `instance->event_send_retry_limit` times in a row, it shall invoke `instance->on_state_changed_callback`, if provided, with error code AMQP_MESSENGER_STATE_ERROR]
			if (instance->send_error_count >= instance->max_send_error_count)
			{
				LogError("amqp_messenger_do_work failed (failed sending events; reached max number of consecutive failures)");
				update_messenger_state(instance, AMQP_MESSENGER_STATE_ERROR);
			}
		}
	}
}

void amqp_messenger_destroy(AMQP_MESSENGER_HANDLE messenger_handle)
{
	if (messenger_handle == NULL)
	{
		LogError("invalid argument (messenger_handle is NULL)");
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		if (instance->state != AMQP_MESSENGER_STATE_STOPPED)
		{
			(void)amqp_messenger_stop(messenger_handle);
		}

		if (instance->send_queue != NULL)
		{
			message_queue_destroy(instance->send_queue);
		}

		destroy_configuration(instance->config);

		free((void*)instance);
	}
}

AMQP_MESSENGER_HANDLE amqp_messenger_create(const AMQP_MESSENGER_CONFIG* messenger_config)
{
	AMQP_MESSENGER_HANDLE handle;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_001: [If parameter `messenger_config` is NULL, amqp_messenger_create() shall return NULL]
	if (!is_valid_configuration(messenger_config))
	{
		handle = NULL;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_006: [amqp_messenger_create() shall allocate memory for the messenger instance structure (aka `instance`)]
		if ((instance = (AMQP_MESSENGER_INSTANCE*)malloc(sizeof(AMQP_MESSENGER_INSTANCE))) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_007: [If malloc() fails, amqp_messenger_create() shall fail and return NULL]
			LogError("Failed allocating AMQP_MESSENGER_INSTANCE");
			handle = NULL;
		}
		else
		{
			memset(instance, 0, sizeof(AMQP_MESSENGER_INSTANCE));

			if ((instance->config = clone_configuration(messenger_config)) == NULL)
			{
				LogError("Failed copying AMQP messenger configuration");
				handle = NULL;
			}
			else
			{
				MESSAGE_QUEUE_CONFIG mq_config;
				mq_config.max_retry_count = DEFAULT_EVENT_SEND_RETRY_LIMIT;
				mq_config.max_message_enqueued_time_secs = DEFAULT_EVENT_SEND_TIMEOUT_SECS;
				mq_config.max_message_processing_time_secs = 0;
				mq_config.on_process_message_callback = on_process_message_callback;

				if ((instance->send_queue = message_queue_create(&mq_config)) == NULL)
				{
					// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_133: [If singlylinkedlist_create() fails, amqp_messenger_create() shall fail and return NULL] 
					LogError("Failed creating message queue");
					handle = NULL;
				}
				else
				{
					instance->state = AMQP_MESSENGER_STATE_STOPPED;
					instance->message_sender_current_state = MESSAGE_SENDER_STATE_IDLE;
					instance->message_sender_previous_state = MESSAGE_SENDER_STATE_IDLE;
					instance->message_receiver_current_state = MESSAGE_RECEIVER_STATE_IDLE;
					instance->message_receiver_previous_state = MESSAGE_RECEIVER_STATE_IDLE;
					instance->last_message_sender_state_change_time = INDEFINITE_TIME;
					instance->last_message_receiver_state_change_time = INDEFINITE_TIME;
					instance->max_send_error_count = DEFAULT_MAX_SEND_ERROR_COUNT;
					instance->on_state_changed_callback = messenger_config->on_state_changed_callback;
					instance->on_state_changed_context = messenger_config->on_state_changed_context;
					instance->receive_messages = false;

					handle = (AMQP_MESSENGER_HANDLE)instance;
				}
			}
		}

		if (handle == NULL)
		{
			amqp_messenger_destroy((AMQP_MESSENGER_HANDLE)instance);
		}
	}

	return handle;
}

int amqp_messenger_set_option(AMQP_MESSENGER_HANDLE messenger_handle, const char* name, void* value)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_167: [If `messenger_handle` or `name` or `value` is NULL, amqp_messenger_set_option shall fail and return a non-zero value]
	if (messenger_handle == NULL || name == NULL || value == NULL)
	{
		LogError("invalid argument (one of the followin are NULL: messenger_handle=%p, name=%p, value=%p)",
			messenger_handle, name, value);
		result = __FAILURE__;
	}
	else
	{
		AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_168: [If name matches MESSENGER_SAVED_MQ_OPTIONS, `value` shall be saved on `instance->event_send_timeout_secs`]
		if (strcmp(MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS, name) == 0)
		{
			if (message_queue_set_max_message_enqueued_time_secs(instance->send_queue, *(size_t*)value) == 0)
			{
				LogError("Failed setting option %s", MESSENGER_OPTION_EVENT_SEND_TIMEOUT_SECS);
				result = __FAILURE__;
			}
			else
			{
				result = RESULT_OK;
			}
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_171: [If name does not match any supported option, authentication_set_option shall fail and return a non-zero value]
			LogError("invalid argument (option with name '%s' is not suppported)", name);
			result = __FAILURE__;
		}
	}

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_172: [If no errors occur, amqp_messenger_set_option shall return 0]
	return result;
}

OPTIONHANDLER_HANDLE amqp_messenger_retrieve_options(AMQP_MESSENGER_HANDLE messenger_handle)
{
	OPTIONHANDLER_HANDLE result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_173: [If `messenger_handle` is NULL, amqp_messenger_retrieve_options shall fail and return NULL]
	if (messenger_handle == NULL)
	{
		LogError("Failed to retrieve options from messenger instance (messenger_handle is NULL)");
		result = NULL;
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_174: [An OPTIONHANDLER_HANDLE instance shall be created using OptionHandler_Create]
		result = OptionHandler_Create(amqp_messenger_clone_option, amqp_messenger_destroy_option, (pfSetOption)amqp_messenger_set_option);

		if (result == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_175: [If an OPTIONHANDLER_HANDLE instance fails to be created, amqp_messenger_retrieve_options shall fail and return NULL]
			LogError("Failed to retrieve options from messenger instance (OptionHandler_Create failed)");
		}
		else
		{
			AMQP_MESSENGER_INSTANCE* instance = (AMQP_MESSENGER_INSTANCE*)messenger_handle;
			OPTIONHANDLER_HANDLE mq_options;

			if ((mq_options = message_queue_retrieve_options(instance->send_queue)) == NULL)
			{
				LogError("failed to retrieve options from send queue)");
				OptionHandler_Destroy(result);
				result = NULL;
			}
			else if (OptionHandler_AddOption(result, MESSENGER_SAVED_MQ_OPTIONS, (void*)mq_options) != OPTIONHANDLER_OK)
			{
				LogError("failed adding option '%s'", MESSENGER_SAVED_MQ_OPTIONS);
				OptionHandler_Destroy(mq_options);
				OptionHandler_Destroy(result);
				result = NULL;
			}
		}
	}

	return result;
}

void amqp_messenger_destroy_disposition_info(AMQP_MESSENGER_MESSAGE_DISPOSITION_INFO* disposition_info)
{
	if (disposition_info == NULL)
	{
		LogError("Invalid argument (disposition_info is NULL)");
	}
	else
	{
		destroy_message_disposition_info(disposition_info);
	}
}
