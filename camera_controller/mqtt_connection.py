from mqtt_configuration import MqttConfiguration
from paho.mqtt import client as mqtt_client
from logging import getLogger

class MqttConnection:
    def __init__(self, mqtt_configuration: MqttConfiguration):
        self.mqtt_configuration = mqtt_configuration
        self.logger = getLogger('MqttConnection')
        self.subscriptions = {}
        self.client = None
        
    def __do_subscribe(self, subscription):
        self.logger.info(f'Subscribing to {subscription["topic"]}')

        self.client.subscribe(subscription['topic'])

    def __on_connect(self):
        def on_connect_curry(client, userdata, flags, rc):
            self.logger.info(f'on_connect')

            for topic, subscription in self.subscriptions.items():
                self.__do_subscribe(subscription)

        return on_connect_curry

    def __on_disconnect(self):
        def on_disconnect_curry(client, userdata, flags):
            self.logger.warn('on_disconnect')
        
            self.client.reconnect()

        return on_disconnect_curry

    def __on_message(self):
        def on_message_curry(client, userdata, message):
            payload_string = message.payload.decode('utf-8')

            self.logger.debug(f'on_message: {message.topic}: {payload_string}')

            subscription = self.subscriptions.get(message.topic)

            if subscription is None:
                self.logger.warn(f'Unknown topic: {message.topic}')
                return
            
            subscription['action'](message.topic, payload_string)

        return on_message_curry

        
    def __connect(self, broker, port, client_id, user, password):
        self.client = mqtt_client.Client(client_id)
        
        self.client.username_pw_set(user, password)

        self.client.on_connect = self.__on_connect()
        self.client.on_disconnect = self.__on_disconnect()
        self.client.on_message = self.__on_message()

        self.client.connect(broker, port)
    
    def connect(self):
        self.__connect(self.mqtt_configuration.broker, self.mqtt_configuration.broker_port, self.mqtt_configuration.client_id, self.mqtt_configuration.username, self.mqtt_configuration.password)

        self.client.loop_start()

    def subscribe(self, topic, action):
        full_topic = self.mqtt_configuration.topic + topic

        subscription = {
            'topic': full_topic,
            'action': action
        }

        self.subscriptions[full_topic] = subscription

        if self.client is not None and self.client.is_connected():
            self.__do_subscribe(subscription)

    def publish(self, topic, value):
        full_topic = self.mqtt_configuration.topic + topic

        self.logger.debug(f'publish: {full_topic}: {value}')

        self.client.publish(full_topic, value)

        # Ensure messages can be flushed immediately
        self.client.loop()