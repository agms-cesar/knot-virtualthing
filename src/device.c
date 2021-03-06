/**
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2020, CESAR. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

/**
 *  Device source file
 */

#include <string.h>
#include <stdbool.h>
#include <knot/knot_protocol.h>
#include <knot/knot_types.h>
#include <knot/knot_cloud.h>
#include <ell/ell.h>
#include <stdio.h>
#include <errno.h>

#include "storage.h"
#include "settings.h"
#include "conf-parameters.h"
#include "device.h"
#include "device-pvt.h"
#include "iface-modbus.h"
#include "sm.h"
#include "knot-config.h"
#include "poll.h"
#include "properties.h"

#define CONNECTED_MASK		0xFF
#define set_conn_bitmask(a, b1, b2) (a) ? (b1) | (b2) : (b1) & ~(b2)
#define DEFAULT_POLLING_INTERVAL 1

enum CONN_TYPE {
	MODBUS = 0x0F,
	CLOUD = 0xF0
};

struct modbus_slave {
	int id;
	char *url;
};

struct modbus_source {
	int reg_addr;
	int bit_offset;
};

struct knot_data_item {
	int sensor_id;
	knot_config config;
	knot_schema schema;
	knot_value_type current_val;
	knot_value_type sent_val;
	struct modbus_source modbus_source;
};

struct knot_thing {
	char token[KNOT_PROTOCOL_TOKEN_LEN + 1];
	char id[KNOT_PROTOCOL_UUID_LEN + 1];
	char name[KNOT_PROTOCOL_DEVICE_NAME_LEN];
	char *user_token;

	struct modbus_slave modbus_slave;
	char *rabbitmq_url;
	char *credentials_path;

	struct l_hashmap *data_items;

	struct l_timeout *msg_to;
};

struct knot_thing thing;

static void knot_thing_destroy(struct knot_thing *thing)
{
	if (thing->msg_to)
		l_timeout_remove(thing->msg_to);

	l_free(thing->user_token);
	l_free(thing->rabbitmq_url);
	l_free(thing->modbus_slave.url);
	l_free(thing->credentials_path);

	l_hashmap_destroy(thing->data_items, l_free);
}

static void foreach_send_schema(const void *key, void *value, void *user_data)
{
	struct knot_data_item *data_item = value;
	struct l_queue *schema_queue = user_data;
	knot_msg_schema schema_aux;

	schema_aux.sensor_id = data_item->sensor_id;
	schema_aux.values = data_item->schema;
	l_queue_push_head(schema_queue, l_memdup(&schema_aux,
						 sizeof(knot_msg_schema)));
}

static void on_publish_data(void *data, void *user_data)
{
	struct knot_data_item *data_item;
	int *sensor_id = data;
	int rc;

	data_item = l_hashmap_lookup(thing.data_items,
				     L_INT_TO_PTR(*sensor_id));
	if (!data_item)
		return;

	rc = knot_cloud_publish_data(thing.id, data_item->sensor_id,
				     data_item->schema.value_type,
				     &data_item->current_val,
				     sizeof(data_item->schema.value_type));
	if (rc < 0)
		l_error("Couldn't send data_update for data_item #%d",
			*sensor_id);
}

static void foreach_publish_all_data(const void *key, void *value,
				     void *user_data)
{
	struct knot_data_item *data_item = value;

	on_publish_data(&data_item->sensor_id, NULL);
}

static void on_msg_timeout(struct l_timeout *timeout, void *user_data)
{
	sm_input_event(EVT_TIMEOUT, user_data);
}

static void foreach_config_add_data_item(const void *key, void *value,
					 void *user_data)
{
	struct knot_data_item *data_item = value;

	config_add_data_item(data_item->sensor_id, data_item->config);
}

static void on_config_timeout(int id)
{
	struct l_queue *list;

	list = l_queue_new();
	l_queue_push_head(list, &id);

	sm_input_event(EVT_PUB_DATA, list);

	l_queue_destroy(list, NULL);
}

