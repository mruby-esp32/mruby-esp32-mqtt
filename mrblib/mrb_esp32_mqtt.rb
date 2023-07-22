module ESP32
  module MQTT
    class Client
      attr_accessor :ca, :cert, :key

      def initialize(host, port)
        @callbacks = {}
        
        # C calls the block given here with every received message.
        self._initialize(host, port) do |topic, message|
          @callbacks[topic].call(message) if @callbacks[topic]
        end
      end

      def subscribe(topic, &block)
        @callbacks[topic] = block if block
        self._subscribe(topic)
      end

      def on_message_from(topic, &block)
        @callbacks[topic] = block if block
      end
    end
  end
end
