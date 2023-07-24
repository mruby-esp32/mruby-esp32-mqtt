module ESP32
  module MQTT
    class Client
      attr_accessor :ca, :cert, :key, :connected

      def initialize(host, port)
        self._initialize(host, port)
        
        @connected = false
        @connect_callbacks = []
        @message_callbacks = {}
        
        self.set_connected_handler do
          @connected = true
          @connect_callbacks.each { |cb| cb.call }
          @connect_callbacks = []
        end
        
        self.set_disconnected_handler do
          @connected = false
        end
        
        self.set_unsubscribed_handler do |topic|
          @message_callbacks[topic] = nil
        end
        
        # C calls this block with every received message.
        self.set_data_handler do |topic, message|
          @message_callbacks[topic].call(message) if @message_callbacks[topic]
        end
      end

      def subscribe(topic, &block)
        @message_callbacks[topic] = block if block

        # Take semaphore
        
        if @connected
          self._subscribe(topic)
        else
          self.on_connect do
            self._subscribe(topic)
          end
        end
        
        # Release semaphore
      end

      def on_connect(&block)
        @connect_callbacks << block
      end
      
      def on_message_from(topic, &block)
        @message_callbacks[topic] = block
      end
    end
  end
end