static bool on_cloud_receive(const struct knot_cloud_msg *msg, void *user_data)
{
	switch (msg->type) {
	case UPDATE_MSG:
		if (!msg->error)
			sm_input_event(EVT_DATA_UPDT, msg->list);
		break;
	case REQUEST_MSG:
		if (!msg->error)
			sm_input_event(EVT_PUB_DATA, msg->list);
		break;
	case REGISTER_MSG:
		if (msg->error)
			sm_input_event(EVT_REG_NOT_OK, NULL);
		else
			sm_input_event(EVT_REG_OK, (char *) msg->token);
		break;
	case UNREGISTER_MSG:
		if (!msg->error)
			sm_input_event(EVT_UNREG_REQ, NULL);
		break;
	case AUTH_MSG:
		if (msg->error)
			sm_input_event(EVT_AUTH_NOT_OK, NULL);
		else
			sm_input_event(EVT_AUTH_OK, NULL);
		break;
	case SCHEMA_MSG:
		if (msg->error)
			sm_input_event(EVT_SCH_NOT_OK, NULL);
		else
			sm_input_event(EVT_SCH_OK, NULL);
		break;
	case LIST_MSG:
	case MSG_TYPES_LENGTH:
	default:
		return true;
	}

	return true;
}

static void conn_handler(enum CONN_TYPE conn, bool is_up)
{
	static uint8_t conn_mask;

	conn_mask = set_conn_bitmask(is_up, conn_mask, conn);

	if (conn_mask != CONNECTED_MASK) {
		sm_input_event(EVT_NOT_READY, NULL);
		return;
	}

	sm_input_event(EVT_READY, NULL);
}

static void on_cloud_disconnected(void *user_data)
{
	l_info("Disconnected from Cloud");

	conn_handler(CLOUD, false);
}

static void on_cloud_connected(void *user_data)
{
	l_info("Connected to Cloud %s", thing.rabbitmq_url);

	conn_handler(CLOUD, true);
}

static void on_modbus_disconnected(void *user_data)
{
	l_info("Disconnected from Modbus");

	poll_stop();
	conn_handler(MODBUS, false);
}

static void on_modbus_connected(void *user_data)
{
	l_info("Connected to Modbus %s", thing.modbus_slave.url);

	poll_start();
	conn_handler(MODBUS, true);
}

static int on_modbus_poll_receive(int id)
{
	struct knot_data_item *data_item;
	struct l_queue *list;
	int rc;

	data_item = l_hashmap_lookup(thing.data_items, L_INT_TO_PTR(id));
	if (!data_item)
		return -EINVAL;

	rc = iface_modbus_read_data(data_item->modbus_source.reg_addr,
				    data_item->modbus_source.bit_offset,
				    &data_item->current_val);
	if (config_check_value(data_item->config,
			       data_item->current_val,
			       data_item->sent_val,
			       data_item->schema.value_type) > 0) {
		data_item->sent_val = data_item->current_val;
		list = l_queue_new();
		l_queue_push_head(list, &id);

		sm_input_event(EVT_PUB_DATA, list);

		l_queue_destroy(list, NULL);
	}

	return rc;
}

static void foreach_data_item_polling(const void *key, void *value,
				      void *user_data)
{
	struct knot_data_item *data_item = value;
	int *rc = user_data;

	if (poll_create(DEFAULT_POLLING_INTERVAL, data_item->sensor_id,
			on_modbus_poll_receive)) {
		l_error("Fail on create poll to read data item with id: %d",
			data_item->sensor_id);
		*rc = -1;
	}
}

static int create_data_item_polling(void)
{
	int rc = 0;

	l_hashmap_foreach(thing.data_items, foreach_data_item_polling,
			  &rc);
	if (rc)
		poll_destroy();

	return rc;
}

char *device_get_id(void)
{
	return thing.id;
}

void device_set_thing_name(struct knot_thing *thing, const char *name)
{
	strcpy(thing->name, name);
}

void device_set_thing_user_token(struct knot_thing *thing, char *token)
{
	thing->user_token = token;
}

void device_set_thing_modbus_slave(struct knot_thing *thing, int slave_id,
				   char *url)
{
	thing->modbus_slave.id = slave_id;
	thing->modbus_slave.url = url;
}

void device_set_new_data_item(struct knot_thing *thing, int sensor_id,
			      knot_schema schema, knot_config config,
			      int reg_addr, int bit_offset)
{
	struct knot_data_item *data_item_aux;

	data_item_aux = l_new(struct knot_data_item, 1);
	data_item_aux->sensor_id = sensor_id;
	data_item_aux->schema = schema;
	data_item_aux->config = config;
	data_item_aux->modbus_source.reg_addr = reg_addr;
	data_item_aux->modbus_source.bit_offset = bit_offset;

	l_hashmap_insert(thing->data_items,
			 L_INT_TO_PTR(data_item_aux->sensor_id),
			 data_item_aux);
}

void *device_data_item_lookup(struct knot_thing *thing, int sensor_id)
{
	return l_hashmap_lookup(thing->data_items, L_INT_TO_PTR(sensor_id));
}

void device_set_thing_rabbitmq_url(struct knot_thing *thing, char *url)
{
	thing->rabbitmq_url = url;
}

