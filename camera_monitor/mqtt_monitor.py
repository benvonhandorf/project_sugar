from multiprocessing import Process
import paho.mqtt.client as mqtt
import logging
import common
import json

from controller import Controller

class MqttConfiguration:
    def __init__(self, broker, username, password, topics):
        self.broker = broker
        self.broker_port = 1883
        self.username = username
        self.password = password
        self.topics = topics

class MqttMonitor(Process):
    def __init__(self, mqtt_configuration: MqttConfiguration, controller: Controller):
        Process.__init__(self)

        self.logger = common.get_logger()
        self.controller = controller

        self.mqtt_configuration = mqtt_configuration

    def configure(self):
        self.logger = common.get_logger('MqttMonitor')

        self.mqtt_client = mqtt.Client()

        self.mqtt_client.username_pw_set(self.mqtt_configuration.username, self.mqtt_configuration.password)

        self.mqtt_client.on_connect = self.on_connect()
        self.mqtt_client.on_message = self.on_message()
        self.mqtt_client.on_disconnect = self.on_disconnect()

        self.mqtt_client.reconnect_delay_set(min_delay=5, max_delay=60)
                 
    def on_connect(self):
        def on_connect_curry(client, userdata, flags, rc):
            self.logger.info(f'on_connect')

            for topic in self.mqtt_configuration.topics:
                self.logger.info(f'Subscribing to {topic}')

                self.mqtt_client.subscribe(topic)

        return on_connect_curry


    def on_disconnect(self):
        def on_disconnect_curry(client, userdata, flags):
            self.logger.warn('on_disconnect')
        
            self.mqtt_client.reconnect()

        return on_disconnect_curry

    def on_message(self):
        def on_message_curry(client, userdata, message):
            parts = message.topic.split('/')

            payload = message.payload.decode('utf-8')

            self.logger.debug(f'on_message: {message.topic}: {payload}')

            topic_end = parts.pop()

            if topic_end == 'record':
                payload_obj = json.loads(payload)

                if type(payload_obj) is not dict:
                    return

                if bool(payload_obj.get('value')):
                    self.logger.debug(f'recording_start: {payload_obj}')
                    self.controller.recording_start(payload_obj)
                else:
                    self.logger.debug(f'recording_stop')
                    self.controller.recording_stop(payload_obj)
            elif topic_end == 'snapshot':
                payload_obj = json.loads(payload)

                self.logger.debug(f'snapshot: {payload_obj}')
                self.controller.snapshot(payload_obj)

        return on_message_curry

    def run(self):
        self.configure()

        self.logger.info('connect')

        self.mqtt_client.connect(self.mqtt_configuration.broker, self.mqtt_configuration.broker_port)

        self.logger.info('loop')

        self.mqtt_client.loop_forever()