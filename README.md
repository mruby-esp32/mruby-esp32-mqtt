# mruby-esp32-mqtt

MQTT library for mruby-esp32.

## Installation

You need [esp-idf v5.0](https://docs.espressif.com/projects/esp-idf/en/release-v5.0/esp32/index.html) to use this mrbgems.

Add the line below to your `build_config.rb`:

```ruby
  conf.gem :github => 'mruby-esp32/mruby-esp32-mqtt'
```

In addition, you may need to add `mqtt` to the component linking mruby.

```cmake
idf_component_register(
  # ...
  REQUIRES esp_wifi esp_hw_support esp_rom mqtt # <- add
)

add_prebuilt_library(
  # ...
  PRIV_REQUIRES esp_wifi esp_hw_support esp_rom mqtt # <- add
)
```

## Examples

Connect MQTT.

```ruby
mqtt = ESP32::MQTT::Client.new('test.mosquitto.org', 1883)
mqtt.connect
```

Connect MQTT + TLS.

```ruby
mqtt = ESP32::MQTT::Client.new('test.mosquitto.org', 8883)
mqtt.ssl = true
mqtt.ca = IO.read('root-ca.pem')
mqtt.cert = IO.read('certificate.pem.crt')
mqtt.key = IO.read('private.pem.key')
mqtt.connect
```

Publish message to topic.

```ruby
mqtt.publish("topic", 'message')
```

Subscribe to one or more topics to receive messages. Received messages are event driven. For each message, the main mruby task pauses temporarily, allowing the block given for the receiving topic to run.

```ruby
mqtt.subscribe("topic1") do |message|
  puts "Received from topic1: #{message}"
end

mqtt.on_message_from("topic2") do |message|
  puts "New message from topic2: #{message}"
end
mqtt.subscribe("topic2")

loop do
  # Do whatever in your main loop.
  ESP32::System.delay(1000)
end
```

Disconnect.

```ruby
mqtt.disconnect
```

## LICENSE

MIT License.