void device_set_thing_credentials(struct knot_thing *thing, const char *id,
				  const char *token)
{
	strncpy(thing->id, id, KNOT_PROTOCOL_UUID_LEN);
	strncpy(thing->token, token, KNOT_PROTOCOL_TOKEN_LEN);
}

void device_set_thing_credentials_path(struct knot_thing *thing,
				       const char *path)
{
	thing->credentials_path = l_strdup(path);
}

void device_generate_thing_id(void)
{
	uint64_t id; /* knot id uses 16 characters which fits inside a uint64 */

	l_getrandom(&id, sizeof(id));
	sprintf(thing.id, "%"PRIx64, id); /* PRIx64 formats the string as hex */
}

void device_clear_thing_id(struct knot_thing *thing)
{
	thing->id[0] = '\0';
}

void device_clear_thing_token(struct knot_thing *thing)
{
	thing->token[0] = '\0';
}

int device_has_thing_token(void)
{
	return thing.token[0] != '\0';
}

int device_store_credentials_on_file(char *token)
{
	int rc;

	rc = properties_store_credentials(&thing, thing.credentials_path,
					  thing.id, token);
	if (rc < 0)
		return -1;

	strncpy(thing.token, token, KNOT_PROTOCOL_TOKEN_LEN);

	return 0;
}

int device_clear_credentials_on_file(void)
{
	return properties_clear_credentials(&thing, thing.credentials_path);
}

int device_check_schema_change(void)
{
	/* TODO: Add schema change verification */
	return 1;
}

int device_send_register_request(void)
{
	return knot_cloud_register_device(thing.id, thing.name);
}

int device_send_auth_request(void)
{
	return knot_cloud_auth_device(thing.id, thing.token);
}

int device_send_schema(void)
{
	struct l_queue *schema_queue;
	int rc;

	schema_queue = l_queue_new();

	l_hashmap_foreach(thing.data_items, foreach_send_schema, schema_queue);

	rc = knot_cloud_update_schema(thing.id, schema_queue);

	l_queue_destroy(schema_queue, l_free);

	return rc;
}

void device_publish_data_list(struct l_queue *sensor_id_list)
{
	l_queue_foreach(sensor_id_list, on_publish_data, NULL);
}

void device_publish_data_all(void)
{
	l_hashmap_foreach(thing.data_items, foreach_publish_all_data, NULL);
}

void device_msg_timeout_create(int seconds)
{
	if (thing.msg_to)
		return;

	thing.msg_to = l_timeout_create(seconds, on_msg_timeout, NULL, NULL);
}

void device_msg_timeout_modify(int seconds)
{
	l_timeout_modify(thing.msg_to, seconds);
}

void device_msg_timeout_remove(void)
{
	l_timeout_remove(thing.msg_to);
	thing.msg_to = NULL;
}

int device_start_config(void)
{
	int rc;

	rc = config_start(on_config_timeout);
	if (rc < 0) {
		l_error("Failed to start config");
		return rc;
	}

	l_hashmap_foreach(thing.data_items, foreach_config_add_data_item, NULL);

	return 0;
}

void device_stop_config(void)
{
	config_stop();
}

int device_start_read_cloud(void)
{
	return knot_cloud_read_start(thing.id, on_cloud_receive, NULL);
}

int device_start(struct device_settings *conf_files)
{
	int err;

	thing.data_items = l_hashmap_new();
	if (properties_create_device(&thing, conf_files)) {
		l_error("Failed to set device properties");
		return -EINVAL;
	}

	sm_start();

	err = create_data_item_polling();
	if (err < 0) {
		l_error("Failed to create the device polling");
		knot_thing_destroy(&thing);
		return err;
	}

	err = iface_modbus_start(thing.modbus_slave.url, thing.modbus_slave.id,
				 on_modbus_connected, on_modbus_disconnected,
				 NULL);
	if (err < 0) {
		l_error("Failed to initialize Modbus");
		poll_destroy();
		knot_thing_destroy(&thing);
		return err;
	}

	err = knot_cloud_start(thing.rabbitmq_url, thing.user_token,
			       on_cloud_connected, on_cloud_disconnected, NULL);
	if (err < 0) {
		l_error("Failed to initialize Cloud");
		poll_destroy();
		iface_modbus_stop();
		knot_thing_destroy(&thing);
		return err;
	}

	l_info("Device \"%s\" has started successfully", thing.name);

	return 0;
}

void device_destroy(void)
{
	config_stop();

	poll_destroy();
	knot_cloud_stop();
	iface_modbus_stop();

	knot_thing_destroy(&thing);
}
