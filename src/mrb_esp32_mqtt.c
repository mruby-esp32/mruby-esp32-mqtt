#include <mruby.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/string.h>
#include <mruby/array.h>

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "mqtt_client.h"


#define TAG ("mruby-esp32-mqtt")
#define WAIT_EVENT_TIMEOUT_SEC (20)
#define WAIT_EVENT_QUEUE_LEN (10)

static void mrb_mqtt_client_free(mrb_state *mrb, void *p);

static const struct mrb_data_type mrb_mqtt_client = {
  "mrb_mruby_esp32_mqtt_client", mrb_mqtt_client_free
};

typedef struct mqtt_client_t {
  mrb_state *mrb;
  char* host;
  mrb_int port;
  mrb_bool ssl;
  esp_mqtt_client_handle_t client;
  QueueHandle_t queue;
  TaskHandle_t main_task_handle;
  mrb_value message_proc;  
} mqtt_client_t;

static void
mqtt_message_handler(mqtt_client_t *client, esp_mqtt_event_handle_t event) {
  // Suspend main task.
  vTaskSuspend(client->main_task_handle);
  int arena_index = mrb_gc_arena_save(client->mrb);

  // Check message_proc is a a proc?
  mrb_assert(mrb_type(client->message_proc) == MRB_TT_PROC);

  // Prep arguments to pass.
  mrb_value args[2];
  args[0] = mrb_str_new_static(client->mrb, event->topic, event->topic_len);
  args[1] = mrb_str_new_static(client->mrb, event->data,  event->data_len); 

  // Call message_proc.
  mrb_yield_argv(client->mrb, client->message_proc, 2, &args[0]);

  // Resume main task.
  mrb_gc_arena_restore(client->mrb, arena_index);
  vTaskResume(client->main_task_handle);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
  mqtt_client_t *client = (mqtt_client_t *)arg;
  esp_mqtt_event_handle_t event = event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
      ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
      xQueueSend(client->queue, event_data, (TickType_t)0);
      break;
  case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      xQueueSend(client->queue, event_data, (TickType_t)0);
      break;
  case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      xQueueSend(client->queue, event_data, (TickType_t)0);
      break;
  case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      xQueueSend(client->queue, event_data, (TickType_t)0);
      break;
  case MQTT_EVENT_PUBLISHED:
      ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
  case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      mqtt_message_handler(client, event);
      break;
  case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      break;
  default:
      ESP_LOGI(TAG, "Other event id:%d", event->event_id);
      break;
  }
}

static void
mqtt_wait_for_event(mrb_state *mrb, mrb_value self, int32_t event_id) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  esp_mqtt_event_t event;
  int wait_count;
  struct RClass* error_class;

  for(wait_count = 0 ; wait_count < WAIT_EVENT_TIMEOUT_SEC ; wait_count++) {
    if(xQueueReceive(client->queue, (void*)&event, (TickType_t)(1000 / portTICK_PERIOD_MS))) {
      if(event.event_id == event_id) return;
    }
  }
  error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::TimeoutError));
  mrb_raise(mrb, error_class, "Timeout wait for mqtt event.");
}

static void
mrb_mqtt_client_free(mrb_state *mrb, void *p) {
  mqtt_client_t *client = (mqtt_client_t *)p;

  esp_mqtt_client_stop(client->client);

  mrb_free(mrb, client->host);
  mrb_free(mrb, p);
}

static mrb_value
mrb_mqtt_client_initialize(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = mrb_malloc(mrb, sizeof(mqtt_client_t));

  mrb_value host;
  mrb_int port;
  mrb_value block;

  mrb_get_args(mrb, "Si&", &host, &port, &block);

  client->mrb = mrb;
  client->host = mrb_malloc(mrb, strlen(mrb_str_to_cstr(mrb, host)));
  strcpy(client->host, mrb_str_to_cstr(mrb, host));
  client->port = port;
  client->ssl = FALSE;
  client->queue = xQueueCreate(WAIT_EVENT_QUEUE_LEN, sizeof(esp_mqtt_event_t));

  // Save block given and main task for handling incoming messages.
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@message_proc"), block);
  client->message_proc = block;
  client->main_task_handle = xTaskGetCurrentTaskHandle();

  mrb_data_init(self, client, &mrb_mqtt_client);
  ESP_LOGI(TAG, "initialize(%s, %d)", client->host, client->port);

  return self;
}

static mrb_value
mrb_mqtt_client_set_ssl(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);

  mrb_bool ssl;

  mrb_get_args(mrb, "b", &ssl);
  client->ssl = ssl;
  ESP_LOGI(TAG, "ssl=%s", client->ssl ? "true" : "false");
  
  return self;
}

static mrb_value
mrb_mqtt_client_connect(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  int ret = ESP_FAIL;
  struct RClass* error_class;

  mrb_value ca = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@ca"));
  mrb_value cert = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@cert"));
  mrb_value key = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@key"));

  esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
    .broker.address.hostname = client->host,
    .broker.address.port = client->port,
  };

  if(client->ssl)
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  if (!mrb_nil_p(ca))
    mqtt_cfg.broker.verification.certificate = mrb_str_to_cstr(mrb, ca);
  if (!mrb_nil_p(cert))
    mqtt_cfg.credentials.authentication.certificate = mrb_str_to_cstr(mrb, cert);
  if (!mrb_nil_p(key))
    mqtt_cfg.credentials.authentication.key = mrb_str_to_cstr(mrb, key);

  esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  ret = esp_mqtt_client_start(mqtt_client);
  if (ret != ESP_OK) {
    error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::ConnectError));
    mrb_raise(mrb, error_class, "Failed to connect.");
    return self;
  }
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, client));

  client->client = mqtt_client;

  mqtt_wait_for_event(mrb, self, MQTT_EVENT_CONNECTED);
  ESP_LOGI(
    TAG,
    "connect(%s://%s:%d)",
    client->ssl ? "mqtts" : "mqtt",
    client->host,
    client->port
  );

  return self;
}

