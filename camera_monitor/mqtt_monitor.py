from multiprocessing import Process, Queue
import logging
import common
import json
from mqtt_configuration import MqttConfiguration
from mqtt_connection import MqttConnection

from controller import Controller

class MqttMonitor(Process):
    def __init__(self, mqtt_configuration: MqttConfiguration, controller: Controller, file_moved_queue: Queue):
        Process.__init__(self)

        self.logger = common.get_logger('MqttMonitor')
        self.controller = controller

        self.mqtt_configuration = mqtt_configuration
        self.file_moved_queue = file_moved_queue
                            
    def record_message_curry(self):
        def record_message(userdata, payload):
            payload_obj = json.loads(payload)

            if bool(payload_obj.get('value')):
                self.logger.debug(f'recording_start: {payload_obj}')
                self.controller.recording_start(payload_obj)
            else:
                self.logger.debug(f'recording_stop')
                self.controller.recording_stop(payload_obj)
        
        return record_message

    def snapshot_message_curry(self):
        def snapshot_message(userdata, payload):
            payload_obj = json.loads(payload)

            self.logger.debug(f'snapshot: {payload_obj}')
            self.controller.snapshot(payload_obj)

        return snapshot_message

    def configure(self):
        self.logger = common.get_logger('MqttMonitor')

        self.mqtt_connection = MqttConnection(self.mqtt_configuration)

        self.logger.info(f'Subscribing to basic topics')

        self.mqtt_connection.subscribe('record', self.record_message_curry())
        self.mqtt_connection.subscribe('snapshot', self.snapshot_message_curry())

        self.mqtt_connection.connect()

    def run(self):
        self.configure()

        while True:
            file_move = self.file_moved_queue.get()

            self.mqtt_connection.publish('camera_monitor/file_moved', json.dumps(file_move))