static mrb_value
mrb_mqtt_client_publish(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  int ret = ESP_FAIL;
  struct RClass* error_class;

  mrb_value topic;
  mrb_value message;

  mrb_get_args(mrb, "SS", &topic, &message);

  ret = esp_mqtt_client_publish(
    client->client,
    mrb_str_to_cstr(mrb, topic),
    mrb_str_to_cstr(mrb, message),
    0, 0, 0
  );
  if (ret == ESP_FAIL) {
    error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::PublishError));
    mrb_raise(mrb, error_class, "Failed to publish.");
    return self;
  }
  ESP_LOGI(TAG, "publish(%s, %s)", mrb_str_to_cstr(mrb, topic), mrb_str_to_cstr(mrb, message));

  return self;
}

static mrb_value
mrb_mqtt_client_subscribe(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  int ret = ESP_FAIL;
  struct RClass* error_class;

  mrb_value topic;

  mrb_get_args(mrb, "S", &topic);

  ret = esp_mqtt_client_subscribe(
    client->client,
    mrb_str_to_cstr(mrb, topic),
    0
  );
  if (ret == ESP_FAIL) {
    error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::SubscribeError));
    mrb_raise(mrb, error_class, "Failed to subscribe.");
    return self;
  }
  mqtt_wait_for_event(mrb, self, MQTT_EVENT_SUBSCRIBED);
  ESP_LOGI(TAG, "subscribe(%s)", mrb_str_to_cstr(mrb, topic));

  return self;
}

static mrb_value
mrb_mqtt_client_unsubscribe(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  int ret = ESP_FAIL;
  struct RClass* error_class;

  mrb_value topic;

  mrb_get_args(mrb, "S", &topic);

  ret = esp_mqtt_client_unsubscribe(
    client->client,
    mrb_str_to_cstr(mrb, topic)
  );
  if (ret == ESP_FAIL) {
    error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::UnsubscribeError));
    mrb_raise(mrb, error_class, "Failed to unsubscribe.");
    return self;
  }
  mqtt_wait_for_event(mrb, self, MQTT_EVENT_UNSUBSCRIBED);
  ESP_LOGI(TAG, "unsubscribe(%s)", mrb_str_to_cstr(mrb, topic));

  return self;
}

static mrb_value
mrb_mqtt_client_disconnect(mrb_state *mrb, mrb_value self) {
  mqtt_client_t *client = (mqtt_client_t *) DATA_PTR(self);
  int ret = ESP_FAIL;
  struct RClass* error_class;

  ret = esp_mqtt_client_disconnect(client->client);
  if (ret != ESP_OK) {
    error_class = mrb_exc_get_id(mrb, MRB_ERROR_SYM(ESP32::MQTT::DisconnectError));
    mrb_raise(mrb, error_class, "Failed to disconnect.");
    return self;
  }
  mqtt_wait_for_event(mrb, self, MQTT_EVENT_DISCONNECTED);
  ESP_LOGI(TAG, "disconnect");
  
  return self;
}

void
mrb_mruby_esp32_mqtt_gem_init(mrb_state* mrb) {
  struct RClass *esp32_module = mrb_define_module(mrb, "ESP32");
  struct RClass *mqtt_module = mrb_define_module_under(mrb, esp32_module, "MQTT");
  struct RClass *client_class = mrb_define_class_under(mrb, mqtt_module, "Client", mrb->object_class);

  mrb_define_method(mrb, client_class, "_initialize", mrb_mqtt_client_initialize, MRB_ARGS_REQ(2)|MRB_ARGS_BLOCK());
  mrb_define_method(mrb, client_class, "ssl=", mrb_mqtt_client_set_ssl, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, client_class, "connect", mrb_mqtt_client_connect, MRB_ARGS_NONE());
  mrb_define_method(mrb, client_class, "publish", mrb_mqtt_client_publish, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, client_class, "_subscribe", mrb_mqtt_client_subscribe, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, client_class, "unsubscribe", mrb_mqtt_client_unsubscribe, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, client_class, "disconnect", mrb_mqtt_client_disconnect, MRB_ARGS_NONE());

  mrb_define_class_under(mrb, mqtt_module, "TimeoutError", mrb->eStandardError_class);
  mrb_define_class_under(mrb, mqtt_module, "ConnectError", mrb->eStandardError_class);
  mrb_define_class_under(mrb, mqtt_module, "PublishError", mrb->eStandardError_class);
  mrb_define_class_under(mrb, mqtt_module, "SubscribeError", mrb->eStandardError_class);
  mrb_define_class_under(mrb, mqtt_module, "UnsubscribeError", mrb->eStandardError_class);
  mrb_define_class_under(mrb, mqtt_module, "DisconnectError", mrb->eStandardError_class);
}

void
mrb_mruby_esp32_mqtt_gem_final(mrb_state* mrb) {
}
